/** @file
  Bit-banged I2C on GPIO pins, for the buses the v1awifi kernel runs through
  i2c-gpio (same bus numbers):

    17: IF PMIC MAX77888 (charger 0x66, MUIC 0x25)   SDA GPB3(6), SCL GPB3(7)
        (board-universal5420-mfd.c)
    18: fuel gauge MAX17050 (0x36)                    SDA GPB0(3), SCL GPB0(4)
        (board-vienna-battery.c)

  Like i2c-gpio without the open-drain flags: a line is driven low by making
  it an output at 0 and released by making it an input; the board has the
  pull-ups (the kernel sets no internal pulls).  5 us half-period, ~100 kHz
  (i2c-gpio's default), clock stretching honoured on SCL.

  The pin registers are written directly (CON/DAT/PUD of the bank) so this
  code depends on neither GpioDxe nor GpioClx: the Windows battery driver
  (ExynosWinDrivers/exynosbatt) builds this file unchanged against its own
  <ExynosEnv.h>, with EXYNOS_GPIO_B_BASE its mapping of the controller.
  Callers serialise: one transfer at a time per bus.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <ExynosEnv.h>

#include "GpioI2c.h"

#define GPB0_BANK       0x060      // offsets from EXYNOS_GPIO_B_BASE
#define GPB3_BANK       0x0C0

#define BANK_CON(b)     (EXYNOS_GPIO_B_BASE + (b) + 0x0)
#define BANK_DAT(b)     (EXYNOS_GPIO_B_BASE + (b) + 0x4)
#define BANK_PUD(b)     (EXYNOS_GPIO_B_BASE + (b) + 0x8)

#define HALF_PERIOD_US  5
#define STRETCH_US      10000     // SCL held low by the slave: give up after 10 ms

typedef struct {
  UINT32  SdaBank;
  UINT32  SdaPin;
  UINT32  SclBank;
  UINT32  SclPin;
} GPIO_I2C_BUS;

STATIC CONST GPIO_I2C_BUS  mBuses[] = {
  { GPB3_BANK, 6, GPB3_BANK, 7 },   // 17
  { GPB0_BANK, 3, GPB0_BANK, 4 },   // 18
};

STATIC BOOLEAN  mInitialised[ARRAY_SIZE (mBuses)];

STATIC
VOID
LineRelease (
  IN UINT32  Bank,
  IN UINT32  Pin
  )
{
  MmioAnd32 (BANK_CON (Bank), ~(0xFu << (Pin * 4)));      // input
}

STATIC
VOID
LineLow (
  IN UINT32  Bank,
  IN UINT32  Pin
  )
{
  MmioAnd32 (BANK_DAT (Bank), ~(1u << Pin));
  MmioAndThenOr32 (BANK_CON (Bank), ~(0xFu << (Pin * 4)), 1u << (Pin * 4));
}

STATIC
UINTN
LineGet (
  IN UINT32  Bank,
  IN UINT32  Pin
  )
{
  return (MmioRead32 (BANK_DAT (Bank)) >> Pin) & 1;
}

STATIC
VOID
PinInit (
  IN UINT32  Bank,
  IN UINT32  Pin
  )
{
  MmioAnd32 (BANK_PUD (Bank), ~(0x3u << (Pin * 2)));      // no pull: board pull-ups
  MmioAnd32 (BANK_DAT (Bank), ~(1u << Pin));
  LineRelease (Bank, Pin);
}

#define SDA_HI(b)   LineRelease ((b)->SdaBank, (b)->SdaPin)
#define SDA_LO(b)   LineLow ((b)->SdaBank, (b)->SdaPin)
#define SDA_GET(b)  LineGet ((b)->SdaBank, (b)->SdaPin)
#define SCL_LO(b)   LineLow ((b)->SclBank, (b)->SclPin)
#define DELAY()     MicroSecondDelay (HALF_PERIOD_US)

/**
  Release SCL and wait for it to go high (clock stretching).
**/
STATIC
BOOLEAN
SclHigh (
  IN CONST GPIO_I2C_BUS  *B
  )
{
  UINTN  Waited;

  LineRelease (B->SclBank, B->SclPin);
  for (Waited = 0; LineGet (B->SclBank, B->SclPin) == 0; Waited++) {
    if (Waited >= STRETCH_US) {
      return FALSE;
    }
    MicroSecondDelay (1);
  }
  DELAY ();
  return TRUE;
}

STATIC
BOOLEAN
Start (
  IN CONST GPIO_I2C_BUS  *B
  )
{
  // Entered with SCL low (repeated start) or with an idle bus.
  SDA_HI (B);
  DELAY ();
  if (!SclHigh (B)) {
    return FALSE;
  }
  if (SDA_GET (B) == 0) {
    return FALSE;              // someone holds SDA: bus stuck
  }
  SDA_LO (B);
  DELAY ();
  SCL_LO (B);
  DELAY ();
  return TRUE;
}

