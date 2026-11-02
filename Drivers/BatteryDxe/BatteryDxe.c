/** @file
  v1awifi battery and charging in UEFI: runs the shared battery logic
  (BatteryCore.c, also built by the Windows driver exynosbatt) every 5 s
  and publishes EXYNOS_BATTERY_PROTOCOL.

  The poll runs from a timer at TPL_CALLBACK; GetStatus() raises to the
  same TPL so the two never interleave transfers on the bit-banged buses.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <ExynosEnv.h>

#include <Protocol/ExynosBattery.h>

#include "BatteryCore.h"

#define POLL_100NS  (5 * 10000000ULL)

STATIC EXYNOS_I2C_PROTOCOL  *mI2c;

BOOLEAN
EnvI2cRead (
  IN  UINT32  Bus,
  IN  UINT8   Addr,
  IN  UINT8   Reg,
  OUT UINT8   *Buf,
  IN  UINTN   Count
  )
{
  return mI2c->Read (mI2c, Bus, Addr, Reg, Buf, Count) == (INTN)Count;
}

BOOLEAN
EnvI2cWrite (
  IN UINT32  Bus,
  IN UINT8   Addr,
  IN UINT8   *Buf,
  IN UINTN   Count
  )
{
  return mI2c->Write (mI2c, Bus, Addr, Buf, Count) == (INTN)Count;
}

STATIC
VOID
EFIAPI
Poll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  BatteryCorePoll ();
}

STATIC
EFI_STATUS
EFIAPI
GetStatus (
  IN  EXYNOS_BATTERY_PROTOCOL  *This,
  OUT EXYNOS_BATTERY_STATUS    *Status
  )
{
  EFI_TPL  OldTpl;
  BOOLEAN  Ok;

  if (Status == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  Ok     = BatteryCoreGetStatus (Status);
  gBS->RestoreTPL (OldTpl);
  return Ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EXYNOS_BATTERY_PROTOCOL  mBattery = { GetStatus };

EFI_STATUS
EFIAPI
BatteryDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT   Event;
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&mI2c);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  BatteryCoreInit ();
  BatteryCorePoll ();

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Poll, NULL, &Event);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (Event, TimerPeriodic, POLL_100NS);
  }
  return gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gExynosBatteryProtocolGuid, &mBattery, NULL);
}
