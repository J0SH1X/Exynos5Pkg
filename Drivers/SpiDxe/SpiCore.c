/** @file
  Exynos5420 SPI controller core (see SpiCore.h).  Register layout and
  sequence from Linux drivers/spi/spi-s3c64xx.c (flush_fifo(),
  s3c64xx_spi_config(), the PIO path); SPI2's FIFO level fields per
  s3c64xx_spi2_pdata (fifo_lvl_mask 0x7F, rx_lvl_offset 15).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SpiCore.h"

#define SPI_CH_CFG         0x00
#define SPI_CLK_CFG        0x04
#define SPI_MODE_CFG       0x08
#define SPI_SLAVE_SEL      0x0C
#define SPI_INT_EN         0x10
#define SPI_STATUS         0x14
#define SPI_TX_DATA        0x18
#define SPI_RX_DATA        0x1C
#define SPI_PACKET_CNT     0x20
#define SPI_PENDING_CLR    0x24
#define SPI_SWAP_CFG       0x28
#define SPI_FB_CLK         0x2C

#define CH_SW_RST          (1u << 5)
#define CH_CPOL_L          (1u << 3)
#define CH_CPHA_B          (1u << 2)
#define CH_RXCH_ON         (1u << 1)
#define CH_TXCH_ON         (1u << 0)
#define CH_HS_EN           (1u << 6)

#define MODE_TRAILCNT      (0x3FFu << 19)
#define SLAVE_SIG_INACT    (1u << 0)

#define TX_FIFO_LVL(s)     (((s) >> 6) & 0x7F)
#define RX_FIFO_LVL(s)     (((s) >> 15) & 0x7F)

#define CHUNK              32           // bytes in flight (FIFO is 64 deep)
#define SPIN_LIMIT         100000

#define GPIO_CON           0x0
#define GPIO_DAT           0x4
#define GPIO_PUD           0x8

STATIC
VOID
CsLevel (
  IN EXYNOS_SPI  *Spi,
  IN BOOLEAN     High
  )
{
  if (High) {
    MmioOr32 (Spi->CsBank + GPIO_DAT, 1u << Spi->CsPin);
  } else {
    MmioAnd32 (Spi->CsBank + GPIO_DAT, ~(1u << Spi->CsPin));
  }
}

STATIC
VOID
Flush (
  IN EXYNOS_SPI  *Spi
  )
{
  UINTN   Spin;
  UINT32  Status;

  MmioWrite32 (Spi->Base + SPI_PACKET_CNT, 0);
  MmioAnd32 (Spi->Base + SPI_CH_CFG, ~(CH_RXCH_ON | CH_TXCH_ON));
  MmioAndThenOr32 (Spi->Base + SPI_CH_CFG, ~CH_HS_EN, CH_SW_RST);
  for (Spin = 0; Spin < SPIN_LIMIT; Spin++) {
    Status = MmioRead32 (Spi->Base + SPI_STATUS);
    if (RX_FIFO_LVL (Status) != 0) {
      MmioRead32 (Spi->Base + SPI_RX_DATA);
    } else if (TX_FIFO_LVL (Status) == 0) {
      break;
    }
  }
  MmioAnd32 (Spi->Base + SPI_CH_CFG, ~CH_SW_RST);
}

VOID
SpiCoreInit (
  IN OUT EXYNOS_SPI  *Spi
  )
{
  //
  // Chip select: GPIO output, high (inactive), no pull.
  //
  CsLevel (Spi, TRUE);
  MmioAndThenOr32 (Spi->CsBank + GPIO_CON, ~(0xFu << (Spi->CsPin * 4)), 1u << (Spi->CsPin * 4));
  MmioAnd32 (Spi->CsBank + GPIO_PUD, ~(3u << (Spi->CsPin * 2)));

  MmioWrite32 (Spi->Base + SPI_SLAVE_SEL, SLAVE_SIG_INACT);
  MmioWrite32 (Spi->Base + SPI_INT_EN, 0);
  MmioWrite32 (Spi->Base + SPI_PENDING_CLR, 0x1F);
  Flush (Spi);

  //
  // Master, mode 0 (CPOL 0, CPHA 0), byte-wide channel and bus, no swap,
  // feedback clock delay 2 (the board's fb_delay).
  //
  MmioAnd32 (Spi->Base + SPI_CH_CFG, ~(CH_CPOL_L | CH_CPHA_B | (1u << 4)));
  MmioWrite32 (Spi->Base + SPI_MODE_CFG, MODE_TRAILCNT);
  MmioWrite32 (Spi->Base + SPI_SWAP_CFG, 0);
  MmioWrite32 (Spi->Base + SPI_FB_CLK, 2);
}

VOID
SpiCoreSelect (
  IN OUT EXYNOS_SPI  *Spi,
  IN     BOOLEAN     Select
  )
{
  if (Select) {
    Flush (Spi);
    MmioOr32 (Spi->Base + SPI_CH_CFG, CH_RXCH_ON | CH_TXCH_ON);
    MmioWrite32 (Spi->Base + SPI_SLAVE_SEL, 0);
    CsLevel (Spi, FALSE);
  } else {
    CsLevel (Spi, TRUE);
    MmioWrite32 (Spi->Base + SPI_SLAVE_SEL, SLAVE_SIG_INACT);
    Flush (Spi);
  }
}

INTN
SpiCoreExchange (
  IN OUT EXYNOS_SPI   *Spi,
  IN     CONST UINT8  *Tx  OPTIONAL,
  OUT    UINT8        *Rx  OPTIONAL,
  IN     UINTN        Len
  )
{
  UINTN   Done;
  UINTN   Chunk;
  UINTN   Index;
  UINTN   Spin;
  UINT32  Byte;

  for (Done = 0; Done < Len; Done += Chunk) {
    Chunk = MIN ((UINTN)CHUNK, Len - Done);
    for (Index = 0; Index < Chunk; Index++) {
      MmioWrite32 (Spi->Base + SPI_TX_DATA, (Tx != NULL) ? Tx[Done + Index] : 0);
    }
    for (Index = 0; Index < Chunk; Index++) {
      for (Spin = 0; (Spin < SPIN_LIMIT) && (RX_FIFO_LVL (MmioRead32 (Spi->Base + SPI_STATUS)) == 0); Spin++) {
      }
      if (Spin == SPIN_LIMIT) {
        Spi->Timeouts++;
        DEBUG ((DEBUG_ERROR, "Spi: stalled after %u of %u bytes (STATUS 0x%08x)\n", (UINT32)(Done + Index),
                (UINT32)Len, MmioRead32 (Spi->Base + SPI_STATUS)));
        return -1;
      }
      Byte = MmioRead32 (Spi->Base + SPI_RX_DATA);
      if (Rx != NULL) {
        Rx[Done + Index] = (UINT8)Byte;
      }
    }
  }
  return 0;
}

INTN
SpiCoreTransfer (
  IN OUT EXYNOS_SPI   *Spi,
  IN     CONST UINT8  *Tx  OPTIONAL,
  IN     UINTN        TxLen,
  OUT    UINT8        *Rx  OPTIONAL,
  IN     UINTN        RxLen
  )
{
  INTN  Result;

  SpiCoreSelect (Spi, TRUE);
  Result = (TxLen > 0) ? SpiCoreExchange (Spi, Tx, NULL, TxLen) : 0;
  if ((Result == 0) && (RxLen > 0)) {
    Result = SpiCoreExchange (Spi, NULL, Rx, RxLen);
  }
  SpiCoreSelect (Spi, FALSE);
  return Result;
}
