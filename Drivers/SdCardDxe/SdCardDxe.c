/** @file
  Exynos5420 (v1awifi) eMMC + microSD storage driver entry point.

  Publishes an EFI_BLOCK_IO_PROTOCOL for the internal eMMC (dw_mmc ch0)
  and, only when the card-detect pin reports a card, for the microSD slot
  (dw_mmc ch2).  An empty slot is never powered or initialised.

  Board wiring is from board-universal5420-mmc.c / board-universal5420-
  clock.c / gpio-v1a-3g-rev00.h:

    eMMC   CLK/CMD/DAT0-3 GPC0[0,1,3-6], DAT4-7 GPC3[0-3], func 2, LV3;
           GPC0(7) func 2 pull-down (RCLK); GPC0(2) output high.
    SD     CLK/CMD/DAT0-3 GPC2[0,1,3-6], func 2, LV3 (CLK LV4);
           card detect GPX2(4), low = inserted (sd_detection_cmd_show);
           slot power GPY2(0) = GPIO_TF_EN, high = on.
    Clocks sclk_dwmci0/2 = CPLL (666 MHz) / 1; CLKSEL 0x03040000 divides by
           4 inside the controller (pdata bus_hz = 666 MHz / 4).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdCardDxe.h"

#include <Protocol/HardwareInterrupt.h>
#include <Protocol/ExynosMmcTest.h>

//
// CMU (EXYNOS5_PA_CMU = 0x10010000, regs-clock.h offsets).
//
#define CMU_BASE            0x10010000
#define CLK_CPLL_CON0       (CMU_BASE + 0x10120)
#define CLK_DPLL_CON0       (CMU_BASE + 0x10128)
#define CLK_EPLL_CON0       (CMU_BASE + 0x10130)
#define CLK_RPLL_CON0       (CMU_BASE + 0x10140)
#define CLK_IPLL_CON0       (CMU_BASE + 0x10150)
#define CLK_SPLL_CON0       (CMU_BASE + 0x10160)
#define CLK_MPLL_CON0       (CMU_BASE + 0x10180)
#define CLK_SRC_FSYS        (CMU_BASE + 0x10244)
#define CLK_SRC_MASK_FSYS   (CMU_BASE + 0x10340)
#define CLK_DIV_FSYS1       (CMU_BASE + 0x1054C)
#define CLK_GATE_IP_FSYS    (CMU_BASE + 0x10944)
#define CLK_DIV_STAT_FSYS1  (CMU_BASE + 0x1064C)

#define XXTI_HZ             24000000U
#define MUX_SRC_CPLL        1

//
// Per-channel fields: mux CLK_SRC_FSYS[shift+2:shift], mask bit in
// CLK_SRC_MASK_FSYS, 10-bit divider in CLK_DIV_FSYS1, IP gate.
//
typedef struct {
  UINT32  MuxShift;
  UINT32  DivShift;
  UINT32  GateBit;
} DWMCI_CLOCK;

STATIC CONST DWMCI_CLOCK  mClk0 = { 8, 0, 12 };
STATIC CONST DWMCI_CLOCK  mClk2 = { 16, 20, 14 };

//
// CLKSEL for default-speed SDR (pdata sdr_timing): CIU divider 4.
//
#define DWMCI_SDR_TIMING    0x03040000U
#define CLKSEL_CIU_DIV(x)   ((((x) >> 24) & 0x7) + 1)

//
// GPIO drive strength (bank + 0x0C, 2 bits per pin; LV3 = 1, LV4 = 3).
//
#define GPIO_T_BASE         0x13410000
#define GPC0_DRV            (GPIO_T_BASE + 0x000 + 0x0C)
#define GPC2_DRV            (GPIO_T_BASE + 0x040 + 0x0C)
#define GPC3_DRV            (GPIO_T_BASE + 0x060 + 0x0C)
#define DRV_LV3             1
#define DRV_LV4             3

#define SD_CD_GPIO          EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX2, 4)
#define SD_POWER_GPIO       EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPY2, 0)

//
// Per-card BlockIo instance.
//
typedef struct {
  EFI_BLOCK_IO_PROTOCOL  BlkIo;
  EFI_BLOCK_IO_MEDIA     Media;
  MMC_HOST               *Host;
} SD_BLOCK_IO;

STATIC SD_BLOCK_IO  mEmmcBlkIo;
STATIC SD_BLOCK_IO  mSdBlkIo;
STATIC MMC_HOST     mEmmcHost;
STATIC MMC_HOST     mSdHost;

typedef struct {
  VENDOR_DEVICE_PATH  Vendor;
  EFI_DEVICE_PATH     End;
} STORAGE_DEVICE_PATH;

STATIC STORAGE_DEVICE_PATH  mEmmcDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)(sizeof (VENDOR_DEVICE_PATH)), (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8) } },
    { 0x5d2a1c40, 0x5420, 0x4e6b, { 0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x54, 0x00 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC STORAGE_DEVICE_PATH  mSdDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)(sizeof (VENDOR_DEVICE_PATH)), (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8) } },
    { 0x5d2a1c40, 0x5420, 0x4e6b, { 0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x54, 0x02 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// ---------------------------------------------------------------------------
// BlockIo.
// ---------------------------------------------------------------------------
//

STATIC
EFI_STATUS
EFIAPI
BlkReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
BlkReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  SD_BLOCK_IO  *BlkIo = BASE_CR (This, SD_BLOCK_IO, BlkIo);

  if (MediaId != BlkIo->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if ((BufferSize % BlkIo->Media.BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (((UINTN)Buffer & 3) != 0) {
    return EFI_INVALID_PARAMETER;     // FIFO is moved in 32-bit words
  }

  return MmcReadBlocks (BlkIo->Host, Lba, BufferSize / BlkIo->Media.BlockSize, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
BlkWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  SD_BLOCK_IO  *BlkIo = BASE_CR (This, SD_BLOCK_IO, BlkIo);

  if (MediaId != BlkIo->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if ((BufferSize % BlkIo->Media.BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (((UINTN)Buffer & 3) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  return MmcWriteBlocks (BlkIo->Host, Lba, BufferSize / BlkIo->Media.BlockSize, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
BlkFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SetupBlockIo (
  IN  SD_BLOCK_IO          *BlkIo,
  IN  MMC_HOST             *Host,
  IN  BOOLEAN              IsEmmc,
  IN  STORAGE_DEVICE_PATH  *DevicePath
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  Status = MmcInitCard (Host, IsEmmc);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&BlkIo->Media, sizeof (BlkIo->Media));
  BlkIo->Media.MediaPresent   = TRUE;
  BlkIo->Media.RemovableMedia = !IsEmmc;
  BlkIo->Media.BlockSize      = Host->Card.BlockSize;
  BlkIo->Media.LastBlock      = Host->Card.BlockCount - 1;
  BlkIo->Media.IoAlign        = 4;

  BlkIo->BlkIo.Revision    = EFI_BLOCK_IO_INTERFACE_REVISION;
  BlkIo->BlkIo.Media       = &BlkIo->Media;
  BlkIo->BlkIo.Reset       = BlkReset;
  BlkIo->BlkIo.ReadBlocks  = BlkReadBlocks;
  BlkIo->BlkIo.WriteBlocks = BlkWriteBlocks;
  BlkIo->BlkIo.FlushBlocks = BlkFlushBlocks;
  BlkIo->Host              = Host;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiBlockIoProtocolGuid, &BlkIo->BlkIo,
                  &gEfiDevicePathProtocolGuid, DevicePath,
                  NULL
                  );
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "SdCard: %a %lu MiB, %u-bit, %a\n",
            IsEmmc ? "eMMC" : "microSD",
            DivU64x32 (MultU64x32 (Host->Card.BlockCount, Host->Card.BlockSize), SIZE_1MB),
            Host->Card.BusWidth,
            Host->Card.HighCapacity ? "block addressed" : "byte addressed"));
  }
  return Status;
}

//
// ---------------------------------------------------------------------------
// Clocks.
// ---------------------------------------------------------------------------
//

/**
  Output of a PLL from its CON0: FOUT = MDIV * FIN / (PDIV << SDIV)
  (pll_35xx; the 36xx EPLL/RPLL fraction is ignored, which only makes the
  computed rate a hair low).  0 if the PLL is off.
**/
STATIC
UINT32
PllRate (
  IN UINTN  Con0
  )
{
  UINT32  Reg;
  UINT32  Mdiv;
  UINT32  Pdiv;
  UINT32  Sdiv;

  Reg = MmioRead32 (Con0);
  if (!(Reg & (1U << 31))) {
    return 0;
  }
  Mdiv = (Reg >> 16) & 0x3FF;
  Pdiv = (Reg >> 8) & 0x3F;
  Sdiv = Reg & 0x7;
  if (Pdiv == 0) {
    return 0;
  }
  return (UINT32)DivU64x32 (MultU64x32 (XXTI_HZ, Mdiv), Pdiv << Sdiv);
}

