/** @file
  Bit-banged sensor I2C: see CamSensorI2c.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "CamSensorI2c.h"

#define HALF_US   5
#define STRETCH   2000

#define CON(c)    ((c)->Gpio + (c)->Bank + 0x0)
#define DAT(c)    ((c)->Gpio + (c)->Bank + 0x4)

STATIC
VOID
Release (
  CAM_I2C  *C,
  UINT32   Pin
  )
{
  MmioAnd32 (CON (C), ~(0xFu << (Pin * 4)));
}

STATIC
VOID
Low (
  CAM_I2C  *C,
  UINT32   Pin
  )
{
  MmioAnd32 (DAT (C), ~(1u << Pin));
  MmioAndThenOr32 (CON (C), ~(0xFu << (Pin * 4)), 1u << (Pin * 4));
}

STATIC
UINT32
Get (
  CAM_I2C  *C,
  UINT32   Pin
  )
{
  return (MmioRead32 (DAT (C)) >> Pin) & 1;
}

STATIC
BOOLEAN
SclHigh (
  CAM_I2C  *C
  )
{
  UINTN  Left;

  Release (C, C->Scl);
  for (Left = 0; Left < STRETCH; Left++) {
    if (Get (C, C->Scl) != 0) {
      return TRUE;
    }
    MicroSecondDelay (1);
  }
  return FALSE;
}

STATIC
VOID
Start (
  CAM_I2C  *C
  )
{
  Release (C, C->Sda);
  SclHigh (C);
  MicroSecondDelay (HALF_US);
  Low (C, C->Sda);
  MicroSecondDelay (HALF_US);
  Low (C, C->Scl);
}

STATIC
VOID
Stop (
  CAM_I2C  *C
  )
{
  Low (C, C->Sda);
  MicroSecondDelay (HALF_US);
  SclHigh (C);
  MicroSecondDelay (HALF_US);
  Release (C, C->Sda);
  MicroSecondDelay (HALF_US);
}

STATIC
BOOLEAN
WriteByte (
  CAM_I2C  *C,
  UINT8    Byte
  )
{
  UINTN    Bit;
  BOOLEAN  Ack;

  for (Bit = 0; Bit < 8; Bit++) {
    if ((Byte & 0x80) != 0) {
      Release (C, C->Sda);
    } else {
      Low (C, C->Sda);
    }
    Byte <<= 1;
    MicroSecondDelay (HALF_US);
    SclHigh (C);
    MicroSecondDelay (HALF_US);
    Low (C, C->Scl);
  }
  Release (C, C->Sda);
  MicroSecondDelay (HALF_US);
  SclHigh (C);
  MicroSecondDelay (HALF_US);
  Ack = (Get (C, C->Sda) == 0);
  Low (C, C->Scl);
  return Ack;
}

STATIC
UINT8
ReadByte (
  CAM_I2C  *C,
  BOOLEAN  Nack
  )
{
  UINT8  Byte = 0;
  UINTN  Bit;

  Release (C, C->Sda);
  for (Bit = 0; Bit < 8; Bit++) {
    MicroSecondDelay (HALF_US);
    SclHigh (C);
    MicroSecondDelay (HALF_US);
    Byte = (UINT8)((Byte << 1) | Get (C, C->Sda));
    Low (C, C->Scl);
  }
  if (Nack) {
    Release (C, C->Sda);
  } else {
    Low (C, C->Sda);
  }
  MicroSecondDelay (HALF_US);
  SclHigh (C);
  MicroSecondDelay (HALF_US);
  Low (C, C->Scl);
  Release (C, C->Sda);
  return Byte;
}

//
// Takes the pins from the ISP (they are in function 2) and gives them back.
//
STATIC
UINT32
Take (
  CAM_I2C  *C
  )
{
  UINT32  Saved = MmioRead32 (CON (C)) & ((0xFu << (C->Sda * 4)) | (0xFu << (C->Scl * 4)));

  Release (C, C->Sda);
  Release (C, C->Scl);
  MicroSecondDelay (20);
  return Saved;
}

STATIC
VOID
Give (
  CAM_I2C  *C,
  UINT32   Saved
  )
{
  MmioAndThenOr32 (CON (C), ~((0xFu << (C->Sda * 4)) | (0xFu << (C->Scl * 4))), Saved);
}

INTN
CamI2cRead (
  IN  CAM_I2C  *I2c,
  IN  UINT16   Reg,
  OUT UINT8    *Value
  )
{
  UINT32  Saved = Take (I2c);
  INTN    Result = 0;

  if (Get (I2c, I2c->Sda) == 0) {
    Result = -3;
    goto Done;
  }
  Start (I2c);
  if (!WriteByte (I2c, (UINT8)(I2c->Addr << 1))) {
    Result = -1;
    goto Stop;
  }
  if (!WriteByte (I2c, (UINT8)(Reg >> 8)) || !WriteByte (I2c, (UINT8)Reg)) {
    Result = -2;
    goto Stop;
  }
  Stop (I2c);
  Start (I2c);
  if (!WriteByte (I2c, (UINT8)((I2c->Addr << 1) | 1))) {
    Result = -1;
    goto Stop;
  }
  *Value = ReadByte (I2c, TRUE);
Stop:
  Stop (I2c);
Done:
  Give (I2c, Saved);
  return Result;
}

INTN
CamI2cWrite (
  IN CAM_I2C  *I2c,
  IN UINT16   Reg,
  IN UINT8    Value
  )
{
  UINT32  Saved = Take (I2c);
  INTN    Result = 0;

  if (Get (I2c, I2c->Sda) == 0) {
    Result = -3;
    goto Done;
  }
  Start (I2c);
  if (!WriteByte (I2c, (UINT8)(I2c->Addr << 1))) {
    Result = -1;
  } else if (!WriteByte (I2c, (UINT8)(Reg >> 8)) || !WriteByte (I2c, (UINT8)Reg) || !WriteByte (I2c, Value)) {
    Result = -2;
  }
  Stop (I2c);
Done:
  Give (I2c, Saved);
  return Result;
}

INTN
CamI2cRead16 (
  IN  CAM_I2C  *I2c,
  IN  UINT16   Reg,
  OUT UINT16   *Value
  )
{
  UINT8  Hi, Lo;
  INTN   R;

  R = CamI2cRead (I2c, Reg, &Hi);
  if (R == 0) {
    R = CamI2cRead (I2c, Reg + 1, &Lo);
  }
  if (R == 0) {
    *Value = (UINT16)((Hi << 8) | Lo);
  }
  return R;
}

INTN
CamI2cWrite16 (
  IN CAM_I2C  *I2c,
  IN UINT16   Reg,
  IN UINT16   Value
  )
{
  INTN  R = CamI2cWrite (I2c, Reg, (UINT8)(Value >> 8));

  if (R == 0) {
    R = CamI2cWrite (I2c, Reg + 1, (UINT8)Value);
  }
  return R;
}
