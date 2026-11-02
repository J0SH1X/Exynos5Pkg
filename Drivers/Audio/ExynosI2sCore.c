/** @file
  Exynos5420 I2S0 + IDMA playback core (see ExynosI2sCore.h).  Registers
  and sequences from the Samsung v1awifi kernel: sound/soc/samsung/
  i2s-regs.h, i2s.c (i2s_txctrl(), i2s_set_fmt(), i2s_hw_params()), idma.c
  (idma_hw_params(), idma_enqueue(), idma_control(), iis_irq(),
  idma_getpos()); mach-exynos/regs-audss.h, clock-audss.c,
  pm_domains5.c (pd-maudio = MAU_CONFIGURATION 0x40E0), pm.c (pad
  retention release), mach-exynos5-audio.c + clock-exynos5420.c (CLKOUT =
  PMU DEBUG 0xA00, source 16 = XXTI), dev-audio.c (GPZ0.. function 2).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "ExynosI2sCore.h"

//
// I2S0.
//
#define I2SCON              0x00
#define I2SMOD              0x04
#define I2SFIC              0x08
#define I2SRXD              0x14
#define I2SFICS             0x18
#define I2SAHB              0x20
#define I2SSTR0             0x24
#define I2SSIZE             0x28
#define I2STRNCNT           0x2C
#define I2SLVL0ADDR         0x30

#define CON_RSTCLR          (1u << 31)     // out of reset (QUIRK_NEED_RSTCLR: i2sv5)
#define CON_TXSDMA_PAUSE    (1u << 20)
#define CON_TXSDMA_ACTIVE   (1u << 18)
#define CON_FTXURSTATUS     (1u << 17)
#define CON_TXDMA_PAUSE     (1u << 6)
#define CON_RXDMA_PAUSE     (1u << 5)
#define CON_TXCH_PAUSE      (1u << 4)
#define CON_RXCH_PAUSE      (1u << 3)
#define CON_RXFIFO_EMPTY    (1u << 9)
#define CON_RXDMA_ACTIVE    (1u << 1)
#define CON_ACTIVE          (1u << 0)

#define MOD_OPCLK_PCLK      (3u << 30)
#define MOD_TXS_IDMA        (1u << 28)
#define MOD_SLAVE           (1u << 11)
#define MOD_TXONLY          (0u << 8)
#define MOD_RXONLY          (1u << 8)
#define MOD_TXRX            (2u << 8)
#define MOD_MASK            (3u << 8)
#define MOD_SDF_IIS         (0u << 6)
#define MOD_RCLK_256FS      (0u << 4)
#define MOD_BCLK_32FS       (0u << 0)
// BLCS / BLCP / BLC = 0: 16-bit samples on the secondary, primary channel.

#define FIC_TXFLUSH         (1u << 15)
#define FIC_RXFLUSH         (1u << 7)

#define AHB_INTENLVL0       (1u << 24)
#define AHB_LVL0INT         (1u << 20)
#define AHB_CLRLVL0INT      (1u << 16)
#define AHB_DMARLD          (1u << 5)
#define AHB_INTMASK         (1u << 3)
#define AHB_DMAEN           (1u << 0)

#define SIZE_SHIFT          16
#define SIZE_MASK           0xFFFFu

//
// Audio subsystem clock control.
//
#define AUDSS_CLKSRC        0x0
#define AUDSS_CLKDIV        0x4
#define AUDSS_CLKGATE       0x8
#define AUDSS_GATE_RP       (1u << 0)
#define AUDSS_GATE_I2SBUS   (1u << 2)
#define AUDSS_GATE_I2SSPEC  (1u << 3)
#define AUDSS_GATE_GPIO     (1u << 6)

//
// PMU.
//
#define PMU_DEBUG           0x0A00         // CLKOUT: source [12:8], disable bit 0
#define PMU_CLKOUT_XXTI     16
#define PMU_PAD_RET_MAUDIO  0x3028
#define PMU_MAU_CONFIG      0x40E0
#define PMU_MAU_STATUS      0x40E4
#define PAD_RET_RELEASE     (1u << 28)

#define GPIO_CON            0x0
#define GPIO_PUD            0x8

INTN
ExynosI2sPowerUp (
  IN OUT EXYNOS_I2S  *I2s
  )
{
  UINTN   Try;
  UINT32  Pin;

  if ((MmioRead32 (I2s->Pmu + PMU_MAU_STATUS) & 0x7) != 0x7) {
    MmioWrite32 (I2s->Pmu + PMU_MAU_CONFIG, 0x7);
    for (Try = 0; (Try < 100) && ((MmioRead32 (I2s->Pmu + PMU_MAU_STATUS) & 0x7) != 0x7); Try++) {
      MicroSecondDelay (100);
    }
    if ((MmioRead32 (I2s->Pmu + PMU_MAU_STATUS) & 0x7) != 0x7) {
      DEBUG ((DEBUG_ERROR, "ExynosI2s: MAU power domain stays off (status 0x%x)\n",
              MmioRead32 (I2s->Pmu + PMU_MAU_STATUS)));
      return -1;
    }
  }
  MmioWrite32 (I2s->Pmu + PMU_PAD_RET_MAUDIO, PAD_RET_RELEASE);

  //
  // MOUT_AUDSS = crystal, MOUT_I2S = MOUT_AUDSS (the codec is the clock
  // master; I2S0 needs its bus clock and, to be safe, the special clock).
  //
  MmioAnd32 (I2s->Audss + AUDSS_CLKSRC, ~((1u << 0) | (3u << 2)));
  MmioOr32 (I2s->Audss + AUDSS_CLKGATE, AUDSS_GATE_RP | AUDSS_GATE_I2SBUS | AUDSS_GATE_I2SSPEC | AUDSS_GATE_GPIO);

  //
  // GPZ0 SCLK, GPZ1 CDCLK, GPZ2 LRCK, GPZ3 SDI, GPZ4 SDO0: function 2.
  //
  for (Pin = 0; Pin <= 4; Pin++) {
    MmioAndThenOr32 (I2s->Gpz + GPIO_CON, ~(0xFu << (Pin * 4)), 2u << (Pin * 4));
    MmioAnd32 (I2s->Gpz + GPIO_PUD, ~(3u << (Pin * 2)));
  }

  //
  // MCLK1 = XXTI (24 MHz) on CLKOUT.
  //
  MmioAndThenOr32 (I2s->Pmu + PMU_DEBUG, ~((0x1Fu << 8) | 1u), PMU_CLKOUT_XXTI << 8);
  return 0;
}

VOID
ExynosI2sConfigure (
  IN OUT EXYNOS_I2S  *I2s
  )
{
  //
  // i2s_startup(): the v5 block (QUIRK_NEED_RSTCLR) sits in reset until
  // CON_RSTCLR is written; every later CON write keeps it.
  //
  MmioWrite32 (I2s->I2s + I2SCON, CON_RSTCLR);
  MmioWrite32 (I2s->I2s + I2SAHB, 0);
  MmioWrite32 (I2s->I2s + I2SCON, CON_RSTCLR | CON_TXSDMA_PAUSE | CON_TXDMA_PAUSE | CON_RXDMA_PAUSE | CON_TXCH_PAUSE |
               CON_RXCH_PAUSE);
  MmioWrite32 (I2s->I2s + I2SMOD, MOD_OPCLK_PCLK | MOD_SLAVE | MOD_TXONLY | MOD_SDF_IIS | MOD_RCLK_256FS | MOD_BCLK_32FS);
  MmioWrite32 (I2s->I2s + I2SFIC, FIC_TXFLUSH);
  MmioWrite32 (I2s->I2s + I2SFIC, 0);
  MmioWrite32 (I2s->I2s + I2SFICS, FIC_TXFLUSH);
  MmioWrite32 (I2s->I2s + I2SFICS, 0);
}

VOID
ExynosI2sStart (
  IN OUT EXYNOS_I2S  *I2s,
  IN     UINT32      Start,
  IN     UINT32      Bytes,
  IN     UINT32      PeriodBytes,
  IN     BOOLEAN     Interrupt
  )
{
  UINT32  Con;

  I2s->Start       = Start;
  I2s->BufferBytes = Bytes;
  I2s->PeriodBytes = PeriodBytes;

  //
  // idma_hw_params() + idma_enqueue(): reload at the end, IDMA feeds the
  // secondary TX FIFO, level-0 mark one period in.
  //
  MmioWrite32 (I2s->I2s + I2SAHB, AHB_DMARLD | AHB_INTMASK);
  MmioOr32 (I2s->I2s + I2SMOD, MOD_TXS_IDMA);
  MmioWrite32 (I2s->I2s + I2SLVL0ADDR, Start + PeriodBytes);
  MmioWrite32 (I2s->I2s + I2SSTR0, Start);
  MmioAndThenOr32 (I2s->I2s + I2SSIZE, ~(SIZE_MASK << SIZE_SHIFT), ((Bytes >> 2) & SIZE_MASK) << SIZE_SHIFT);

  //
  // idma_control(START), then i2s_txctrl(on) for the secondary channel.
  //
  MmioOr32 (I2s->I2s + I2SAHB, AHB_DMAEN | (Interrupt ? (AHB_INTENLVL0 | AHB_CLRLVL0INT) : 0));
  Con  = MmioRead32 (I2s->I2s + I2SCON);
  Con |= CON_RSTCLR | CON_ACTIVE | CON_TXSDMA_ACTIVE;
  Con &= ~(CON_TXCH_PAUSE | CON_TXSDMA_PAUSE);
  MmioAndThenOr32 (I2s->I2s + I2SMOD, ~MOD_MASK, I2s->RxOn ? MOD_TXRX : MOD_TXONLY);
  MmioWrite32 (I2s->I2s + I2SCON, Con);
  I2s->TxOn = TRUE;
}

VOID
ExynosI2sStop (
  IN OUT EXYNOS_I2S  *I2s
  )
{
  UINT32  Con;

  Con  = MmioRead32 (I2s->I2s + I2SCON);
  Con |= CON_RSTCLR | CON_TXSDMA_PAUSE | CON_TXCH_PAUSE;
  Con &= ~CON_TXSDMA_ACTIVE;
  if (!I2s->RxOn) {
    Con &= ~CON_ACTIVE;
  } else {
    MmioAndThenOr32 (I2s->I2s + I2SMOD, ~MOD_MASK, MOD_RXONLY);
  }
  MmioWrite32 (I2s->I2s + I2SCON, Con);
  I2s->TxOn = FALSE;
  MmioAnd32 (I2s->I2s + I2SAHB, ~(AHB_INTENLVL0 | AHB_DMAEN));
  MmioWrite32 (I2s->I2s + I2SFICS, FIC_TXFLUSH);
  MmioWrite32 (I2s->I2s + I2SFICS, 0);
  I2s->BufferBytes = 0;
}

VOID
ExynosI2sRxStart (
  IN OUT EXYNOS_I2S  *I2s,
  IN     BOOLEAN     Dma
  )
{
  UINT32  Con;

  MmioWrite32 (I2s->I2s + I2SFIC, MmioRead32 (I2s->I2s + I2SFIC) | FIC_RXFLUSH);
  MmioWrite32 (I2s->I2s + I2SFIC, MmioRead32 (I2s->I2s + I2SFIC) & ~FIC_RXFLUSH);

  //
  // i2s_rxctrl(on).
  //
  Con  = MmioRead32 (I2s->I2s + I2SCON);
  Con |= CON_RSTCLR | CON_ACTIVE | (Dma ? CON_RXDMA_ACTIVE : 0);
  Con &= ~(CON_RXCH_PAUSE | (Dma ? CON_RXDMA_PAUSE : 0));
  MmioAndThenOr32 (I2s->I2s + I2SMOD, ~MOD_MASK, I2s->TxOn ? MOD_TXRX : MOD_RXONLY);
  MmioWrite32 (I2s->I2s + I2SCON, Con);
  I2s->RxOn = TRUE;
}

VOID
ExynosI2sRxStop (
  IN OUT EXYNOS_I2S  *I2s
  )
{
  UINT32  Con;

  Con  = MmioRead32 (I2s->I2s + I2SCON);
  Con |= CON_RSTCLR | CON_RXDMA_PAUSE | CON_RXCH_PAUSE;
  Con &= ~CON_RXDMA_ACTIVE;
  if (!I2s->TxOn) {
    Con &= ~CON_ACTIVE;
  } else {
    MmioAndThenOr32 (I2s->I2s + I2SMOD, ~MOD_MASK, MOD_TXONLY);
  }
  MmioWrite32 (I2s->I2s + I2SCON, Con);
  I2s->RxOn = FALSE;
}

UINT32
ExynosI2sRxRead (
  IN  EXYNOS_I2S  *I2s,
  OUT UINT32      *Buffer,
  IN  UINT32      Words,
  IN  UINT32      TimeoutUs
  )
{
  UINT32  Done;
  UINT32  Waited;

  for (Done = 0; Done < Words; Done++) {
    for (Waited = 0; (MmioRead32 (I2s->I2s + I2SCON) & CON_RXFIFO_EMPTY) != 0; Waited++) {
      if (Waited >= TimeoutUs) {
        return Done;
      }
      MicroSecondDelay (1);
    }
    Buffer[Done] = MmioRead32 (I2s->I2s + I2SRXD);
  }
  return Done;
}

UINT32
ExynosI2sPosition (
  IN EXYNOS_I2S  *I2s
  )
{
  UINT32  Words;
  UINT32  Max;

  if (I2s->BufferBytes == 0) {
    return 0;
  }
  Max   = I2s->BufferBytes >> 2;
  Words = MmioRead32 (I2s->I2s + I2STRNCNT) & 0xFFFFFF;

  //
  // The counter can run one word ahead of the transfer (idma_getpos()).
  //
  Words = (Words == 0) ? (Max - 1) : (Words - 1);
  return (Words % Max) << 2;
}

BOOLEAN
ExynosI2sInterrupt (
  IN OUT EXYNOS_I2S  *I2s
  )
{
  UINT32  Ahb;
  UINT32  Mark;

  Ahb = MmioRead32 (I2s->I2s + I2SAHB);
  if ((Ahb & AHB_LVL0INT) == 0) {
    return FALSE;
  }
  MmioWrite32 (I2s->I2s + I2SAHB, Ahb | AHB_CLRLVL0INT);
  if ((I2s->BufferBytes != 0) && (I2s->PeriodBytes != 0)) {
    Mark = MmioRead32 (I2s->I2s + I2SLVL0ADDR) - I2s->Start + I2s->PeriodBytes;
    MmioWrite32 (I2s->I2s + I2SLVL0ADDR, I2s->Start + (Mark % I2s->BufferBytes));
  }
  return TRUE;
}

VOID
ExynosI2sLogStatus (
  IN EXYNOS_I2S  *I2s
  )
{
  DEBUG ((DEBUG_INFO, "ExynosI2s: CON 0x%08x MOD 0x%08x AHB 0x%08x TRNCNT 0x%x FICS 0x%08x MAU 0x%x CLKGATE 0x%x\n",
          MmioRead32 (I2s->I2s + I2SCON), MmioRead32 (I2s->I2s + I2SMOD), MmioRead32 (I2s->I2s + I2SAHB),
          MmioRead32 (I2s->I2s + I2STRNCNT), MmioRead32 (I2s->I2s + I2SFICS),
          MmioRead32 (I2s->Pmu + PMU_MAU_STATUS), MmioRead32 (I2s->Audss + AUDSS_CLKGATE)));
}