STATIC
UINT32
MuxSourceRate (
  IN UINT32  Source
  )
{
  STATIC CONST UINTN  Pll[] = {
    0, CLK_CPLL_CON0, CLK_DPLL_CON0, CLK_MPLL_CON0,
    CLK_SPLL_CON0, CLK_IPLL_CON0, CLK_EPLL_CON0, CLK_RPLL_CON0
  };

  if (Source == 0) {
    return XXTI_HZ;
  }
  return (Source < ARRAY_SIZE (Pll)) ? PllRate (Pll[Source]) : 0;
}

/**
  Gate on sclk_dwmci for one channel, feed it from CPLL / 1 like the kernel
  (or keep the bootloader's source if CPLL is off), and return the CIU clock
  after the CLKSEL divider.
**/
STATIC
UINT32
DwmciClockInit (
  IN CONST DWMCI_CLOCK  *Clk,
  IN UINT32             ClkSel
  )
{
  UINT32  Source;
  UINT32  Div;
  UINT32  Rate;

  MmioOr32 (CLK_GATE_IP_FSYS, 1U << Clk->GateBit);
  MmioOr32 (CLK_SRC_MASK_FSYS, 1U << Clk->MuxShift);

  if (MuxSourceRate (MUX_SRC_CPLL) != 0) {
    MmioAndThenOr32 (CLK_SRC_FSYS, ~(0x7U << Clk->MuxShift), MUX_SRC_CPLL << Clk->MuxShift);
    MmioAnd32 (CLK_DIV_FSYS1, ~(0x3FFU << Clk->DivShift));
  }

  Source = (MmioRead32 (CLK_SRC_FSYS) >> Clk->MuxShift) & 0x7;
  Div    = ((MmioRead32 (CLK_DIV_FSYS1) >> Clk->DivShift) & 0x3FF) + 1;
  Rate   = MuxSourceRate (Source) / Div;
  return Rate / CLKSEL_CIU_DIV (ClkSel);
}

