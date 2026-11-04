/** @file
  v1awifi camera ISP bring-up test (shell application), milestone "A5
  alive", over the shared core (Drivers/Camera/FimcIsCore.c):
    1. register snapshot (what the bootloader left),
    2. ISP power domain, ISP dividers, clock gates,
    3. \fimc_is_fw2_IMX134.bin (stock /system/vendor/firmware) into a
       20 MiB, 64 MiB-aligned buffer (BBOAR = its physical address; the
       ISP SysMMU is off after reset, so the A5 sees physical memory),
    4. secure configuration through the secure monitor (SMC_CMD_REG),
    5. A5 power-on, then the first mailbox message: the firmware's
       IHC_GET_SENSOR_NUMBER (0x1000), param1 = firmware version,
    6. GIC pending / combiner raw status before and after, to identify the
       interrupt Windows will need,
    7. everything off.

    CameraTest.efi [nosmc] [topmux] [keep]
      nosmc   skip the secure-monitor writes
      topmux  also switch the ISP TOP user muxes (default: leave them)
      keep    leave the A5 running

  No sensor is powered: the sensors, CSIS and FIMC-Lite come after this.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/FileInfo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#include "../../Drivers/Camera/FimcIsHost.h"
#include "../../Drivers/Camera/CamBoard.h"
#include "../../Drivers/Camera/CamCapture.h"
#include "../../Drivers/Camera/CamSensorI2c.h"
#include "../../Drivers/Camera/CamAe.h"
#include <Protocol/ExynosI2c.h>

#define FW_FILE           L"\\fimc_is_fw2_IMX134.bin"
#define SETFILE_FILE      L"\\setfile_imx134.bin"
#define SETFILE_FRONT     L"\\setfile_6b2.bin"

STATIC BOOLEAN  mFront;
STATIC UINT32   mRawW = 3280;
STATIC UINT32   mRawH = 2458;
#define PMIC_BUS          7
#define PMIC_ADDR         0x09
#define FW_LOG_OFFSET     0x01340000u      // FIMC_IS_DEBUG_REGION_ADDR
#define FW_LOG_SIZE       0x0007D000u      // DEBUG_CNT; write offset (absolute) at FW_LOG_OFFSET + FW_LOG_SIZE

#define GICD_ISPENDR(n)   (0x10481200u + (n) * 4)
#define GICD_WORDS        9                 // IRQs 0..287
#define COMBINER_IESR(g)  (0x10440000u + ((g) / 4) * 0x10 + 0x0)
#define COMBINER_IECR(g)  (0x10440000u + ((g) / 4) * 0x10 + 0x4)
#define COMBINER_ISTR(g)  (0x10440000u + ((g) / 4) * 0x10 + 0x8)

#define SMC_CMD_REG       ((UINT32)-101)

#define MCU_INTMR1        (FIMC_IS_PHYS + 0x180024)
#define MCU_INTSR1        (FIMC_IS_PHYS + 0x180028)
#define MCU_INTMSR1       (FIMC_IS_PHYS + 0x18002C)
#define MCU_IFLAG         (FIMC_IS_PHYS + 0x180080 + 0x24)
#define SMC_SFR_W(a)      ((1u << 30) | ((a) >> 2))
#define SMC_SFR_R(a)      ((3u << 30) | ((a) >> 2))

VOID
EnvCacheClean (
  IN VOID   *Va,
  IN UINTN  Size
  )
{
  WriteBackDataCacheRange (Va, Size);
}

VOID
EnvCacheInvalidate (
  IN VOID   *Va,
  IN UINTN  Size
  )
{
  InvalidateDataCacheRange (Va, Size);
}

//
// exynos_smc() / exynos_smc_readsfr() (arch/arm/mach-exynos/smc.S).
//
STATIC
UINT32
Smc (
  IN     UINT32  A0,
  IN OUT UINT32  *A1,
  IN OUT UINT32  *A2,
  IN     UINT32  A3
  )
{
  register UINT32  R0 __asm__ ("r0") = A0;
  register UINT32  R1 __asm__ ("r1") = *A1;
  register UINT32  R2 __asm__ ("r2") = *A2;
  register UINT32  R3 __asm__ ("r3") = A3;

  __asm__ volatile (
    ".arch_extension sec\n\t"
    "dsb\n\t"
    "smc #0\n\t"
    "dsb\n\t"
    : "+r" (R0), "+r" (R1), "+r" (R2), "+r" (R3)
    :
    : "memory"
    );
  *A1 = R1;
  *A2 = R2;
  return R0;
}

STATIC UINT32  mFirstSmcResult  = ~0u;
STATIC UINT32  mLastReadStatus = ~0u;

STATIC
INTN
SecureWrite (
  UINT32  PhysAddr,
  UINT32  Value
  )
{
  UINT32  R1 = SMC_SFR_W (PhysAddr);
  UINT32  R0 = Smc (SMC_CMD_REG, &R1, &Value, 0);

  //
  // exynos_smc_readsfr(): the monitor leaves r0 = SMC_CMD_REG and puts
  // the status into r1 (0 = done).
  //
  if (R0 == SMC_CMD_REG) {
    R0 = R1;
  }

  if (mFirstSmcResult == ~0u) {
    mFirstSmcResult = R0;
  }
  return 0;
}

STATIC
INTN
SecureRead (
  UINT32  PhysAddr,
  UINT32  *Value
  )
{
  UINT32  R1 = SMC_SFR_R (PhysAddr);
  UINT32  R2 = 0;
  UINT32  R0 = Smc (SMC_CMD_REG, &R1, &R2, 0);

  if (R0 == SMC_CMD_REG) {
    R0 = R1;
  }
  mLastReadStatus = R0;
  if (R0 != 0) {
    return -1;
  }
  *Value = R2;
  return 0;
}

STATIC
EFI_STATUS
LoadFile (
  EFI_HANDLE    ImageHandle,
  CONST CHAR16  *Name,
  VOID          **Data,
  UINTN         *Size
  )
{
  EFI_LOADED_IMAGE_PROTOCOL        *Image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize;
  EFI_STATUS                       Status;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (Image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  }
  if (!EFI_ERROR (Status)) {
    Status = Fs->OpenVolume (Fs, &Root);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Root->Open (Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    InfoSize = SIZE_OF_EFI_FILE_INFO + 512;
    Info     = AllocatePool (InfoSize);
    Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
    if (!EFI_ERROR (Status)) {
      *Size  = (UINTN)Info->FileSize;
      *Data  = AllocatePool (*Size);
      Status = File->Read (File, Size, *Data);
    }
    FreePool (Info);
    File->Close (File);
  }
  Root->Close (Root);
  return Status;
}

STATIC
VOID
LogRegs (
  CONST CHAR8  *When,
  FIMC_IS      *Is
  )
{
  FIMC_IS_REGS  R;

  FimcIsSnapshot (Is, &R);
  DEBUG ((DEBUG_INFO, "CameraTest: %a: PD 0x%x/0x%x LPO 0x%08x A5 cfg 0x%x st 0x%x opt 0x%08x\n", When, R.PdConfig,
          R.PdStatus, R.LowPowerOff, R.ArmConfig, R.ArmStatus, R.ArmOption));
  DEBUG ((DEBUG_INFO, "CameraTest: %a: SRC_TOP1 %08x TOP3 %08x TOP4 %08x TOP10 %08x TOP11 %08x DIV_TOP0 %08x TOP1 %08x\n",
          When, R.SrcTop1, R.SrcTop3, R.SrcTop4, R.SrcTop10, R.SrcTop11, R.DivTop0, R.DivTop1));
  DEBUG ((DEBUG_INFO, "CameraTest: %a: DIV_ISP %08x %08x %08x GATE_ISP %08x %08x GSCL0 %08x MCUCTLR %08x BBOAR %08x "
          "INTMSR1 %08x ISSR6 %08x\n", When, R.DivIsp0, R.DivIsp1, R.DivIsp2, R.GateIsp0, R.GateIsp1, R.GateGscl0,
          R.Mcuctlr, R.Bboar, R.Intmsr1, R.Issr6));
}

STATIC
VOID
SnapIrq (
  UINT32  *Pend,
  UINT32  *Comb
  )
{
  UINTN  Index;

  for (Index = 0; Index < GICD_WORDS; Index++) {
    Pend[Index] = MmioRead32 (GICD_ISPENDR (Index));
  }
  *Comb = MmioRead32 (COMBINER_ISTR (19));
}

STATIC
VOID
LogIrqDiff (
  UINT32  *Before,
  UINT32  *After,
  UINT32  CombBefore,
  UINT32  CombAfter
  )
{
  UINTN   Index;
  UINT32  New;
  UINTN   Bit;

  for (Index = 1; Index < GICD_WORDS; Index++) {
    New = After[Index] & ~Before[Index];
    for (Bit = 0; Bit < 32; Bit++) {
      if ((New & (1u << Bit)) != 0) {
        DEBUG ((DEBUG_INFO, "CameraTest: newly pending: GIC ID %u (SPI %u)\n", Index * 32 + Bit, Index * 32 + Bit - 32));
      }
    }
  }
  // group 19 is byte 3 of its block: (19,1) = bit 25 ARMISP, (19,4) = bit 28 ISP
  DEBUG ((DEBUG_INFO, "CameraTest: combiner group 16..19 raw 0x%08x -> 0x%08x (group 19: 0x%02x -> 0x%02x)\n",
          CombBefore, CombAfter, CombBefore >> 24, CombAfter >> 24));
}

STATIC
VOID
LogFwTail (
  CONST UINT8  *Fw,
  UINTN        Size
  )
{
  CHAR8  Text[65];
  UINTN  Index;
  UINTN  Start = (Size > 64) ? Size - 64 : 0;

  for (Index = 0; Start + Index < Size; Index++) {
    Text[Index] = ((Fw[Start + Index] >= 0x20) && (Fw[Start + Index] < 0x7F)) ? (CHAR8)Fw[Start + Index] : '.';
  }
  Text[Index] = 0;
  DEBUG ((DEBUG_INFO, "CameraTest: firmware %u bytes, tail \"%a\"\n", Size, Text));
}


//
// ---------------------------------------------------------------------------
// Step 2: rear module power, HIC_OPEN_SENSOR, setfile.
// ---------------------------------------------------------------------------
//
STATIC EXYNOS_I2C_PROTOCOL  *mI2c;

STATIC
INTN
PmicRead (
  VOID   *Context,
  UINT8  Reg,
  UINT8  *Value
  )
{
  return (mI2c->Read (mI2c, PMIC_BUS, PMIC_ADDR, Reg, Value, 1) == 1) ? 0 : -1;
}

STATIC
INTN
PmicWrite (
  VOID   *Context,
  UINT8  Reg,
  UINT8  Value
  )
{
  UINT8  Buf[2] = { Reg, Value };

  return (mI2c->Write (mI2c, PMIC_BUS, PMIC_ADDR, Buf, 2) == 2) ? 0 : -1;
}

STATIC
VOID
LogBoard (
  CONST CHAR8  *When,
  CAM_BOARD    *Board
  )
{
  CAM_BOARD_REGS  R;

  CamBoardSnapshot (Board, &R);
  DEBUG ((DEBUG_INFO, "CameraTest: %a: GPE0 con %08x dat %02x GPF0 con %08x GPF1 con %08x dat %02x GPH0 con %08x "
          "SCLK_SRC_ISP %08x DIV %08x GATE %08x LDO18 0x%02x LDO24 0x%02x\n", When, R.Gpe0Con, R.Gpe0Dat, R.Gpf0Con,
          R.Gpf1Con, R.Gpf1Dat, R.Gph0Con, R.SclkSrcIsp, R.SclkDivIsp, R.GateTopSclkIsp, R.Ldo18, R.Ldo24));
}

STATIC
EFI_STATUS
SaveFile (
  EFI_HANDLE    ImageHandle,
  CONST CHAR16  *Name,
  VOID          *Data,
  UINTN         Size
  )
{
  EFI_LOADED_IMAGE_PROTOCOL        *Image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_STATUS                       Status;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (Image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  }
  if (!EFI_ERROR (Status)) {
    Status = Fs->OpenVolume (Fs, &Root);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Root->Open (Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (!EFI_ERROR (Status)) {
    File->Delete (File);   // drop an old one (closes it)
    Status = Root->Open (Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  }
  if (!EFI_ERROR (Status)) {
    Status = File->Write (File, &Size, Data);
    File->Close (File);
  }
  Root->Close (Root);
  return Status;
}


STATIC BOOLEAN  mI2cProbe;
STATIC BOOLEAN  mAe;
STATIC UINT32   mExpoLines;       // 0 = leave the sensor alone
STATIC UINT32   mAnalogGain;      // IMX134: code 0..224 (gain = 256 / (256 - code))
STATIC UINT32   mDigitalGain;     // 8.8 fixed point, 0x100 = 1x

STATIC
VOID
SensorExposure (
  VOID
  )
{
  CAM_I2C  I2c;
  UINT8    Addr;
  UINT8    V8 = 0;
  UINT16   V16, Frame, Line, Coarse, Again, Dgain;
  INTN     R;

  I2c.Gpio = CAM_GPIO_LB_PHYS;
  I2c.Bank = 0x40;                         // GPF0
  I2c.Sda  = mFront ? 4 : 0;
  I2c.Scl  = mFront ? 5 : 1;
  //
  // Find the sensor: the first address that answers a read of register 0.
  //
  for (Addr = 0x08; Addr < 0x78; Addr++) {
    I2c.Addr = Addr;
    if (CamI2cRead (&I2c, 0x0000, &V8) == 0) {
      break;
    }
  }
  if (Addr >= 0x78) {
    DEBUG ((DEBUG_ERROR, "CameraTest: i2c: no sensor answers on the ISP I2C pins\n"));
    return;
  }
  I2c.Addr = Addr;
  CamI2cRead16 (&I2c, 0x0000, &V16);
  DEBUG ((DEBUG_INFO, "CameraTest: i2c: sensor at 0x%02x, model id 0x%04x\n", Addr, V16));
  Frame = Line = Coarse = Again = Dgain = 0;
  CamI2cRead (&I2c, 0x0100, &V8);
  CamI2cRead16 (&I2c, 0x0340, &Frame);
  CamI2cRead16 (&I2c, 0x0342, &Line);
  CamI2cRead16 (&I2c, 0x0202, &Coarse);
  CamI2cRead16 (&I2c, 0x0204, &Again);
  CamI2cRead16 (&I2c, 0x020E, &Dgain);
  DEBUG ((DEBUG_INFO, "CameraTest: i2c: mode_select %u, frame_length %u, line_length %u, coarse integration %u, "
          "analog gain code 0x%04x, digital gain 0x%04x\n", V8, Frame, Line, Coarse, Again, Dgain));
  if (mExpoLines == 0) {
    return;
  }
  //
  // Hold, set exposure and gains, release (Sony / SMIA grouped parameter hold).
  //
  R  = CamI2cWrite (&I2c, 0x0104, 1);
  R |= CamI2cWrite16 (&I2c, 0x0202, (UINT16)mExpoLines);
  if (mAnalogGain != 0) {
    R |= CamI2cWrite16 (&I2c, 0x0204, (UINT16)mAnalogGain);
  }
  if (mDigitalGain != 0) {
    R |= CamI2cWrite16 (&I2c, 0x020E, (UINT16)mDigitalGain);
    R |= CamI2cWrite16 (&I2c, 0x0210, (UINT16)mDigitalGain);
    R |= CamI2cWrite16 (&I2c, 0x0212, (UINT16)mDigitalGain);
    R |= CamI2cWrite16 (&I2c, 0x0214, (UINT16)mDigitalGain);
  }
  R |= CamI2cWrite (&I2c, 0x0104, 0);
  CamI2cRead16 (&I2c, 0x0202, &Coarse);
  CamI2cRead16 (&I2c, 0x0204, &Again);
  DEBUG ((DEBUG_INFO, "CameraTest: i2c: set exposure %u lines, analog 0x%x, digital 0x%x (status %d); reads back %u / 0x%04x\n",
          mExpoLines, mAnalogGain, mDigitalGain, R, Coarse, Again));
}

//
// Mean of the green samples above black over a sparse grid of the raw frame.
//
STATIC
UINT32
RawMean (
  CONST UINT16  *Raw,
  UINT32        W,
  UINT32        H
  )
{
  UINT64  Sum = 0;
  UINT32  Count = 0;
  UINT32  X, Y;

  for (Y = 0; Y < 60; Y++) {
    for (X = 0; X < 80; X++) {
      UINT32  Px = ((X * W) / 80) & ~1u;
      UINT32  Py = ((Y * H) / 60) & ~1u;
      INT32   G  = (INT32)Raw[Py * W + Px + 1] - 65;

      Sum += (G < 0) ? 0 : (UINT32)G;
      Count++;
    }
  }
  return (UINT32)(Sum / Count);
}

//
// Closed loop: capture a frame, measure, set the sensor, repeat.
//
STATIC
VOID
AutoExposure (
  CAM_CAPTURE  *Cap,
  UINT16       *Buf,
  UINTN        Bytes
  )
{
  CAM_I2C  I2c;
  CAM_AE   Ae;
  UINT8    Addr;
  UINT8    V8;
  UINT16   Frame = 0;
  UINT32   Iter, Mean, Status, CsisSrc, Wait;
  UINT32   Ends;

  I2c.Gpio = CAM_GPIO_LB_PHYS;
  I2c.Bank = 0x40;
  I2c.Sda  = mFront ? 4 : 0;
  I2c.Scl  = mFront ? 5 : 1;
  for (Addr = 0x08; Addr < 0x78; Addr++) {
    I2c.Addr = Addr;
    if (CamI2cRead (&I2c, 0x0000, &V8) == 0) {
      break;
    }
  }
  if (Addr >= 0x78) {
    DEBUG ((DEBUG_ERROR, "CameraTest: ae: no sensor on I2C\n"));
    return;
  }
  I2c.Addr = Addr;
  CamI2cRead16 (&I2c, 0x0340, &Frame);
  CamAeInit (&Ae, (UINT32)Frame - 6, 150, mFront ? CAM_AE_S5K6B2 : CAM_AE_IMX134);
  DEBUG ((DEBUG_INFO, "CameraTest: ae: sensor 0x%02x, frame length %u, start %u lines\n", Addr, Frame, Ae.Lines));
  for (Iter = 0; Iter < 14; Iter++) {
    CamCaptureEnable (Cap);
    Status = 0;
    for (Wait = 0; Wait < 300000; Wait += 200) {
      Status = CamCaptureTakeStatus (Cap, &CsisSrc);
      if (Status & (1u << 4)) {
        break;
      }
      gBS->Stall (200);
    }
    CamCaptureDisable (Cap);
    InvalidateDataCacheRange (Buf, Bytes);
    Mean = RawMean (Buf, Cap->Width, Cap->Height);
    DEBUG ((DEBUG_INFO, "CameraTest: ae: step %u: mean %u (target %u), %u lines, analog %u/256, digital %u/256, frame end %a\n",
            Iter, Mean, Ae.Target, Ae.Lines, Ae.AnalogX256, Ae.DigitalX256, (Status & (1u << 4)) ? "yes" : "NO"));
    CamAeStep (&Ae, Mean);
    CamAeApply (&I2c, &Ae);
    Ends = 0;
    gBS->Stall (120000);                    // the new settings reach a frame in 2 frames
  }
}

//
// Step 3a: one raw frame, sensor -> CSIS0 -> FIMC-Lite A DMA, no ISP.
//
#define RAW_W  mRawW
#define RAW_H  mRawH

STATIC
VOID
CaptureRaw (
  EFI_HANDLE  ImageHandle,
  FIMC_IS     *Is
  )
{
  CAM_CAPTURE       Cap;
  CAM_CAPTURE_REGS  R;
  UINTN             Bytes = RAW_W * RAW_H * 2;
  UINT16            *Buf;
  EFI_PHYSICAL_ADDRESS  Pa = 0xFFFFFFFF;
  UINT32            Status;
  UINT32            CsisSrc;
  UINT32            AllStatus = 0;
  UINT32            AllCsis = 0;
  UINTN             Starts = 0;
  UINTN             Ends = 0;
  UINTN             Loops;
  UINTN             Index;
  UINTN             Changed;
  UINT64            Sum;
  EFI_STATUS        St;

  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, EFI_SIZE_TO_PAGES (Bytes), &Pa))) {
    DEBUG ((DEBUG_ERROR, "CameraTest: raw: no %u-byte buffer\n", Bytes));
    return;
  }
  Buf = (UINT16 *)(UINTN)Pa;
  SetMem (Buf, Bytes, 0xA5);
  WriteBackInvalidateDataCacheRange (Buf, Bytes);

  ZeroMem (&Cap, sizeof (Cap));
  Cap.Csis   = mFront ? CAM_CSIS1_PHYS : CAM_CSIS0_PHYS;
  Cap.Flite  = mFront ? CAM_FLITE_B_PHYS : CAM_FLITE_A_PHYS;
  Cap.FliteB = mFront;
  Cap.Pmu    = CAM_PMU_PHYS;
  Cap.Dphy   = mFront ? 1 : 0;
  Cap.Width  = RAW_W;
  Cap.Height = RAW_H;
  Cap.Settle = mFront ? 16 : 15;
  Cap.Lanes  = 4;
  Cap.BufPa  = (UINT32)Pa;
  CamCaptureStart (&Cap);
  CamCaptureSnapshot (&Cap, &R);
  DEBUG ((DEBUG_INFO, "CameraTest: raw: armed at 0x%08x: CSIS ctrl %08x dphy %08x cfg %08x resol %08x FLITE ctrl %08x cpt %08x "
          "seq %08x PMU dphy %08x\n", Cap.BufPa, R.CsisCtrl, R.CsisDphy, R.CsisConfig, R.CsisResol, R.FliteCtrl,
          R.FliteCapture, R.FliteSeq, R.Dphy));

  if (FimcIsCommand (Is, "stream on", HIC_STREAM_ON, 0, 0, 0, 0, 0, NULL) != 0) {
    DEBUG ((DEBUG_ERROR, "CameraTest: raw: stream on refused\n"));
  }

  if (mAe) {
    AutoExposure (&Cap, Buf, Bytes);
  }
  if (mI2cProbe) {
    SensorExposure ();
    gBS->Stall (400000);                     // a few frames with the new exposure
  }

  //
  // Up to 3 s: count FIMC-Lite frame starts / ends; stop after 3 ends
  // (one buffer slot, so the last complete frame is in the buffer).
  //
  for (Loops = 0; (Loops < 30000) && (Ends < 3); Loops++) {
    Status = CamCaptureTakeStatus (&Cap, &CsisSrc);
    AllStatus |= Status;
    AllCsis |= CsisSrc;
    if (Status & (1u << 5)) {
      Starts++;
    }
    if (Status & (1u << 4)) {
      Ends++;
    }
    MicroSecondDelay (100);
  }
  CamCaptureSnapshot (&Cap, &R);
  DEBUG ((DEBUG_INFO, "CameraTest: raw: %u starts, %u ends in %u ms; FLITE status seen %08x now %08x/%08x/%08x, "
          "CSIS intsrc seen %08x\n", Starts, Ends, Loops / 10, AllStatus, R.FliteStatus, R.FliteStatus2, R.FliteStatus3,
          AllCsis));
  FimcIsCommand (Is, "stream off", HIC_STREAM_OFF, 0, 0, 0, 0, 0, NULL);
  CamCaptureStop (&Cap);

  InvalidateDataCacheRange (Buf, Bytes);
  for (Index = 0, Changed = 0, Sum = 0; Index < Bytes / 2; Index++) {
    if (Buf[Index] != 0xA5A5) {
      Changed++;
      Sum += Buf[Index];
    }
  }
  DEBUG ((DEBUG_INFO, "CameraTest: raw: %u of %u pixels written, mean %u; first %04x %04x %04x %04x, middle %04x %04x\n",
          Changed, Bytes / 2, Changed ? (UINT32)(Sum / Changed) : 0, Buf[0], Buf[1], Buf[2], Buf[3],
          Buf[(RAW_H / 2) * RAW_W + RAW_W / 2], Buf[(RAW_H / 2) * RAW_W + RAW_W / 2 + 1]));
  if (Changed != 0) {
    St = SaveFile (ImageHandle, mFront ? L"\\front.raw" : L"\\frame.raw", Buf, Bytes);
    DEBUG ((DEBUG_INFO, "CameraTest: raw: %a (%ux%u, 16-bit) - %r\n", mFront ? "\\front.raw" : "\\frame.raw", RAW_W, RAW_H, St));
  }
  gBS->FreePages (Pa, EFI_SIZE_TO_PAGES (Bytes));
}

STATIC
VOID
OpenSensor (
  EFI_HANDLE  ImageHandle,
  FIMC_IS     *Is,
  UINT32      Group,
  BOOLEAN     Raw
  )
{
  VOID   *Setf;
  UINTN  SetfSize;

  if ((mFront ? FimcIsOpenFrontSensor (Is, Group) : FimcIsOpenRearSensor (Is, Group)) != 0) {
    DEBUG ((DEBUG_ERROR, "CameraTest: open sensor failed\n"));
    return;
  }
  if (EFI_ERROR (LoadFile (ImageHandle, mFront ? SETFILE_FRONT : SETFILE_FILE, &Setf, &SetfSize))) {
    DEBUG ((DEBUG_ERROR, "CameraTest: setfile not on the boot volume\n"));
    return;
  }
  if (FimcIsLoadSetfile (Is, Group, Setf, SetfSize) != 0) {
    DEBUG ((DEBUG_ERROR, "CameraTest: setfile failed\n"));
    return;
  }
  DEBUG ((DEBUG_INFO, "CameraTest: sensor open sequence complete\n"));
  if (Raw) {
    CaptureRaw (ImageHandle, Is);
  }
}

EFI_STATUS
EFIAPI
CameraTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Image;
  CHAR16                     *Opt = L"";
  BOOLEAN                    NoSmc;
  BOOLEAN                    TopMux;
  BOOLEAN                    Keep;
  BOOLEAN                    Open;
  BOOLEAN                    Raw;
  UINT32                     Group;
  CAM_BOARD                  Board;
  FIMC_IS                    Is;
  FIMC_IS_MSG                Msg;
  VOID                       *Fw;
  UINTN                      FwSize;
  VOID                       *Mem;
  UINT32                     PendBefore[GICD_WORDS];
  UINT32                     PendAfter[GICD_WORDS];
  UINT32                     CombBefore;
  UINT32                     CombAfter;
  INTN                       Result;
  UINTN                      Count;

  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image)) &&
      (Image->LoadOptions != NULL) && (Image->LoadOptionsSize >= sizeof (CHAR16)))
  {
    Opt = (CHAR16 *)Image->LoadOptions;
  }
  NoSmc  = (StrStr (Opt, L"nosmc") != NULL);
  TopMux = (StrStr (Opt, L"topmux") != NULL);
  Keep   = (StrStr (Opt, L"keep") != NULL);
  mFront = (StrStr (Opt, L"front") != NULL);
  mI2cProbe = (StrStr (Opt, L"i2c") != NULL);
  mAe       = (StrStr (Opt, L"ae") != NULL);
  {
    CONST CHAR16  *P;

    //
    // expo=<lines> again=<code> dgain=<hex 8.8>  (decimal / hex parsed by StrDecimalToUintn / StrHexToUintn)
    //
    P = StrStr (Opt, L"expo=");
    mExpoLines  = (P != NULL) ? (UINT32)StrDecimalToUintn (P + 5) : 0;
    P = StrStr (Opt, L"again=");
    mAnalogGain = (P != NULL) ? (UINT32)StrDecimalToUintn (P + 6) : 0;
    P = StrStr (Opt, L"dgain=");
    mDigitalGain = (P != NULL) ? (UINT32)StrDecimalToUintn (P + 6) : 0;
    if (mExpoLines != 0) {
      mI2cProbe = TRUE;
    }
  }
  if (mFront) {
    mRawW = 1936;
    mRawH = 1090;
  }
  Raw    = (StrStr (Opt, L"raw") != NULL);
  Open   = Raw || (StrStr (Opt, L"open") != NULL);
  Group  = (StrStr (Opt, L"g1") != NULL) ? FIMC_IS_GROUP_3A1 : FIMC_IS_GROUP_3A0;
  ZeroMem (&Board, sizeof (Board));
  Board.GpioLb      = CAM_GPIO_LB_PHYS;
  Board.GpioB       = CAM_GPIO_B_PHYS;
  Board.GpioRt      = CAM_GPIO_RT_PHYS;
  Board.CmuTop      = CAM_CMU_TOP_PHYS;
  Board.PmicRead    = PmicRead;
  Board.PmicWrite   = PmicWrite;
  if (Open && EFI_ERROR (gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&mI2c))) {
    DEBUG ((DEBUG_ERROR, "CameraTest: no I2C protocol, no sensor power\n"));
    Open = FALSE;
  }
  DEBUG ((DEBUG_INFO, "CameraTest: start (smc %a, topmux %a, keep %a, open %a group %u)\n", NoSmc ? "off" : "on",
          TopMux ? "on" : "off", Keep ? "on" : "off", Open ? "on" : "off", Group));

  ZeroMem (&Is, sizeof (Is));
  Is.Is     = FIMC_IS_PHYS;
  Is.Pmu    = FIMC_IS_PMU_PHYS;
  Is.Cmu    = FIMC_IS_CMU_PHYS;
  Is.Sysreg = FIMC_IS_SYSREG_PHYS;
  if (!NoSmc) {
    Is.SecureWrite = SecureWrite;
    Is.SecureRead  = SecureRead;
  }
  LogRegs ("before", &Is);

  if (EFI_ERROR (LoadFile (ImageHandle, FW_FILE, &Fw, &FwSize))) {
    DEBUG ((DEBUG_ERROR, "CameraTest: %s not on the boot volume, stopping\n", FW_FILE));
    return EFI_NOT_FOUND;
  }
  LogFwTail (Fw, FwSize);

  Mem = AllocateAlignedPages (EFI_SIZE_TO_PAGES (FIMC_IS_A5_MEM_SIZE), FIMC_IS_A5_MEM_ALIGN);
  if ((Mem == NULL) || ((UINTN)Mem > MAX_UINT32)) {
    DEBUG ((DEBUG_ERROR, "CameraTest: no 20 MiB / 64 MiB-aligned buffer\n"));
    return EFI_OUT_OF_RESOURCES;
  }
  Is.MemVa = Mem;
  Is.MemPa = (UINT32)(UINTN)Mem;
  DEBUG ((DEBUG_INFO, "CameraTest: A5 memory at 0x%08x\n", Is.MemPa));

  if (FimcIsPowerUp (&Is, TopMux) != 0) {
    DEBUG ((DEBUG_ERROR, "CameraTest: power-up failed at step %u (ISP domain)\n", Is.Step));
    goto Off;
  }
  LogRegs ("powered", &Is);

  if (FimcIsLoadFirmware (&Is, Fw, FwSize) != 0) {
    DEBUG ((DEBUG_ERROR, "CameraTest: firmware does not fit\n"));
    goto Off;
  }

  if (Open) {
    LogBoard ("module before", &Board);
    Board.FrontExtraIo = (StrStr (Opt, L"io18") != NULL);
    Board.I2cDrive     = (StrStr (Opt, L"drv") != NULL);
    Board.I2cPullUp    = (StrStr (Opt, L"pu") != NULL);
    if ((mFront ? CamFrontPowerOn (&Board) : CamRearPowerOn (&Board)) != 0) {
      DEBUG ((DEBUG_ERROR, "CameraTest: module power failed at step %u\n", Board.Step));
    }
    LogBoard ("module on", &Board);
    if (mFront) {
      DEBUG ((DEBUG_INFO, "CameraTest: front: I2C2 lines idle (SDA,SCL as inputs) %u%u, GPY7 dat 0x%02x, LDO19 0x%02x\n",
              Board.I2cProbe & 1, (Board.I2cProbe >> 1) & 1, Board.FrontRails & 0xFF, Board.FrontRails >> 8));
    }
  }

  //
  // Let combiner group 19 (ARMISP bit 1, ISP bit 4) through to the GIC;
  // the GIC keeps SPI 19 disabled, so it only shows up as pending.
  //
  MmioWrite32 (COMBINER_IESR (19), (1u << 25) | (1u << 28));
  SnapIrq (PendBefore, &CombBefore);
  Result = FimcIsStartA5 (&Is);
  if (!NoSmc) {
    DEBUG ((DEBUG_INFO, "CameraTest: secure monitor: first write status 0x%08x, read status 0x%08x, 0x131E0004 = 0x%08x (0xFC expected)\n",
            mFirstSmcResult, mLastReadStatus, Is.SecureCheck));
  }
  if (Result != 0) {
    DEBUG ((DEBUG_ERROR, "CameraTest: A5 start failed at step %u (%d)\n", Is.Step, Result));
    LogRegs ("failed", &Is);
    goto Off;
  }
  DEBUG ((DEBUG_INFO, "CameraTest: A5 on, waiting for the firmware\n"));

  //
  // First message: IHC_GET_SENSOR_NUMBER.  Log whatever else arrives
  // within the next second too.
  //
  //
  // Wait for the first message WITHOUT acknowledging it, so the
  // interrupt line is still up when the GIC / combiner are sampled.
  //
  for (Count = 0; Count < 30000; Count++) {
    if (((MmioRead32 (MCU_INTSR1) | MmioRead32 (MCU_INTMSR1)) & 1) || (MmioRead32 (MCU_IFLAG) != 0)) {
      break;
    }
    MicroSecondDelay (100);
  }
  MicroSecondDelay (1000);
  SnapIrq (PendAfter, &CombAfter);
  DEBUG ((DEBUG_INFO, "CameraTest: at message: INTMR1 0x%08x INTSR1 0x%08x INTMSR1 0x%08x IFLAG 0x%x, after %u x 100 us\n",
          MmioRead32 (MCU_INTMR1), MmioRead32 (MCU_INTSR1), MmioRead32 (MCU_INTMSR1), MmioRead32 (MCU_IFLAG), Count));
  LogIrqDiff (PendBefore, PendAfter, CombBefore, CombAfter);

  if (FimcIsWaitMessage (&Is, 3000, &Msg) != 0) {
    DEBUG ((DEBUG_ERROR, "CameraTest: no message from the A5 in 3 s\n"));
  } else {
    DEBUG ((DEBUG_INFO, "CameraTest: A5 says cmd 0x%x sensor %u params %08x %08x %08x %08x%a\n", Msg.Command, Msg.SensorId,
            Msg.Param[0], Msg.Param[1], Msg.Param[2], Msg.Param[3],
            (Msg.Command == IHC_GET_SENSOR_NUMBER) ? " -> ALIVE" : ""));
    if (Open) {
      OpenSensor (ImageHandle, &Is, Group, Raw);
    }
    for (Count = 0; !Open && (Count < 8) && (FimcIsWaitMessage (&Is, 1000, &Msg) == 0); Count++) {
      DEBUG ((DEBUG_INFO, "CameraTest: then cmd 0x%x sensor %u params %08x %08x %08x %08x\n", Msg.Command, Msg.SensorId,
              Msg.Param[0], Msg.Param[1], Msg.Param[2], Msg.Param[3]));
    }
  }
  LogRegs ("after", &Is);

  //
  // fimc_is_itf_power_down(): the firmware parks the A5 (WFI) before the
  // domain goes off.
  //
  if (!Keep && (Msg.Command == IHC_GET_SENSOR_NUMBER || Open)) {
    FimcIsCommand (&Is, "power down", HIC_POWER_DOWN, 0, 0, 0, 0, 0, NULL);
    DEBUG ((DEBUG_INFO, "CameraTest: A5 state (ISSR6) 0x%08x\n", MmioRead32 (FIMC_IS_PHYS + 0x180080 + 6 * 4)));
  }

  if (Keep) {
    MmioWrite32 (COMBINER_IECR (19), (1u << 25) | (1u << 28));
    DEBUG ((DEBUG_INFO, "CameraTest: done, A5 left running\n"));
    return EFI_SUCCESS;
  }

Off:
  MmioWrite32 (COMBINER_IECR (19), (1u << 25) | (1u << 28));
  FimcIsPowerDown (&Is);
  if (mFront) {
    CamFrontPowerOff (&Board);
  } else {
    CamRearPowerOff (&Board);
  }
  if (Open) {
    LogBoard ("module off", &Board);
  }
  LogRegs ("off", &Is);
  DEBUG ((DEBUG_INFO, "CameraTest: done, ISP off\n"));
  return EFI_SUCCESS;
}
