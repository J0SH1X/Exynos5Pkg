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
// Host command flags.
//
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

#endif // __MMC_HOST_H__
