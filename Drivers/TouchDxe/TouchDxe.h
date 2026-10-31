/** @file
  TouchDxe: v1awifi (SM-P900) touchscreen (Atmel mXT1664S) and S Pen
  (Wacom G9PLL) behind one EFI_ABSOLUTE_POINTER_PROTOCOL.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef TOUCH_DXE_H_
#define TOUCH_DXE_H_

#include <Uefi.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosI2c.h>

//
// The pointer's range is the pen's (the finer of the two), in screen
// orientation: X along the 2560-pixel side, Y along the 1600-pixel side
// (include/linux/wacom_i2c.h, CONFIG_V1A: no inversion, no X/Y switch).
// Finger coordinates are scaled up to it.
//
#define TOUCH_MAX_X  26266
#define TOUCH_MAX_Y  16416
#define TOUCH_MAX_Z  1023

typedef enum {
  TouchSourceFinger,
  TouchSourcePen
} TOUCH_SOURCE;

extern EXYNOS_I2C_PROTOCOL   *gTouchI2c;
extern EXYNOS_GPIO_PROTOCOL  *gTouchGpio;

/**
  Report a contact.  X/Y are in TOUCH_MAX_X/Y units, Z is pressure
  0..TOUCH_MAX_Z, Buttons are EFI_ABSP_TouchActive / EFI_ABS_AltActive.
  InRange FALSE means the source has gone (finger lifted, pen out of range).
**/
VOID
TouchReport (
  IN TOUCH_SOURCE  Source,
  IN BOOLEAN       InRange,
  IN UINT32        X,
  IN UINT32        Y,
  IN UINT32        Z,
  IN UINT32        Buttons
  );

EFI_STATUS
MxtTouchInit (
  VOID
  );

VOID
MxtTouchPoll (
  VOID
  );

EFI_STATUS
WacomPenInit (
  VOID
  );

VOID
WacomPenPoll (
  VOID
  );

#endif