//
// ---------------------------------------------------------------------------
// Board.
// ---------------------------------------------------------------------------
//

STATIC
VOID
SetDrive (
  IN UINTN   DrvReg,
  IN UINT32  Pin,
  IN UINT32  Level
  )
{
  MmioAndThenOr32 (DrvReg, ~(0x3U << (Pin * 2)), Level << (Pin * 2));
}

/**
  exynos_dwmci0_cfg_gpio(8).
**/
STATIC
VOID
EmmcBoardInit (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio
  )
{
  UINT32  Pin;

  for (Pin = 0; Pin <= 6; Pin++) {
    if (Pin == 2) {
      continue;
    }
    Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC0, Pin), 2, EXYNOS_GPIO_PULL_NONE);
    SetDrive (GPC0_DRV, Pin, DRV_LV3);
  }
  for (Pin = 0; Pin <= 3; Pin++) {
    Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC3, Pin), 2, EXYNOS_GPIO_PULL_NONE);
    SetDrive (GPC3_DRV, Pin, DRV_LV3);
  }
  Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC0, 7), 2, EXYNOS_GPIO_PULL_DOWN);

  Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC0, 2), EXYNOS_GPIO_MODE_OUTPUT,
                EXYNOS_GPIO_PULL_NONE);
  Gpio->Set (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC0, 2), 1);
}

