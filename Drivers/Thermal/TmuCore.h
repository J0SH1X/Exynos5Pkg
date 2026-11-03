/** @file
  Exynos5420 thermal management units (TMU): five sensors, CPU0..CPU3 (the
  A15 cluster) and the GPU.  Shared by UEFI (Drivers/Thermal/TmuDxe,
  Application/ThermalTestApp) and, if needed, Windows, through
  <ExynosEnv.h>; the caller maps the blocks.

  Sources: Samsung's 3.4 kernel (drivers/thermal/exynos_thermal.c,
  board-universal5420-power.c exynos5_tmu_data: one-point trimming,
  gain 5, reference voltage 16, noise-cancel mode 4, trigger levels
  75 / 90 / 110 / 115 °C, eFuse fallback 55 when the trim is outside
  16..76), mainline drivers/thermal/samsung/exynos_tmu.c (register layout,
  the 5420 triminfo ring: sensors 2 / 3 / 4 take their trim from blocks
  3 / 4 / 2; clocks GATE_IP_PERIS bits 21 (TMU) and 22 (TMU_GPU)).

  Temperature = code - trim + 25 (°C), codes 31..146 valid.  Level 3
  (115 °C) is the hardware thermal trip (CONTROL bit 12): the PMU powers the
  SoC off without any software.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __TMU_CORE_H__
#define __TMU_CORE_H__

#include <ExynosEnv.h>

#define TMU_COUNT          5
#define TMU_GPU            4

#define TMU_PHYS(i)        ((i) == 0 ? 0x10060000u : (i) == 1 ? 0x10064000u : (i) == 2 ? 0x10068000u : \
                            (i) == 3 ? 0x1006C000u : 0x100A0000u)
#define TMU_SIZE           0x100
#define TMU_CMU_TOP_PHYS   0x10020000u      // GATE_IP_PERIS at +0x960

#define TMU_GAIN_BOARD     5                // Samsung board file (mainline: 8)
#define TMU_TRIP_C         { 75, 90, 110, 115 }

typedef struct {
  UINTN    Base[TMU_COUNT];  // filled by the caller
  UINTN    Cmu;
  UINT8    Trim[TMU_COUNT];  // per sensor (after the ring and the fallback)
  UINT32   TrimInfo[TMU_COUNT];
  UINT32   Gain;
  BOOLEAN  Up;
} EXYNOS_TMU;

//
// Clocks on, triminfo, thresholds (75 / 90 / 110 / 115 °C), sensing on,
// hardware trip at level 3; no interrupts (readers poll).  0 or < 0.
//
INTN
TmuInit (
  IN OUT EXYNOS_TMU  *Tmu,
  IN     UINT32      Gain
  );

//
// °C, or a value <= -100 for "no valid reading"; Code gets the raw code.
//
INT32
TmuRead (
  IN  EXYNOS_TMU  *Tmu,
  IN  UINTN       Index,
  OUT UINT32      *Code   OPTIONAL
  );

#endif // __TMU_CORE_H__
