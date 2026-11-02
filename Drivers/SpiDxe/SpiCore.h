/** @file
  Exynos5420 SPI controller (s3c64xx-style, polled, byte mode), shared by
  UEFI (SpiDxe) and Windows (ExynosWinDrivers/exynosspi, an SpbCx
  controller), built unchanged in both through <ExynosEnv.h>.

  The caller maps the registers and owns clocks and pin functions (UEFI's
  SpiDxe sets them up at boot; Windows leaves them as UEFI left them, like
  exynosi2c).  The chip select is a plain GPIO (the kernel's
  exynos_spi_cfg_cs()), driven here.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __SPI_CORE_H__
#define __SPI_CORE_H__

#include <ExynosEnv.h>

typedef struct {
  UINTN   Base;        // controller registers
  UINTN   CsBank;      // GPIO bank of the chip select (CON +0x0, DAT +0x4, PUD +0x8)
  UINT32  CsPin;
  UINT32  Timeouts;    // for the logs
} EXYNOS_SPI;

/**
  Reset the controller, mode 0 / byte transfers, chip select high (output).
**/
VOID
SpiCoreInit (
  IN OUT EXYNOS_SPI  *Spi
  );

/**
  Chip select low (TRUE, channels on) or high (FALSE, channels off, FIFOs
  flushed).
**/
VOID
SpiCoreSelect (
  IN OUT EXYNOS_SPI  *Spi,
  IN     BOOLEAN     Select
  );

/**
  Full duplex while selected: Len bytes out of Tx (zeros if NULL), Len
  bytes into Rx (dropped if NULL).  0, or -1 when the controller stalls.
**/
INTN
SpiCoreExchange (
  IN OUT EXYNOS_SPI   *Spi,
  IN     CONST UINT8  *Tx  OPTIONAL,
  OUT    UINT8        *Rx  OPTIONAL,
  IN     UINTN        Len
  );

/**
  Select, TxLen bytes out, RxLen bytes in, deselect.  0 or -1.
**/
INTN
SpiCoreTransfer (
  IN OUT EXYNOS_SPI   *Spi,
  IN     CONST UINT8  *Tx  OPTIONAL,
  IN     UINTN        TxLen,
  OUT    UINT8        *Rx  OPTIONAL,
  IN     UINTN        RxLen
  );

#endif // __SPI_CORE_H__
