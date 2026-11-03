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

#include "../../Drivers/Camera/FimcIsCore.h"

#define FW_FILE           L"\\fimc_is_fw2_IMX134.bin"

#define GICD_ISPENDR(n)   (0x10481200u + (n) * 4)
#define GICD_WORDS        9                 // IRQs 0..287
#define COMBINER_ISTR(g)  (0x10440000u + ((g) / 4) * 0x10 + 0x8)

#define SMC_CMD_REG       ((UINT32)-101)
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

//
// exynos_smc() / exynos_smc_readsfr() (arch/arm/mach-exynos/smc.S).
//
STATIC
UINT32
Smc (
  IN     UINT32  A0,
  IN     UINT32  A1,
  IN OUT UINT32  *A2,
  IN     UINT32  A3
  )
{
  register UINT32  R0 __asm__ ("r0") = A0;
  register UINT32  R1 __asm__ ("r1") = A1;
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
  *A2 = R2;
  return R0;
}

STATIC UINT32  mFirstSmcResult = ~0u;

STATIC
INTN
SecureWrite (
  UINT32  PhysAddr,
  UINT32  Value
  )
{
  UINT32  R0 = Smc (SMC_CMD_REG, SMC_SFR_W (PhysAddr), &Value, 0);

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
  UINT32  R2 = 0;

  if (Smc (SMC_CMD_REG, SMC_SFR_R (PhysAddr), &R2, 0) != 0) {
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
  DEBUG ((DEBUG_INFO, "CameraTest: start (smc %a, topmux %a, keep %a)\n", NoSmc ? "off" : "on", TopMux ? "on" : "off",
          Keep ? "on" : "off"));

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

  SnapIrq (PendBefore, &CombBefore);
  Result = FimcIsStartA5 (&Is);
  if (!NoSmc) {
    DEBUG ((DEBUG_INFO, "CameraTest: secure monitor: first write r0 0x%08x, 0x131E0004 = 0x%08x (0xFC expected)\n",
            mFirstSmcResult, Is.SecureCheck));
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
  if (FimcIsWaitMessage (&Is, 3000, &Msg) != 0) {
    DEBUG ((DEBUG_ERROR, "CameraTest: no message from the A5 in 3 s\n"));
  } else {
    DEBUG ((DEBUG_INFO, "CameraTest: A5 says cmd 0x%x sensor %u params %08x %08x %08x %08x%a\n", Msg.Command, Msg.SensorId,
            Msg.Param[0], Msg.Param[1], Msg.Param[2], Msg.Param[3],
            (Msg.Command == IHC_GET_SENSOR_NUMBER) ? " -> ALIVE" : ""));
    for (Count = 0; (Count < 8) && (FimcIsWaitMessage (&Is, 1000, &Msg) == 0); Count++) {
      DEBUG ((DEBUG_INFO, "CameraTest: then cmd 0x%x sensor %u params %08x %08x %08x %08x\n", Msg.Command, Msg.SensorId,
              Msg.Param[0], Msg.Param[1], Msg.Param[2], Msg.Param[3]));
    }
  }
  SnapIrq (PendAfter, &CombAfter);
  LogIrqDiff (PendBefore, PendAfter, CombBefore, CombAfter);
  LogRegs ("after", &Is);

  if (Keep) {
    DEBUG ((DEBUG_INFO, "CameraTest: done, A5 left running\n"));
    return EFI_SUCCESS;
  }

Off:
  FimcIsPowerDown (&Is);
  LogRegs ("off", &Is);
  DEBUG ((DEBUG_INFO, "CameraTest: done, ISP off\n"));
  return EFI_SUCCESS;
}
