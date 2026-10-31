#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/KeypadDeviceImplLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/KeypadDevice.h>
// Must follow
#include <Device/KeypadDevicePath.h>

STATIC KEYPAD_DEVICE_PROTOCOL mInternalKeypadDevice = {
    KeypadDeviceImplReset,
    KeypadDeviceImplGetKeys,
};

EFI_STATUS
EFIAPI
KeypadDeviceDxeInitialize(
    IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  Status = gBS->InstallMultipleProtocolInterfaces(
      &ImageHandle, &gEFIDroidKeypadDeviceProtocolGuid, &mInternalKeypadDevice,
      &gEfiDevicePathProtocolGuid, &mInternalKeypadDevicePath, NULL);
  ASSERT_EFI_ERROR(Status);

  DEBUG ((DEBUG_ERROR,
          "WATCH 2/4: GenericKeypadDeviceDxe OK - KEYPAD_DEVICE_PROTOCOL installed, stalling 5s\n"));
  gBS->Stall (5000000);

  return Status;
}
