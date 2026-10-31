/** @file
  Keypad device implementation for the Galaxy Tab Pro 12.2 WiFi (v1awifi).

  Four plain gpio-keys, exactly as the kernel sees them in gpio_button[]
  (arch/arm/mach-exynos/board-klimt-input.c) with pins from
  gpio-v1a-3g-rev00.h:

    KEY_POWER       GPX2(2)  active low
    KEY_VOLUMEUP    GPX0(2)  active low
    KEY_VOLUMEDOWN  GPX0(3)  active low
    KEY_HOME        GPX0(5)  active low

  They are mapped to shell keys the same way HtcLeoPkg maps its keys:
  power -> ESC, vol+ -> Up, vol- -> Down, home -> Home.  All pins are
  (re)configured as inputs with pull-up on Reset.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/KeypadDeviceHelperLib.h>
#include <Library/KeypadDeviceImplLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosGpio.h>
#include <Protocol/KeypadDevice.h>

// Cached copy of the GPIO protocol instance
STATIC EXYNOS_GPIO_PROTOCOL *mGpio = NULL;

typedef struct {
  KEY_CONTEXT     EfiKeyContext;
  BOOLEAN         IsValid;
  EXYNOS_GPIO_PIN Pin;
  BOOLEAN         ActiveLow;
} KEY_CONTEXT_PRIVATE;

STATIC KEY_CONTEXT_PRIVATE KeyContextPower;
STATIC KEY_CONTEXT_PRIVATE KeyContextVolumeUp;
STATIC KEY_CONTEXT_PRIVATE KeyContextVolumeDown;
STATIC KEY_CONTEXT_PRIVATE KeyContextHome;

STATIC KEY_CONTEXT_PRIVATE *KeyList[] = {
    &KeyContextPower, &KeyContextVolumeUp, &KeyContextVolumeDown,
    &KeyContextHome};

RETURN_STATUS
EFIAPI
KeypadDeviceImplConstructor (
  VOID
  )
{
  EFI_STATUS Status;

  // Find the GPIO controller protocol.  ASSERT if not found.
  Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&mGpio);
  ASSERT_EFI_ERROR (Status);

  return RETURN_SUCCESS;
}

EFI_STATUS
EFIAPI
KeypadDeviceImplReset (
  KEYPAD_DEVICE_PROTOCOL  *This
  )
{
  UINTN Index;

  LibKeyInitializeKeyContext (&KeyContextPower.EfiKeyContext);
  KeyContextPower.EfiKeyContext.KeyData.Key.ScanCode      = SCAN_ESC;
  KeyContextPower.Pin                                     = EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX2, 2);
  KeyContextPower.ActiveLow                               = TRUE;
  KeyContextPower.IsValid                                 = TRUE;

  LibKeyInitializeKeyContext (&KeyContextVolumeUp.EfiKeyContext);
  KeyContextVolumeUp.EfiKeyContext.KeyData.Key.ScanCode   = SCAN_UP;
  KeyContextVolumeUp.Pin                                  = EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX0, 2);
  KeyContextVolumeUp.ActiveLow                            = TRUE;
  KeyContextVolumeUp.IsValid                              = TRUE;

  LibKeyInitializeKeyContext (&KeyContextVolumeDown.EfiKeyContext);
  KeyContextVolumeDown.EfiKeyContext.KeyData.Key.ScanCode = SCAN_DOWN;
  KeyContextVolumeDown.Pin                                = EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX0, 3);
  KeyContextVolumeDown.ActiveLow                          = TRUE;
  KeyContextVolumeDown.IsValid                            = TRUE;

  LibKeyInitializeKeyContext (&KeyContextHome.EfiKeyContext);
  KeyContextHome.EfiKeyContext.KeyData.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;
  KeyContextHome.Pin                                      = EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX0, 5);
  KeyContextHome.ActiveLow                                = TRUE;
  KeyContextHome.IsValid                                  = TRUE;

  // Make sure every key pin is an input with pull-up
  for (Index = 0; Index < (sizeof (KeyList) / sizeof (KeyList[0])); Index++) {
    if (KeyList[Index]->IsValid) {
      mGpio->Config (mGpio, KeyList[Index]->Pin, EXYNOS_GPIO_MODE_INPUT,
                     EXYNOS_GPIO_PULL_UP);
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
KeypadDeviceImplGetKeys (
  KEYPAD_DEVICE_PROTOCOL  *This,
  KEYPAD_RETURN_API       *KeypadReturnApi,
  UINT64                  Delta
  )
{
  UINTN    Index;
  UINTN    Level;
  BOOLEAN  IsPressed;

  //
  // DEBUG: dump the raw button registers twice a second so a panel photo
  // shows whether the pins ever move.  GPX0 CON/DAT/PULL at 0x13400C00,
  // GPX2 at 0x13400C40.  Delta is nanoseconds (GetTimeInNanoSecond).
  // DEBUG_ERROR because PcdDebugPrintErrorLevel masks out DEBUG_INFO.
  //
  {
    STATIC UINT64 mDumpAccum = 0;
    mDumpAccum += Delta;
    if (mDumpAccum >= 500000000ULL) {
      mDumpAccum = 0;
      DEBUG ((DEBUG_ERROR,
              "GPX0 CON=%08x DAT=%08x PULL=%08x | GPX2 CON=%08x DAT=%08x PULL=%08x\n",
              MmioRead32 (0x13400C00), MmioRead32 (0x13400C04),
              MmioRead32 (0x13400C08),
              MmioRead32 (0x13400C40), MmioRead32 (0x13400C44),
              MmioRead32 (0x13400C48)));
    }
  }

  for (Index = 0; Index < (sizeof (KeyList) / sizeof (KeyList[0])); Index++) {
    KEY_CONTEXT_PRIVATE *Context = KeyList[Index];

    if (Context->IsValid == FALSE) {
      continue;
    }

    // All four keys are active low
    Level     = mGpio->Get (mGpio, Context->Pin);
    IsPressed = (BOOLEAN)((Level ? 1 : 0) ^ Context->ActiveLow);

    LibKeyUpdateKeyStatus (&Context->EfiKeyContext, KeypadReturnApi, IsPressed,
                           Delta);
  }

  return EFI_SUCCESS;
}
