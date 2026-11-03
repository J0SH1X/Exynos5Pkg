/** @file
  Starts the five Exynos5420 thermal sensors at boot (shared TmuCore.c) and
  leaves them running for the OS: Windows' ACPI thermal zones read them
  from the DSDT (EXYNOS_THERMAL, PlatformThermal.h).  Arms the hardware
  thermal trip (115 °C, the PMU powers off).  Logs the temperatures once a
  minute while UEFI runs (timer event).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "../TmuCore.h"

STATIC EXYNOS_TMU  mTmu;
STATIC EFI_EVENT   mTimer;

STATIC
VOID
LogTemps (
  IN CONST CHAR8  *When
  )
{
  INT32  T[TMU_COUNT];
  UINTN  Index;

  for (Index = 0; Index < TMU_COUNT; Index++) {
    T[Index] = TmuRead (&mTmu, Index, NULL);
  }
  DEBUG ((DEBUG_INFO, "Tmu: %a: CPU %d %d %d %d, GPU %d C\n", When, T[0], T[1], T[2], T[3], T[4]));
}

STATIC
VOID
EFIAPI
OnTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  LogTemps ("now");
}

EFI_STATUS
EFIAPI
TmuDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN  Index;

  for (Index = 0; Index < TMU_COUNT; Index++) {
    mTmu.Base[Index] = TMU_PHYS (Index);
  }
  mTmu.Cmu = TMU_CMU_TOP_PHYS;
  TmuInit (&mTmu, TMU_GAIN_BOARD);
  DEBUG ((DEBUG_INFO, "Tmu: trim %u %u %u %u %u (TRIMINFO %08x %08x %08x %08x %08x), gain %u, hardware trip 115 C\n",
          mTmu.Trim[0], mTmu.Trim[1], mTmu.Trim[2], mTmu.Trim[3], mTmu.Trim[4], mTmu.TrimInfo[0], mTmu.TrimInfo[1],
          mTmu.TrimInfo[2], mTmu.TrimInfo[3], mTmu.TrimInfo[4], mTmu.Gain));
  gBS->Stall (2000);          // first conversion
  LogTemps ("boot");
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnTimer, NULL, &mTimer))) {
    gBS->SetTimer (mTimer, TimerPeriodic, 60ull * 10000000);
  }
  return EFI_SUCCESS;
}
