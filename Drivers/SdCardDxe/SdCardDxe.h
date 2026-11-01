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
  buffer; everything is polled.  The SD/MMC command layer (MmcCore.c) and
  the host backend (Dwmci.c) are environment independent (MmcHost.h): the
  Windows eMMC miniport builds the same files.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __SD_CARD_DXE_H__
#define __SD_CARD_DXE_H__

#include "MmcHost.h"

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/ExynosGpio.h>

//
// Host controller register bases (map.h EXYNOS5_PA_HSMMC(x)).
//
#define EXYNOS5420_PA_DWMCI0  0x12200000  // eMMC
#define EXYNOS5420_PA_DWMCI2  0x12220000  // microSD

#endif // __SD_CARD_DXE_H__
