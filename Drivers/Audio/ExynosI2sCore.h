/** @file
  Exynos5420 audio subsystem playback: I2S0 as clock slave, fed by its
  internal DMA (IDMA) from a circular DRAM buffer.  Shared by UEFI
  (AudioTestApp) and Windows (ExynosWinDrivers/exynosaudio), built
  unchanged in both through <ExynosEnv.h>.

  As the stock kernel runs it (sound/soc/samsung/i2s.c + idma.c with
  CONFIG_SND_SAMSUNG_USE_IDMA_DRAM, "samsung-i2s.4" = I2S0's secondary TX
  FIFO): the codec drives BCLK / LRCLK, I2S0 only shifts out what IDMA
  fetches.  IDMA loops over [Start, Start + Size) by itself (AHB_DMARLD);
  its word counter is the play position, and the level-0 interrupt
  (I2S0: GIC SPI 115) can mark periods.

  The caller maps the four register blocks below.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_I2S_CORE_H__
#define __EXYNOS_I2S_CORE_H__

#include <ExynosEnv.h>

#define EXYNOS_I2S0_PHYS       0x03830000
#define EXYNOS_AUDSS_PHYS      0x03810000   // audio subsystem clock control
#define EXYNOS_GPZ_PHYS        0x03860000   // audio GPIO block (GPZ = I2S0 pins)
#define EXYNOS_PMU_PHYS        0x10040000
#define EXYNOS_I2S0_GSI        (115 + 32)

typedef struct {
  UINTN   I2s;
  UINTN   Audss;
  UINTN   Gpz;
  UINTN   Pmu;
  UINT32  BufferBytes;   // of the running stream
  UINT32  PeriodBytes;
  UINT32  Start;
  BOOLEAN TxOn;
  BOOLEAN RxOn;
} EXYNOS_I2S;

/**
  MAU (audio) power domain on and its pads out of retention, audio
  subsystem clocks on, GPZ0-4 to I2S0, and MCLK1 for the codec: the 24 MHz
  crystal on the CLKOUT pin (PMU DEBUG).  0 or -1 (domain did not power up).
**/
INTN
ExynosI2sPowerUp (
  IN OUT EXYNOS_I2S  *I2s
  );

/**
  I2S0 as clock slave, I2S format, 16-bit stereo, TX only, idle.
**/
VOID
ExynosI2sConfigure (
  IN OUT EXYNOS_I2S  *I2s
  );

/**
  Start IDMA over the physically contiguous buffer at Start (Bytes, a
  multiple of PeriodBytes and of 4, at most 256 KiB) and the secondary TX
  channel.  Level-0 interrupt after the first period if Interrupt.
**/
VOID
ExynosI2sStart (
  IN OUT EXYNOS_I2S  *I2s,
  IN     UINT32      Start,
  IN     UINT32      Bytes,
  IN     UINT32      PeriodBytes,
  IN     BOOLEAN     Interrupt
  );

VOID
ExynosI2sStop (
  IN OUT EXYNOS_I2S  *I2s
  );

/**
  Receive (the primary RX FIFO, I2SRXD): on / off, independent of the
  playback side (MOD becomes TX+RX / RX only as needed).  Dma: raise the
  RX DMA request (for the PL330); FALSE for PIO reads.
**/
VOID
ExynosI2sRxStart (
  IN OUT EXYNOS_I2S  *I2s,
  IN     BOOLEAN     Dma
  );

VOID
ExynosI2sRxStop (
  IN OUT EXYNOS_I2S  *I2s
  );

/**
  PIO: up to Words 32-bit FIFO entries (one stereo 16-bit frame each) into
  Buffer, waiting at most TimeoutUs for each.  Returns the entries read.
**/
UINT32
ExynosI2sRxRead (
  IN  EXYNOS_I2S  *I2s,
  OUT UINT32      *Buffer,
  IN  UINT32      Words,
  IN  UINT32      TimeoutUs
  );

/**
  Physical address of the RX FIFO data register (DMA source).
**/
#define EXYNOS_I2S0_RXD_PHYS   (EXYNOS_I2S0_PHYS + 0x14)

/**
  Byte offset in the buffer that IDMA reads next (idma_getpos()).
**/
UINT32
ExynosI2sPosition (
  IN EXYNOS_I2S  *I2s
  );

/**
  Interrupt half: TRUE if the level-0 interrupt was pending (acknowledged,
  next mark one period further).  Registers only, any IRQL.
**/
BOOLEAN
ExynosI2sInterrupt (
  IN OUT EXYNOS_I2S  *I2s
  );

VOID
ExynosI2sLogStatus (
  IN EXYNOS_I2S  *I2s
  );

#endif // __EXYNOS_I2S_CORE_H__
