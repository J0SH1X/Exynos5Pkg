/** @file
  v1awifi thermal sensor test (shell application): the TMU calibration
  (triminfo per block, the 5420 trim ring), then ten readings of all five
  sensors with the board's gain (5) and with mainline's (8), raw codes
  included, so the calibration can be checked against the room / a known
  temperature.  Leaves the sensors running with gain 5.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include "../../Drivers/Thermal/TmuCore.h"

STATIC
VOID
Sample (
  EXYNOS_TMU  *Tmu,
  UINT32      Gain
  )
{
  UINTN   Round;
  UINTN   Index;
  INT32   T[TMU_COUNT];
  UINT32  C[TMU_COUNT];

  TmuInit (Tmu, Gain);
  gBS->Stall (5000);
  for (Round = 0; Round < 10; Round++) {
    for (Index = 0; Index < TMU_COUNT; Index++) {
      T[Index] = TmuRead (Tmu, Index, &C[Index]);
    }
    DEBUG ((DEBUG_INFO, "ThermalTest: gain %u: CPU %d %d %d %d GPU %d C (codes %u %u %u %u %u)\n", Gain, T[0], T[1], T[2],
            T[3], T[4], C[0], C[1], C[2], C[3], C[4]));
    gBS->Stall (200 * 1000);
  }
}

EFI_STATUS
EFIAPI
ThermalTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EXYNOS_TMU  Tmu;
  UINTN       Index;

  ZeroMem (&Tmu, sizeof (Tmu));
  for (Index = 0; Index < TMU_COUNT; Index++) {
    Tmu.Base[Index] = TMU_PHYS (Index);
  }
  Tmu.Cmu = TMU_CMU_TOP_PHYS;
  TmuInit (&Tmu, TMU_GAIN_BOARD);
  DEBUG ((DEBUG_INFO, "ThermalTest: TRIMINFO %08x %08x %08x %08x %08x -> trim %u %u %u %u %u\n", Tmu.TrimInfo[0],
          Tmu.TrimInfo[1], Tmu.TrimInfo[2], Tmu.TrimInfo[3], Tmu.TrimInfo[4], Tmu.Trim[0], Tmu.Trim[1], Tmu.Trim[2],
          Tmu.Trim[3], Tmu.Trim[4]));
  Sample (&Tmu, 8);
  Sample (&Tmu, TMU_GAIN_BOARD);
  DEBUG ((DEBUG_INFO, "ThermalTest: done (gain %u left running)\n", TMU_GAIN_BOARD));
  return EFI_SUCCESS;
}
