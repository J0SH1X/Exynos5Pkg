/** @file
  Test / tuning access to the eMMC host (SdCardDxe): speed modes, interrupt
  mode and raw block I/O on the same controller state the BlockIo uses.
  Only meant for EmmcTest.efi.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_MMC_TEST_PROTOCOL_H__
#define __EXYNOS_MMC_TEST_PROTOCOL_H__

#define EXYNOS_MMC_TEST_PROTOCOL_GUID                                          \
  {                                                                            \
    0x9d3f4a71, 0x5c20, 0x4b8e,                                                \
    {                                                                          \
      0xa1, 0x77, 0x6e, 0x24, 0x41, 0x10, 0x44, 0x06                           \
    }                                                                          \
  }

typedef struct _EXYNOS_MMC_TEST_PROTOCOL EXYNOS_MMC_TEST_PROTOCOL;

//
// Speed modes (same values as MMC_SPEED in Drivers/SdCardDxe/MmcHost.h).
//
#define EXYNOS_MMC_SPEED_DEFAULT  0
#define EXYNOS_MMC_SPEED_HS       1
#define EXYNOS_MMC_SPEED_HS400ES  2
#define EXYNOS_MMC_SPEED_DDR52    3
#define EXYNOS_MMC_SPEED_HS200    4

typedef struct {
  UINT32  Base;
  UINT32  Speed;
  UINT32  BusWidth;
  UINT32  ClockHz;        // card clock requested last
  UINT32  CiuHz;          // clock CLKDIV divides
  UINT32  IrqMode;        // completion by interrupt
  UINT32  IrqCount;       // interrupts taken
  UINT32  IrqMissed;      // completions only found by the safety poll
  UINT32  DqsDelay;       // HS400 RD_DQS_DELAY_CTRL
  UINT64  BlockCount;
  UINT8   ExtCsdRev;
  UINT8   DeviceType;
  UINT8   StrobeSupport;
  UINT8   Reserved;
} EXYNOS_MMC_INFO;

typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_GET_INFO)(IN EXYNOS_MMC_TEST_PROTOCOL *This, OUT EXYNOS_MMC_INFO *Info);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_SET_SPEED)(IN EXYNOS_MMC_TEST_PROTOCOL *This, IN UINT32 Speed);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_SET_IRQ)(IN EXYNOS_MMC_TEST_PROTOCOL *This, IN BOOLEAN On);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_SET_CLOCK)(IN EXYNOS_MMC_TEST_PROTOCOL *This, IN UINT32 Hz);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_SET_DQS)(IN EXYNOS_MMC_TEST_PROTOCOL *This, IN UINT32 Delay);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_SET_SAMPLE)(IN EXYNOS_MMC_TEST_PROTOCOL *This, IN UINT32 Phase);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_SET_OPTIONS)(IN EXYNOS_MMC_TEST_PROTOCOL *This, IN UINT32 Uhs1V8, IN UINT32 CardThreshold);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_SET_TIMING)(IN EXYNOS_MMC_TEST_PROTOCOL *This, IN UINT32 DrivePhase, IN UINT32 DdrClockHz);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_READ)(IN EXYNOS_MMC_TEST_PROTOCOL *This, IN UINT64 Lba, IN UINTN Blocks, OUT VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_WRITE)(IN EXYNOS_MMC_TEST_PROTOCOL *This, IN UINT64 Lba, IN UINTN Blocks, IN VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EXYNOS_MMC_READ_EXTCSD)(IN EXYNOS_MMC_TEST_PROTOCOL *This, OUT UINT8 *Buffer);

struct _EXYNOS_MMC_TEST_PROTOCOL {
  EXYNOS_MMC_GET_INFO     GetInfo;
  EXYNOS_MMC_SET_SPEED    SetSpeed;
  EXYNOS_MMC_SET_IRQ      SetIrq;
  EXYNOS_MMC_SET_CLOCK    SetClock;
  EXYNOS_MMC_SET_DQS      SetDqsDelay;
  EXYNOS_MMC_READ         Read;
  EXYNOS_MMC_WRITE        Write;
  EXYNOS_MMC_READ_EXTCSD  ReadExtCsd;
  EXYNOS_MMC_SET_TIMING   SetTiming;          // CLKSEL drive phase (0xFF = default), DDR52 card clock (0 = 52 MHz)
  EXYNOS_MMC_SET_OPTIONS  SetOptions;         // UHS_REG bit 0 and CDTHRCTL (0xFFFFFFFF = default) for the DDR modes
  EXYNOS_MMC_SET_SAMPLE   SetSamplePhase;     // applies at the next switch to HS400
};

extern EFI_GUID gExynosMmcTestProtocolGuid;

#endif