STATIC
VOID
Stop (
  IN CONST GPIO_I2C_BUS  *B
  )
{
  SDA_LO (B);
  DELAY ();
  SclHigh (B);
  SDA_HI (B);
  DELAY ();
}

/**
  @return 0 ACK, 1 NACK, -1 SCL stuck.
**/
STATIC
INTN
WriteByte (
  IN CONST GPIO_I2C_BUS  *B,
  IN UINT8               Byte
  )
{
  INTN   Bit;
  UINTN  Nack;

  for (Bit = 7; Bit >= 0; Bit--) {
    if ((Byte >> Bit) & 1) {
      SDA_HI (B);
    } else {
      SDA_LO (B);
    }
    DELAY ();
    if (!SclHigh (B)) {
      return -1;
    }
    SCL_LO (B);
  }

  SDA_HI (B);
  DELAY ();
  if (!SclHigh (B)) {
    return -1;
  }
  Nack = SDA_GET (B);
  SCL_LO (B);
  DELAY ();
  return (INTN)Nack;
}

STATIC
INTN
ReadByte (
  IN  CONST GPIO_I2C_BUS  *B,
  OUT UINT8               *Byte,
  IN  BOOLEAN             Ack
  )
{
  UINTN  Bit;
  UINT8  Value;

  SDA_HI (B);
  for (Value = 0, Bit = 0; Bit < 8; Bit++) {
    DELAY ();
    if (!SclHigh (B)) {
      return -1;
    }
    Value = (UINT8)((Value << 1) | SDA_GET (B));
    SCL_LO (B);
  }
  *Byte = Value;

  if (Ack) {
    SDA_LO (B);
  }
  DELAY ();
  if (!SclHigh (B)) {
    return -1;
  }
  SCL_LO (B);
  SDA_HI (B);
  DELAY ();
  return 0;
}

/**
  Nine clocks with SDA released, then a STOP: frees a slave left mid-byte
  (e.g. by a bootloader that was interrupted).
**/
STATIC
VOID
BusRecover (
  IN CONST GPIO_I2C_BUS  *B
  )
{
  UINTN  Clock;

  SDA_HI (B);
  for (Clock = 0; Clock < 9; Clock++) {
    SCL_LO (B);
    DELAY ();
    SclHigh (B);
  }
  Stop (B);
}

INTN
GpioI2cXfer (
  IN UINT32          Bus,
  IN EXYNOS_I2C_MSG  *Msgs,
  IN INTN            Num
  )
{
  CONST GPIO_I2C_BUS  *B;
  INTN                Index;
  UINTN               Byte;
  INTN                Ret;
  CONST CHAR8         *Why;

  if ((Bus < EXYNOS_I2C_FIRST_GPIO_BUS) || (Bus - EXYNOS_I2C_FIRST_GPIO_BUS >= ARRAY_SIZE (mBuses))) {
    return -1;
  }
  B = &mBuses[Bus - EXYNOS_I2C_FIRST_GPIO_BUS];

  if (!mInitialised[Bus - EXYNOS_I2C_FIRST_GPIO_BUS]) {
    PinInit (B->SdaBank, B->SdaPin);
    PinInit (B->SclBank, B->SclPin);
    MicroSecondDelay (HALF_PERIOD_US * 2);
    if ((SDA_GET (B) == 0) || (LineGet (B->SclBank, B->SclPin) == 0)) {
      DEBUG ((DEBUG_INFO, "GpioI2c%u: bus not idle (SDA %u SCL %u), recovering\n",
              Bus, SDA_GET (B), LineGet (B->SclBank, B->SclPin)));
      BusRecover (B);
    }
    mInitialised[Bus - EXYNOS_I2C_FIRST_GPIO_BUS] = TRUE;
  }

  Ret = 0;
  Why = NULL;
  for (Index = 0; (Ret == 0) && (Index < Num); Index++) {
    if (!Start (B)) {
      Why = "bus stuck at start";
      Ret = -1;
      break;
    }
    Ret = WriteByte (B, (UINT8)((Msgs[Index].Addr << 1) | ((Msgs[Index].Flags & I2C_M_RD) ? 1 : 0)));
    if (Ret != 0) {
      Why = (Ret == 1) ? "address NACK" : "SCL stuck";
      break;
    }
    for (Byte = 0; (Ret == 0) && (Byte < Msgs[Index].Len); Byte++) {
      if ((Msgs[Index].Flags & I2C_M_RD) != 0) {
        Ret = ReadByte (B, &Msgs[Index].Buf[Byte], Byte + 1 < Msgs[Index].Len);
      } else {
        Ret = WriteByte (B, Msgs[Index].Buf[Byte]);
      }
    }
    if (Ret != 0) {
      Why = (Ret == 1) ? "data NACK" : "SCL stuck";
    }
  }
  Stop (B);

  if (Ret != 0) {
    DEBUG ((DEBUG_ERROR, "GpioI2c%u: chip 0x%x msg %d: %a\n",
            Bus, Msgs[(Index < Num) ? Index : Num - 1].Addr, Index, Why));
    return -1;
  }
  return Num;
}