/**
  Card detect, debounced: GPX2(4) low on several reads 2 ms apart.  Plain
  input, no pull (exynos_dwmci2_cfg_gpio: the board pulls it up).
**/
STATIC
BOOLEAN
SdCardPresent (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio
  )
{
  UINTN  Read;

  Gpio->Config (Gpio, SD_CD_GPIO, EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_NONE);
  gBS->Stall (1000);

  for (Read = 0; Read < 5; Read++) {
    if (Gpio->Get (Gpio, SD_CD_GPIO) != 0) {
      return FALSE;
    }
    gBS->Stall (2000);
  }
  return TRUE;
}

STATIC
VOID
SdPower (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio,
  IN BOOLEAN               On
  )
{
  Gpio->Config (Gpio, SD_POWER_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  Gpio->Set (Gpio, SD_POWER_GPIO, On ? 1 : 0);
}

/**
  Power-cycle the slot and route its pins (exynos_dwmci2_cfg_gpio(4) +
  exynos_dwmci2_setpower).  Pins are inputs while the card is unpowered so
  it is not back-fed through CMD/DAT.
**/
STATIC
VOID
SdBoardInit (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio
  )
{
  UINT32  Pin;

  for (Pin = 0; Pin <= 6; Pin++) {
    if (Pin != 2) {
      Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC2, Pin),
                    EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_NONE);
    }
  }
  SdPower (Gpio, FALSE);
  gBS->Stall (50000);

  for (Pin = 0; Pin <= 6; Pin++) {
    if (Pin == 2) {
      continue;
    }
    Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC2, Pin), 2, EXYNOS_GPIO_PULL_NONE);
    SetDrive (GPC2_DRV, Pin, (Pin == 0) ? DRV_LV4 : DRV_LV3);
  }

  SdPower (Gpio, TRUE);
  gBS->Stall (20000);      // rail ramp + card power-up (SD spec: >= 1 ms)
}


//
// ---------------------------------------------------------------------------
// Speed modes: the CMU side, interrupt, test protocol.
// ---------------------------------------------------------------------------
//

//
// eMMC interrupt: EXYNOS5_IRQ_HSMMC0 = IRQ_SPI(75) = GIC INTID 107.
//
#define EMMC_IRQ  107

//
// sclk_dwmci0 = CPLL / (N + 1).  N = 0 (666 MHz) for the default / high-speed
// modes (CIU 166.5 MHz after CLKSEL / 4, as the kernel's bus_hz); HS400 uses
// the kernel's HS200_DDR table row: sclk 333 MHz (N = 1), CLKSEL / 2.
//
STATIC UINT32  mHs400DivN = 1;

STATIC EFI_HARDWARE_INTERRUPT_PROTOCOL  *mGic;
STATIC BOOLEAN                          mIrqRouted;

STATIC
VOID
EFIAPI
EmmcInterruptHandler (
  IN HARDWARE_INTERRUPT_SOURCE  Source,
  IN EFI_SYSTEM_CONTEXT         SystemContext
  )
{
  DwmciIsr (&mEmmcHost);
  mGic->EndOfInterrupt (mGic, Source);
}

