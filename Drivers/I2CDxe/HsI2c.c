/** @file
  Exynos5420 high-speed I2C (exynos5-hs-i2c) polling master.

  Channels 0..6 (Linux buses 4..10).  Register model and transfer sequence
  follow the downstream kernel's drivers/i2c/busses/i2c-exynos5.c in its
  HSI2C_POLLING mode: per message, program direction, slave address, length
  and STOP-after-transfer in AUTO_CONFIG, set MASTER_RUN, then move bytes
  through the FIFOs and wait for the transfer to drain.  A message that is
  not the last one is sent without STOP, so the next one starts with a
  repeated start (register-address write followed by a read).

  Timing is computed for 400 kHz from ACLK_66_PERIC exactly like
  exynos5_i2c_set_timing().  Pin muxing is the caller's job.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include <Protocol/ExynosI2c.h>

#include "HsI2c.h"

//
// Channel bases (map.h EXYNOS5420_PA_HSIICx) and IP_PERIC gate bits
// (clock-exynos5420.c exynos5-hs-i2c.0..6).
//
STATIC CONST UINT32  mHsI2cBase[] = {
  0x12CA0000, 0x12CB0000, 0x12CC0000, 0x12CD0000,
  0x12E00000, 0x12E10000, 0x12E20000
};
STATIC CONST UINT8   mHsI2cGate[] = { 10, 11, 12, 13, 28, 30, 31 };

#define CLKGATE_IP_PERIC  0x10020950

//
// Registers (i2c-exynos5.h).
//
#define HSI2C_CTL           0x00
#define HSI2C_FIFO_CTL      0x04
#define HSI2C_TRAILING_CTL  0x08
#define HSI2C_INT_ENABLE    0x20
#define HSI2C_FIFO_STATUS   0x30
#define HSI2C_TX_DATA       0x34
#define HSI2C_RX_DATA       0x38
#define HSI2C_CONF          0x40
#define HSI2C_AUTO_CONFIG   0x44
#define HSI2C_TIMEOUT       0x48
#define HSI2C_TRANS_STATUS  0x50
#define HSI2C_TIMING_FS1    0x60
#define HSI2C_TIMING_FS2    0x64
#define HSI2C_TIMING_FS3    0x68
#define HSI2C_TIMING_SLA    0x6C
#define HSI2C_ADDR          0x70

#define CTL_FUNC_MODE_I2C   (1U << 0)
#define CTL_MASTER          (1U << 3)
#define CTL_RXCHON          (1U << 6)
#define CTL_TXCHON          (1U << 7)
#define CTL_SW_RST          (1U << 31)

#define FIFO_CTL_RX_EN      (1U << 0)
#define FIFO_CTL_TX_EN      (1U << 1)
#define FIFO_CTL_TX_TRIG    (0x20U << 16)
#define FIFO_CTL_RX_TRIG    (0x20U << 4)

#define FIFO_STAT_TX_LEVEL(s)  ((s) & 0x7F)
#define FIFO_STAT_RX_EMPTY     (1U << 24)
#define FIFO_STAT_ALL_EMPTY    0x01000100U

#define AUTO_READ_WRITE     (1U << 16)
#define AUTO_STOP_AFTER     (1U << 17)
#define AUTO_MASTER_RUN     (1U << 31)

#define TIMEOUT_EN          (1U << 31)
#define TRANS_STATUS_IDLE_NO_STOP  0x20000U

#define HSI2C_PCLK_HZ       66600000U
#define HSI2C_BUS_HZ        400000U
#define HSI2C_MSG_TIMEOUT_US  50000

STATIC BOOLEAN  mHsI2cReady[ARRAY_SIZE (mHsI2cBase)];

/**
  exynos5_i2c_set_timing() for fast mode.
**/
STATIC
VOID
HsI2cSetTiming (
  IN UINT32  Base
  )
{
  UINT32  Filter;
  UINT32  Temp0;
  UINT32  Temp1;
  UINT32  Temp2;
  UINT32  Div;
  UINT32  SclL;

  Filter = (MmioRead32 (Base + HSI2C_CONF) >> 16) & 0x7;
  Temp0  = (HSI2C_PCLK_HZ / HSI2C_BUS_HZ) - 8 - 2 * Filter;
  Temp2  = 0;
  for (Div = 0; Div < 256; Div++) {
    Temp1 = Temp0 / (Div + 1);
    if (Temp1 < 128) {
      Temp2 = Temp1 - 2;
      break;
    }
  }
  SclL = Temp2 / 2;

  MmioWrite32 (Base + HSI2C_TIMING_FS1, (SclL << 24) | (SclL << 16) | (SclL << 8));
  MmioWrite32 (Base + HSI2C_TIMING_FS2, ((SclL / 2) << 24) | (SclL << 8) | (Temp2 / 2));
  MmioWrite32 (Base + HSI2C_TIMING_FS3, (Div << 16) | Temp2);
  MmioWrite32 (Base + HSI2C_TIMING_SLA, SclL / 2);
}

