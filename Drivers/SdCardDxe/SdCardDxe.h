/** @file
  Exynos5420 eMMC + microSD storage driver (v1awifi).

  Exposes each card as an EFI_BLOCK_IO_PROTOCOL.  Both slots sit on Synopsys
  DesignWare MMC ("dw_mmc") controllers (board-universal5420-mmc.c):

    - eMMC    -> dw_mmc channel 0 at 0x12200000, CD_PERMANENT, 8-bit bus.
    - microSD -> dw_mmc channel 2 at 0x12220000, card detect on GPX2(4)
                 (low = card present), slot power switched by GPY2(0)
                 (GPIO_TF_EN), 4-bit bus.

  The microSD slot is only powered and initialised when the card-detect pin
  says a card is inserted; an empty slot is skipped and left unpowered.

  Data moves by the controller's internal DMA (IDMAC) through a bounce
  buffer; everything is polled.  The SD/MMC command layer (MmcCore.c) is shared
  with Exynos441xPkg's SdCardDxe; Dwmci.c is the host backend.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __SD_CARD_DXE_H__
#define __SD_CARD_DXE_H__

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/ExynosGpio.h>

//
// Host controller register bases (map.h EXYNOS5_PA_HSMMC(x)).
//
#define EXYNOS5420_PA_DWMCI0  0x12200000  // eMMC
#define EXYNOS5420_PA_DWMCI2  0x12220000  // microSD

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
  CARD_STATE             Card;
  MMC_HOST_SEND_CMD      SendCmd;
  MMC_HOST_SET_CLOCK     SetClock;
  MMC_HOST_PREPARE_DATA  PrepareData;
  MMC_HOST_XFER_BLOCK    XferBlock;
  MMC_HOST_SET_BUS_WIDTH SetBusWidth;
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

#endif // __SD_CARD_DXE_H__