STATIC
EFI_STATUS
EFIAPI
EmmcSourceClock (
  IN  MMC_HOST   *Host,
  IN  MMC_SPEED  Speed,
  IN  UINT32     ClkSel,
  OUT UINT32     *CiuHz
  )
{
  UINT32  N;
  UINT32  Cpll;
  UINTN   Retry;

  N    = (Speed == MmcSpeedHs400Es) ? mHs400DivN : 0;
  Cpll = PllRate (CLK_CPLL_CON0);
  if (Cpll == 0) {
    return EFI_NOT_READY;
  }
  MmioAndThenOr32 (CLK_DIV_FSYS1, ~(0x3FFU << mClk0.DivShift), N << mClk0.DivShift);
  for (Retry = 0; Retry < 1000; Retry++) {
    if (MmioRead32 (CLK_DIV_STAT_FSYS1) == 0) {
      break;
    }
    gBS->Stall (10);
  }
  *CiuHz = (Cpll / (N + 1)) / CLKSEL_CIU_DIV (ClkSel);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TestGetInfo (
  IN  EXYNOS_MMC_TEST_PROTOCOL  *This,
  OUT EXYNOS_MMC_INFO           *Info
  )
{
  ZeroMem (Info, sizeof (*Info));
  Info->Base          = mEmmcHost.Base;
  Info->Speed         = mEmmcHost.Speed;
  Info->BusWidth      = mEmmcHost.Card.BusWidth;
  Info->ClockHz       = mEmmcHost.ClockHz;
  Info->CiuHz         = mEmmcHost.MaxClockHz;
  Info->IrqMode       = mEmmcHost.IrqMode;
  Info->IrqCount      = mEmmcHost.IrqCount;
  Info->IrqMissed     = mEmmcHost.IrqMissed;
  Info->DqsDelay      = mEmmcHost.DqsDelay;
  Info->BlockCount    = mEmmcHost.Card.BlockCount;
  Info->ExtCsdRev     = mEmmcHost.Card.ExtCsdRev;
  Info->DeviceType    = mEmmcHost.Card.DeviceType;
  Info->StrobeSupport = mEmmcHost.Card.StrobeSupport;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TestSetSpeed (
  IN EXYNOS_MMC_TEST_PROTOCOL  *This,
  IN UINT32                    Speed
  )
{
  return (Speed < MmcSpeedCount) ? MmcSetSpeed (&mEmmcHost, (MMC_SPEED)Speed) : EFI_INVALID_PARAMETER;
}

STATIC
EFI_STATUS
EFIAPI
TestSetIrq (
  IN EXYNOS_MMC_TEST_PROTOCOL  *This,
  IN BOOLEAN                   On
  )
{
  if (!mIrqRouted) {
    return EFI_UNSUPPORTED;
  }
  if (On) {
    DwmciIrqEnable (&mEmmcHost, TRUE);
    mGic->EnableInterruptSource (mGic, EMMC_IRQ);
  } else {
    mGic->DisableInterruptSource (mGic, EMMC_IRQ);
    DwmciIrqEnable (&mEmmcHost, FALSE);
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TestSetClock (
  IN EXYNOS_MMC_TEST_PROTOCOL  *This,
  IN UINT32                    Hz
  )
{
  return mEmmcHost.SetClock (&mEmmcHost, Hz);
}

STATIC
EFI_STATUS
EFIAPI
TestSetDqs (
  IN EXYNOS_MMC_TEST_PROTOCOL  *This,
  IN UINT32                    Delay
  )
{
  UINT32  Hz;

  mEmmcHost.DqsDelay = Delay & 0x3FF;
  if (mEmmcHost.Speed == MmcSpeedHs400Es) {
    Hz = mEmmcHost.ClockHz;
    mEmmcHost.SetClock (&mEmmcHost, 0);
    DwmciWrite (&mEmmcHost, 0x188, (2U << 16) | mEmmcHost.DqsDelay);
    return mEmmcHost.SetClock (&mEmmcHost, Hz);
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TestRead (
  IN  EXYNOS_MMC_TEST_PROTOCOL  *This,
  IN  UINT64                    Lba,
  IN  UINTN                     Blocks,
  OUT VOID                      *Buffer
  )
{
  return MmcReadBlocks (&mEmmcHost, Lba, Blocks, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
TestWrite (
  IN EXYNOS_MMC_TEST_PROTOCOL  *This,
  IN UINT64                    Lba,
  IN UINTN                     Blocks,
  IN VOID                      *Buffer
  )
{
  return MmcWriteBlocks (&mEmmcHost, Lba, Blocks, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
TestReadExtCsd (
  IN  EXYNOS_MMC_TEST_PROTOCOL  *This,
  OUT UINT8                     *Buffer
  )
{
  return MmcRefreshExtCsd (&mEmmcHost, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
TestSetSample (
  IN EXYNOS_MMC_TEST_PROTOCOL  *This,
  IN UINT32                    Phase
  )
{
  mEmmcHost.SamplePhase = Phase & 7;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TestSetOptions (
  IN EXYNOS_MMC_TEST_PROTOCOL  *This,
  IN UINT32                    Uhs1V8,
  IN UINT32                    CardThreshold
  )
{
  mEmmcHost.Uhs1V8      = Uhs1V8 & 1;
  mEmmcHost.ThrOverride = CardThreshold;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TestSetTiming (
  IN EXYNOS_MMC_TEST_PROTOCOL  *This,
  IN UINT32                    DrivePhase,
  IN UINT32                    DdrClockHz
  )
{
  mEmmcHost.DrivePhase = DrivePhase;
  mEmmcHost.DdrClockHz = DdrClockHz;
  return EFI_SUCCESS;
}

STATIC EXYNOS_MMC_TEST_PROTOCOL  mTest = {
  TestGetInfo, TestSetSpeed, TestSetIrq, TestSetClock, TestSetDqs, TestRead, TestWrite, TestReadExtCsd, TestSetTiming, TestSetOptions, TestSetSample
};

/**
  Route the eMMC interrupt (left disabled until the test app switches the
  controller to interrupt mode), hook the CMU for the speed modes and publish
  the test protocol.
**/
STATIC
VOID
EmmcSpeedSetup (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  mEmmcHost.SetSourceClock = EmmcSourceClock;

  Status = gBS->LocateProtocol (&gHardwareInterruptProtocolGuid, NULL, (VOID **)&mGic);
  if (!EFI_ERROR (Status)) {
    Status = mGic->RegisterInterruptSource (mGic, EMMC_IRQ, EmmcInterruptHandler);
    if (!EFI_ERROR (Status)) {
      mGic->DisableInterruptSource (mGic, EMMC_IRQ);
      mIrqRouted = TRUE;
    }
  }
  DEBUG ((DEBUG_INFO, "SdCard: eMMC interrupt %u %a\n", EMMC_IRQ, mIrqRouted ? "routed" : "not available"));

  Handle = NULL;
  gBS->InstallProtocolInterface (&Handle, &gExynosMmcTestProtocolGuid, EFI_NATIVE_INTERFACE, &mTest);
}

//
// ---------------------------------------------------------------------------
// Probe.
// ---------------------------------------------------------------------------
//

STATIC
VOID
ProbeEmmc (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio
  )
{
  EFI_STATUS  Status;
  UINT32      CiuHz;

  EmmcBoardInit (Gpio);
  CiuHz = DwmciClockInit (&mClk0, DWMCI_SDR_TIMING);
  if (CiuHz == 0) {
    DEBUG ((DEBUG_ERROR, "SdCard: eMMC has no clock\n"));
    return;
  }

  Status = DwmciInit (&mEmmcHost, EXYNOS5420_PA_DWMCI0, CiuHz, 8,
                      DWMCI_SDR_TIMING, TRUE);
  if (!EFI_ERROR (Status)) {
    mEmmcBlkIo.Media.MediaId = 0;
    EmmcSpeedSetup ();
    Status = SetupBlockIo (&mEmmcBlkIo, &mEmmcHost, TRUE, &mEmmcDevicePath);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdCard: eMMC init failed - %r\n", Status));
  }
}

STATIC
VOID
ProbeSd (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio
  )
{
  EFI_STATUS  Status;
  UINT32      CiuHz;

  if (!SdCardPresent (Gpio)) {
    DEBUG ((DEBUG_INFO, "SdCard: no microSD card inserted\n"));
    SdPower (Gpio, FALSE);
    return;
  }

  SdBoardInit (Gpio);
  CiuHz = DwmciClockInit (&mClk2, DWMCI_SDR_TIMING);
  if (CiuHz == 0) {
    DEBUG ((DEBUG_ERROR, "SdCard: microSD has no clock\n"));
    SdPower (Gpio, FALSE);
    return;
  }

  Status = DwmciInit (&mSdHost, EXYNOS5420_PA_DWMCI2, CiuHz, 4,
                      DWMCI_SDR_TIMING, FALSE);
  if (!EFI_ERROR (Status)) {
    Status = SetupBlockIo (&mSdBlkIo, &mSdHost, FALSE, &mSdDevicePath);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdCard: microSD init failed - %r\n", Status));
    SdPower (Gpio, FALSE);
  }
}

EFI_STATUS
EFIAPI
SdCardInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EXYNOS_GPIO_PROTOCOL  *Gpio;

  Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&Gpio);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdCard: GPIO protocol unavailable - %r\n", Status));
    return Status;
  }

  ProbeEmmc (Gpio);
  ProbeSd (Gpio);
  return EFI_SUCCESS;
}