/**
  exynos5_i2c_init(), plus a soft reset (exynos5_i2c_reset) first.
**/
STATIC
VOID
HsI2cInit (
  IN UINT32  Base
  )
{
  MmioOr32 (Base + HSI2C_CTL, CTL_SW_RST);
  MmioAnd32 (Base + HSI2C_CTL, ~CTL_SW_RST);

  MmioOr32 (Base + HSI2C_CONF, 0x7U << 16);
  MmioWrite32 (Base + HSI2C_CTL, CTL_FUNC_MODE_I2C | CTL_MASTER);
  MmioWrite32 (Base + HSI2C_TRAILING_CTL, 0xF);
  MmioWrite32 (Base + HSI2C_INT_ENABLE, 0);
  HsI2cSetTiming (Base);
}

/**
  One message.  Stop: end with STOP (last message), else leave the bus for
  a repeated start.

  @return 0 on success, 2 on timeout (NACK shows up as a timeout too).
**/
STATIC
INTN
HsI2cDoMsg (
  IN UINT32          Base,
  IN EXYNOS_I2C_MSG  *Msg,
  IN BOOLEAN         Stop
  )
{
  BOOLEAN  IsRead;
  UINT32   Ctl;
  UINT32   Auto;
  UINT32   Fifo;
  UINT32   Trans;
  UINTN    Index;
  UINTN    Waited;

  IsRead = (Msg->Flags & I2C_M_RD) != 0;

  MmioAnd32 (Base + HSI2C_TIMEOUT, ~TIMEOUT_EN);
  MmioWrite32 (Base + HSI2C_FIFO_CTL, FIFO_CTL_RX_EN | FIFO_CTL_TX_EN |
               FIFO_CTL_TX_TRIG | FIFO_CTL_RX_TRIG);

  Ctl  = MmioRead32 (Base + HSI2C_CTL) & ~(CTL_RXCHON | CTL_TXCHON);
  Ctl |= IsRead ? CTL_RXCHON : CTL_TXCHON;
  Auto = MmioRead32 (Base + HSI2C_AUTO_CONFIG) & ~(AUTO_READ_WRITE | AUTO_STOP_AFTER | 0xFFFF);
  Auto |= IsRead ? AUTO_READ_WRITE : 0;
  Auto |= Stop ? AUTO_STOP_AFTER : 0;
  Auto |= Msg->Len;

  MmioWrite32 (Base + HSI2C_ADDR, ((UINT32)Msg->Addr & 0x7F) << 10);
  MmioWrite32 (Base + HSI2C_CTL, Ctl);
  MmioWrite32 (Base + HSI2C_AUTO_CONFIG, Auto);
  MmioWrite32 (Base + HSI2C_AUTO_CONFIG, Auto | AUTO_MASTER_RUN);

  Index  = 0;
  Waited = 0;
  if (IsRead) {
    while (Index < Msg->Len) {
      if ((MmioRead32 (Base + HSI2C_FIFO_STATUS) & FIFO_STAT_RX_EMPTY) == 0) {
        Msg->Buf[Index++] = (UINT8)MmioRead32 (Base + HSI2C_RX_DATA);
        continue;
      }
      if (Waited++ >= HSI2C_MSG_TIMEOUT_US) {
        return 2;
      }
      MicroSecondDelay (1);
    }
    return 0;
  }

  while (Index < Msg->Len) {
    if (FIFO_STAT_TX_LEVEL (MmioRead32 (Base + HSI2C_FIFO_STATUS)) < 64) {
      MmioWrite32 (Base + HSI2C_TX_DATA, Msg->Buf[Index++]);
      continue;
    }
    if (Waited++ >= HSI2C_MSG_TIMEOUT_US) {
      return 2;
    }
    MicroSecondDelay (1);
  }

  for (Waited = 0; Waited < HSI2C_MSG_TIMEOUT_US; Waited++) {
    Fifo  = MmioRead32 (Base + HSI2C_FIFO_STATUS);
    Trans = MmioRead32 (Base + HSI2C_TRANS_STATUS);
    if ((Fifo == FIFO_STAT_ALL_EMPTY) &&
        ((Trans == 0) || (!Stop && (Trans == TRANS_STATUS_IDLE_NO_STOP)))) {
      return 0;
    }
    MicroSecondDelay (1);
  }
  return 2;
}

/**
  Transfer on hs-i2c channel Channel (0..6).

  @return Num on success, -1 on error.
**/
INTN
HsI2cXfer (
  IN UINT32          Channel,
  IN EXYNOS_I2C_MSG  *Msgs,
  IN INTN            Num
  )
{
  UINT32  Base;
  INTN    Index;
  INTN    Ret;

  if (Channel >= ARRAY_SIZE (mHsI2cBase)) {
    return -1;
  }

  Base = mHsI2cBase[Channel];
  MmioOr32 (CLKGATE_IP_PERIC, 1U << mHsI2cGate[Channel]);
  if (!mHsI2cReady[Channel]) {
    HsI2cInit (Base);
    mHsI2cReady[Channel] = TRUE;
  }

  Ret = 0;
  for (Index = 0; (Ret == 0) && (Index < Num); Index++) {
    Ret = HsI2cDoMsg (Base, &Msgs[Index], Index == Num - 1);
  }

  if (Ret != 0) {
    DEBUG ((DEBUG_VERBOSE, "HsI2c%u: chip 0x%x msg %d failed, FIFO 0x%x TRANS 0x%x\n",
            Channel, Msgs[Index - 1].Addr, Index - 1,
            MmioRead32 (Base + HSI2C_FIFO_STATUS), MmioRead32 (Base + HSI2C_TRANS_STATUS)));
    HsI2cInit (Base);    // leave the controller usable for the next caller
    return -1;
  }
  return Num;
}
