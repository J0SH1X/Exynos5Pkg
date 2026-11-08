/** @file
  DesignWare MMC host + shared SD/MMC command layer: types and prototypes.

  Portable between UEFI (SdCardDxe) and the Windows eMMC miniport
  (~/exynos5/ExynosWinDrivers/exynosemmc), which builds MmcCore.c and Dwmci.c
  from this directory unchanged.  Everything environment specific comes from
  <MmcEnv.h>, found through the include path (angle brackets on purpose):
  this directory's for UEFI, the driver's for Windows.  It provides the UEFI
  base types and library calls used here, plus

    DmaAllocatePages (Pages)  memory the IDMAC can reach (32-bit)
    DMA_ADDRESS (Va)          its bus address

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __MMC_HOST_H__
#define __MMC_HOST_H__

#include <MmcEnv.h>

#define BLOCK_SIZE  512

//
// Largest single read/write command, and the DMA bounce buffer that holds it.
//
#define MMC_MAX_CHUNK_BLOCKS  1024
#define MMC_MAX_DMA_BYTES     (MMC_MAX_CHUNK_BLOCKS * BLOCK_SIZE)

//
// Card types the init layer distinguishes.
//
typedef enum {
  CardTypeNone = 0,
  CardTypeMmc,   // eMMC (CMD1 op-cond path)
  CardTypeSd,    // microSD (ACMD41 op-cond path)
} CARD_TYPE;

//
// Minimal card state after identification.
//
typedef struct {
  CARD_TYPE  Type;
  UINT16     Rca;          // relative card address (SD/MMC, after CMD3)
  BOOLEAN    HighCapacity; // SDHC / block-addressed
  UINT32     BlockSize;    // typically 512
  UINT64     BlockCount;   // total capacity in BlockSize units
  UINT32     BusWidth;     // data lines in use after init (1/4/8)
  UINT8      ExtCsdRev;    // eMMC EXT_CSD fields (0 if not read)
  UINT8      DeviceType;   // EXT_CSD[196]: bit1 HS52, bit6 HS400 1.8V
  UINT8      StrobeSupport;// EXT_CSD[184]
} CARD_STATE;

//
// Host ops: a thin indirection so the shared init/IO layer does not know the
// controller registers.
//
typedef struct _MMC_HOST MMC_HOST;

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_SEND_CMD)(
  IN MMC_HOST  *Host,
  IN UINT32    Cmd,      // raw command index
  IN UINT32    Arg,
  IN UINT32    Flags,    // HOST_CMD_* below
  OUT UINT32   *Resp     // 4 words, may be NULL
  );

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_SET_CLOCK)(
  IN MMC_HOST  *Host,
  IN UINT32    ClockHz
  );

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_PREPARE_DATA)(
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount,
  IN VOID      *Buffer    // writes: the data, copied before the command
  );

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_SET_BUS_WIDTH)(
  IN MMC_HOST  *Host,
  IN UINT32    Width
  );

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_XFER_BLOCK)(
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount,
  OUT VOID     *Buffer
  );

//
// Bus speed modes (eMMC).  The host backend turns a mode into controller
// timing registers; the card side is MmcSetSpeed() in MmcCore.c.
//
typedef enum {
  MmcSpeedDefault = 0,   // legacy, <= 26 MHz, SDR
  MmcSpeedHs,            // high speed, <= 52 MHz, SDR
  MmcSpeedHs400Es,       // HS400 (enhanced strobe if the card has it), 8-bit DDR
  MmcSpeedDdr52,         // 8-bit DDR at 52 MHz
  MmcSpeedHs200,         // HS200: 8-bit SDR up to 200 MHz, sample phase tuned by the caller
  MmcSpeedCount
} MMC_SPEED;

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_SET_SPEED)(
  IN MMC_HOST  *Host,
  IN MMC_SPEED Speed
  );

//
// Environment hook for interrupt-driven completion.  Called by the backend
// when IrqMode is on and no status is pending yet: sleep (or spin) for up to
// WaitUs microseconds or until the ISR has run (it may return early).
//
typedef
VOID
(EFIAPI *MMC_HOST_WAIT_IRQ)(
  IN MMC_HOST  *Host,
  IN UINT32    WaitUs
  );

//
// Environment hook that programs the controller's source clock (CMU) for a
// speed mode and returns the CIU clock the CLKDIV divides.  NULL = the
// clocks stay as the platform set them (only the default speed is valid).
//
typedef
EFI_STATUS
(EFIAPI *MMC_HOST_SET_SOURCE_CLOCK)(
  IN  MMC_HOST  *Host,
  IN  MMC_SPEED Speed,
  IN  UINT32    ClkSel,
  OUT UINT32    *CiuHz
  );

//
// Host command flags.
#define HOST_CMD_RESP_NONE   0x0000
#define HOST_CMD_RESP_SHORT  0x0001
#define HOST_CMD_RESP_LONG   0x0002
#define HOST_CMD_RESP_BUSY   0x0004  // response followed by busy on DAT0
#define HOST_CMD_DATA        0x0010  // this command transfers data
#define HOST_CMD_WRITE       0x0020  // data transfer is card-ward (default read)
#define HOST_CMD_NO_CRC      0x0040  // response CRC invalid by spec (R3: CMD1,
                                    // ACMD41) — do not set the CRC check bit

struct _MMC_HOST {
  UINT32                 Base;        // MMIO base of the host controller
  UINT32                 MaxClockHz;  // controller clock (ciu) feeding CLKDIV
  UINT32                 MaxBusWidth; // data lines wired to the slot
  UINT32                 *DmaDesc;    // IDMAC descriptor chain (4 words each)
  UINT8                  *DmaBounce;  // MMC_MAX_DMA_BYTES transfer buffer
  UINT32                 DmaDescPhys; // their bus addresses for the IDMAC
  UINT32                 DmaBouncePhys;
  CARD_STATE             Card;
  MMC_HOST_SEND_CMD      SendCmd;
  MMC_HOST_SET_CLOCK     SetClock;
  MMC_HOST_PREPARE_DATA  PrepareData;
  MMC_HOST_XFER_BLOCK    XferBlock;
  MMC_HOST_SET_BUS_WIDTH SetBusWidth;

  //
  // Optional, for SDIO CMD53 byte transfers: block size and total byte
  // count of the next data phase.  Zero (the default after DwmciInit) means
  // BLOCK_SIZE and BlockCount * BLOCK_SIZE, which is all SD/MMC memory
  // access uses.  The caller sets both, then clears them again.
  //
  UINT32                 BlockSize;
  UINT32                 ByteCount;

  //
  // Speed modes (SetTiming == NULL: the backend has none).
  //
  MMC_HOST_SET_SPEED         SetSpeed;
  MMC_HOST_SET_SOURCE_CLOCK  SetSourceClock;
  MMC_SPEED                  Speed;
  UINT32                     ClockHz;        // card clock requested last
  UINT32                     DqsDelay;       // HS400 read-strobe delay line (RD_DQS_DELAY_CTRL)
  UINT32                     Uhs1V8;         // UHS_REG bit 0 in the DDR modes (experiments)
  UINT32                     ThrOverride;    // CDTHRCTL for DDR52 / HS400 (0xFFFFFFFF = mode default)
  UINT32                     DrivePhase;     // CLKSEL[18:16] override (0xFF = mode default)
  UINT32                     DdrClockHz;     // card clock for DDR52 (0 = 52 MHz)
  UINT32                     SamplePhase;    // CLKSEL[2:0] (HS400 command/data sample phase)
  BOOLEAN                    Strobe;         // HS400 with enhanced strobe (card STROBE_SUPPORT), else plain HS400

  //
  // Interrupt-driven completion.  The environment connects the controller's
  // interrupt to DwmciIsr (), then calls DwmciIrqEnable ().  The ISR latches
  // RINTSTS into IrqStatus and acknowledges it; the command / data waits look
  // there instead of polling the controller.
  //
  BOOLEAN                    IrqMode;
  volatile UINT32            IrqStatus;
  volatile UINT32            IrqIdSts;
  volatile UINT32            IrqCount;       // interrupts taken
  UINT32                     IrqMissed;      // completions found by the safety poll
  MMC_HOST_WAIT_IRQ          WaitIrq;
};

//
// Shared SD/MMC command layer (MmcCore.c).
//
EFI_STATUS
MmcInitCard (
  IN MMC_HOST  *Host,
  IN BOOLEAN   IsEmmc
  );

EFI_STATUS
MmcReadBlocks (
  IN MMC_HOST  *Host,
  IN EFI_LBA   Lba,
  IN UINTN     Blocks,
  OUT VOID     *Buffer
  );

EFI_STATUS
MmcWriteBlocks (
  IN MMC_HOST  *Host,
  IN EFI_LBA   Lba,
  IN UINTN     Blocks,
  IN VOID      *Buffer
  );

//
// DesignWare MMC host backend (Dwmci.c).
//
EFI_STATUS
DwmciInit (
  IN MMC_HOST  *Host,
  IN UINT32    Base,
  IN UINT32    CiuClockHz,
  IN UINT32    MaxBusWidth,
  IN UINT32    ClkSel,
  IN BOOLEAN   SmuBypass
  );

//
// Switch the eMMC to a speed mode (card + host), proven by reading LBA 0.
// On failure the previous mode is restored.
//
EFI_STATUS
MmcSetSpeed (
  IN MMC_HOST   *Host,
  IN MMC_SPEED  Speed
  );

//
// EXT_CSD of the eMMC (read once at init, refreshed by MmcRefreshExtCsd).
//
EFI_STATUS
MmcRefreshExtCsd (
  IN  MMC_HOST  *Host,
  OUT UINT8     *ExtCsd      // 512 bytes
  );

//
// Interrupt service: call from the controller's interrupt handler.
//
BOOLEAN
DwmciIsr (
  IN MMC_HOST  *Host
  );

//
// Switch between polled and interrupt-driven completion.
//
VOID
DwmciIrqEnable (
  IN MMC_HOST  *Host,
  IN BOOLEAN   Enable
  );

//
// Raw controller access for experiments (clock divider, delay line, ...).
//
UINT32
DwmciRead (
  IN MMC_HOST  *Host,
  IN UINT32    Offset
  );

VOID
DwmciWrite (
  IN MMC_HOST  *Host,
  IN UINT32    Offset,
  IN UINT32    Value
  );

#endif // __MMC_HOST_H__
