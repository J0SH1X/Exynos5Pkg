/** @file
  Software auto exposure: see CamAe.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "CamAe.h"

#define MIN_LINES      4
#define MAX_ANALOG     2048
#define MAX_DIGITAL    4095

VOID
CamAeInit (
  OUT CAM_AE  *Ae,
  IN  UINT32  MaxLines,
  IN  UINT32  Target,
  IN  UINT32  Kind
  )
{
  Ae->Kind        = Kind;
  Ae->MaxDigital  = (Kind == CAM_AE_S5K6B2) ? 2048 : MAX_DIGITAL;
  Ae->Lines       = (Kind == CAM_AE_S5K6B2) ? 500 : 302;   // what the A5 leaves in the sensor
  Ae->AnalogX256  = 256;
  Ae->DigitalX256 = 256;
  Ae->MaxLines    = MaxLines;
  Ae->Target      = Target;
}

VOID
CamAeStep (
  IN OUT CAM_AE  *Ae,
  IN     UINT32  Mean
  )
{
  UINT64  Total;
  UINT64  Want;
  UINT64  Rest;

  if (Mean < 1) {
    Mean = 1;
  }
  Total = (UINT64)Ae->Lines * Ae->AnalogX256 * Ae->DigitalX256;
  Want  = Total * Ae->Target / Mean;
  if (Want > Total * 2) {
    Want = Total * 2;
  } else if (Want < Total / 2) {
    Want = Total / 2;
  }
  if (Want < (UINT64)MIN_LINES * 256 * 256) {
    Want = (UINT64)MIN_LINES * 256 * 256;
  }

  //
  // Exposure time first, then analog gain, then digital gain.
  //
  if (Want <= (UINT64)Ae->MaxLines * 256 * 256) {
    Ae->Lines       = (UINT32)(Want / (256 * 256));
    Ae->AnalogX256  = 256;
    Ae->DigitalX256 = 256;
    return;
  }
  Ae->Lines = Ae->MaxLines;
  Rest = Want / Ae->MaxLines;                           // analog x digital, x 256 x 256
  if (Rest <= (UINT64)MAX_ANALOG * 256) {
    Ae->AnalogX256  = (UINT32)(Rest / 256);
    Ae->DigitalX256 = 256;
  } else {
    Ae->AnalogX256  = MAX_ANALOG;
    Rest            = Rest / MAX_ANALOG;
    Ae->DigitalX256 = (Rest > Ae->MaxDigital) ? Ae->MaxDigital : (UINT32)Rest;
  }
  if (Ae->AnalogX256 < 256) {
    Ae->AnalogX256 = 256;
  }
  if (Ae->DigitalX256 < 256) {
    Ae->DigitalX256 = 256;
  }
}

INTN
CamAeApply (
  IN CAM_I2C  *I2c,
  IN CAM_AE   *Ae
  )
{
  UINT32  Code = (Ae->Kind == CAM_AE_S5K6B2) ? (Ae->AnalogX256 / 8)            // 1x -> 0x20
                                             : 256 - (256 * 256) / Ae->AnalogX256;   // 1x -> 0, 8x -> 224
  INTN    R;

  R  = CamI2cWrite (I2c, 0x0104, 1);
  R |= CamI2cWrite16 (I2c, 0x0202, (UINT16)Ae->Lines);
  R |= CamI2cWrite16 (I2c, 0x0204, (UINT16)Code);
  R |= CamI2cWrite16 (I2c, 0x020E, (UINT16)Ae->DigitalX256);
  R |= CamI2cWrite16 (I2c, 0x0210, (UINT16)Ae->DigitalX256);
  R |= CamI2cWrite16 (I2c, 0x0212, (UINT16)Ae->DigitalX256);
  R |= CamI2cWrite16 (I2c, 0x0214, (UINT16)Ae->DigitalX256);
  R |= CamI2cWrite (I2c, 0x0104, 0);
  return R;
}
