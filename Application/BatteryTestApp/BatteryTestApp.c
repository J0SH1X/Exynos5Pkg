/** @file
  Battery test: prints the BatteryDxe status once a second until a key is
  pressed, so charging can be watched without the USB log (a wall charger
  takes the micro-USB port).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/ExynosBattery.h>

STATIC CONST CHAR16  *mChgState[16] = {
  L"pre-charge", L"fast CC", L"fast CV", L"top-off", L"done", L"high temp",
  L"timer fault", L"thermistor", L"off", L"?9", L"?A", L"?B", L"?C", L"?D", L"?E", L"?F"
};

STATIC CONST CHAR16  *mVbus[4] = { L"UVLO", L"UVLO", L"OVLO", L"valid" };

EFI_STATUS
EFIAPI
BatteryTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EXYNOS_BATTERY_PROTOCOL  *Battery;
  EXYNOS_BATTERY_STATUS    S;
  EFI_INPUT_KEY            Key;
  EFI_STATUS               Status;

  Status = gBS->LocateProtocol (&gExynosBatteryProtocolGuid, NULL, (VOID **)&Battery);
  if (EFI_ERROR (Status)) {
    Print (L"BatteryDxe not loaded: %r\n", Status);
    return Status;
  }

  Print (L"Battery test - any key quits\n\n");
  while (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key) == EFI_NOT_READY) {
    Status = Battery->GetStatus (Battery, &S);
    if (EFI_ERROR (Status)) {
      Print (L"GetStatus: %r\n", Status);
    } else {
      Print (L"%3u.%02u%%  %4u mV  %5d mA (avg %5d)  %2d.%u C  %4u/%4u mAh | %a  in %4u mA  chg %4u mA  VBUS %s  %s%s\n",
             S.SocCentiPercent / 100, S.SocCentiPercent % 100, S.MilliVolts,
             S.MilliAmps, S.AvgMilliAmps, S.DeciCelsius / 10, (UINT32)((S.DeciCelsius < 0) ? -S.DeciCelsius : S.DeciCelsius) % 10,
             S.RemainingMah, S.FullMah, S.Cable, S.InputLimitMa, S.ChargeCurrentMa,
             mVbus[S.VbusDetails & 3], mChgState[S.ChargerDetails & 0xF],
             S.TemperatureHold ? L" (temp hold)" : L"");
    }
    gBS->Stall (1000 * 1000);
  }
  return EFI_SUCCESS;
}
