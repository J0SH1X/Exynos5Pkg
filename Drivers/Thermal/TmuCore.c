/** @file
  Exynos5420 TMU core: see TmuCore.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "TmuCore.h"

#define TMU_TRIMINFO        0x00
#define TMU_TRIMINFO_CON    0x14
#define   TRIMINFO_RELOAD     0x1
#define TMU_CONTROL         0x20
#define   CONTROL_CORE_ON     0x3
#define   CONTROL_TRIP_EN     (1u << 12)
#define   CONTROL_NOISE(m)    ((UINT32)(m) << 13)
#define   CONTROL_MUX_6       (6u << 20)
#define   CONTROL_GAIN(g)     ((UINT32)(g) << 8)
#define   CONTROL_VREF(v)     ((UINT32)(v) << 24)
#define TMU_STATUS          0x28
#define TMU_CURRENT_TEMP    0x40
#define TMU_THD_TEMP_RISE   0x50
#define TMU_THD_TEMP_FALL   0x54
#define TMU_INTEN           0x70
#define TMU_INTCLEAR        0x78
#define   INTCLEAR_ALL        (0x111u | (0x111u << 16))

#define GATE_IP_PERIS       0x960
#define   GATE_TMU            (1u << 21)
#define   GATE_TMU_GPU        (1u << 22)

#define EFUSE_MIN           16
#define EFUSE_MAX           76
#define EFUSE_DEFAULT       55
#define FALL_GAP            2

//
// 5420: sensor -> block whose TRIMINFO holds its calibration (exynos_thermal.c
// code_to_temp fuse_id; mainline's ext-triminfo ring).
//
STATIC CONST UINT8  mTrimBlock[TMU_COUNT] = { 0, 1, 3, 4, 2 };

INTN
TmuInit (
  IN OUT EXYNOS_TMU  *Tmu,
  IN     UINT32      Gain
  )
{
  STATIC CONST UINT32  Trips[4] = TMU_TRIP_C;
  UINTN                Index;
  UINTN                Level;
  UINT32               Rise;
  UINT32               Fall;
  UINT32               Code;
  INTN                 Result = 0;

  MmioOr32 (Tmu->Cmu + GATE_IP_PERIS, GATE_TMU | GATE_TMU_GPU);
  Tmu->Gain = Gain;

  //
  // Calibration: reload every block's triminfo, then pick per sensor.
  //
  for (Index = 0; Index < TMU_COUNT; Index++) {
    MmioWrite32 (Tmu->Base[Index] + TMU_TRIMINFO_CON, TRIMINFO_RELOAD);
  }
  MicroSecondDelay (100);
  for (Index = 0; Index < TMU_COUNT; Index++) {
    Tmu->TrimInfo[Index] = MmioRead32 (Tmu->Base[Index] + TMU_TRIMINFO);
  }
  for (Index = 0; Index < TMU_COUNT; Index++) {
    UINT32  Info = Tmu->TrimInfo[mTrimBlock[Index]];
    UINT32  Trim = Info & 0xFF;

    // exynos_tmu_set_efuse: out of range or a second-point value -> board default
    if ((Trim < EFUSE_MIN) || (Trim > EFUSE_MAX) || (((Info >> 8) & 0xFF) != 0)) {
      Trim = EFUSE_DEFAULT;
    }
    Tmu->Trim[Index] = (UINT8)Trim;
  }

  //
  // Thresholds, sensing on, hardware trip.
  //
  for (Index = 0; Index < TMU_COUNT; Index++) {
    UINTN  Base = Tmu->Base[Index];

    if ((MmioRead32 (Base + TMU_STATUS) & 0xFF) == 0) {
      DEBUG ((DEBUG_ERROR, "Tmu: sensor %u not alive (STATUS 0)\n", Index));
      Result = -1;
      continue;
    }
    Rise = 0;
    Fall = 0;
    for (Level = 0; Level < 4; Level++) {
      Code  = Trips[Level] + Tmu->Trim[Index] - 25;
      Rise |= (Code & 0xFF) << (8 * Level);
      if (Level < 3) {
        Fall |= ((Code - FALL_GAP) & 0xFF) << (8 * Level);
      }
    }
    MmioWrite32 (Base + TMU_THD_TEMP_RISE, Rise);
    MmioWrite32 (Base + TMU_THD_TEMP_FALL, Fall);
    MmioWrite32 (Base + TMU_INTEN, 0);
    MmioWrite32 (Base + TMU_INTCLEAR, INTCLEAR_ALL);
    MmioWrite32 (Base + TMU_CONTROL, CONTROL_VREF (16) | CONTROL_GAIN (Gain) | CONTROL_NOISE (4) | CONTROL_MUX_6 |
                                     CONTROL_TRIP_EN | CONTROL_CORE_ON);
  }
  Tmu->Up = TRUE;
  return Result;
}

INT32
TmuRead (
  IN  EXYNOS_TMU  *Tmu,
  IN  UINTN       Index,
  OUT UINT32      *Code   OPTIONAL
  )
{
  UINT32  Raw;

  if (!Tmu->Up || (Index >= TMU_COUNT)) {
    return -1000;
  }
  Raw = MmioRead32 (Tmu->Base[Index] + TMU_CURRENT_TEMP) & 0xFF;
  if (Code != NULL) {
    *Code = Raw;
  }
  if ((Raw < 31) || (Raw > 146)) {
    return -1000;
  }
  return (INT32)Raw - (INT32)Tmu->Trim[Index] + 25;
}
