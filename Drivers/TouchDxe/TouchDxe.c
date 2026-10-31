/** @file
  TouchDxe: v1awifi touchscreen (MxtTouch.c) and S Pen (WacomPen.c) as one
  EFI_ABSOLUTE_POINTER_PROTOCOL.

  An absolute pointer has a single contact.  While the pen is in range it
  owns the pointer and finger input is ignored (resting the hand on the
  screen while writing); otherwise the first finger does.

  Both chips are polled from a 5 ms timer: the touch CHG line (GPD1(0)) has
  no EINT and the pen IRQ (GPX3(5)) would need the shared EINT16-31 group
  demux.  Each poll only reads a GPIO level; I2C runs only when a chip says
  it has data.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "TouchDxe.h"

#define TOUCH_POLL_100NS  (5 * 10000)    // 5 ms

EXYNOS_I2C_PROTOCOL   *gTouchI2c;
EXYNOS_GPIO_PROTOCOL  *gTouchGpio;

STATIC EFI_ABSOLUTE_POINTER_MODE   mMode;
STATIC EFI_ABSOLUTE_POINTER_STATE  mState;
STATIC BOOLEAN                     mStateChanged;
STATIC BOOLEAN                     mPenInRange;
STATIC BOOLEAN                     mHaveTouch;
STATIC BOOLEAN                     mHavePen;
STATIC EFI_EVENT                   mPollEvent;

STATIC EFI_ABSOLUTE_POINTER_PROTOCOL  mAbsolutePointer;

VOID
TouchReport (
  IN TOUCH_SOURCE  Source,
  IN BOOLEAN       InRange,
  IN UINT32        X,
  IN UINT32        Y,
  IN UINT32        Z,
  IN UINT32        Buttons
  )
{
  EFI_TPL  OldTpl;

  if (Source == TouchSourcePen) {
    mPenInRange = InRange;
  } else if (mPenInRange) {
    return;
  }

  //
  // Bring-up: one log line per contact change (down/up, in/out of range).
  //
  {
    STATIC UINT32  mLast[2] = { MAX_UINT32, MAX_UINT32 };
    UINT32         Now;

    Now = InRange ? (1 + (Buttons & EFI_ABSP_TouchActive)) : 0;
    if (Now != mLast[Source]) {
      mLast[Source] = Now;
      DEBUG ((DEBUG_INFO, "Touch: %a %a at %u,%u z %u buttons 0x%x\n",
              (Source == TouchSourcePen) ? "pen" : "finger",
              !InRange ? "gone" : ((Buttons & EFI_ABSP_TouchActive) ? "down" : "hover"),
              X, Y, Z, Buttons));
    }
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (InRange) {
    mState.CurrentX = MIN (X, TOUCH_MAX_X);
    mState.CurrentY = MIN (Y, TOUCH_MAX_Y);
    mState.CurrentZ = MIN (Z, TOUCH_MAX_Z);
  } else {
    mState.CurrentZ = 0;
    Buttons         = 0;
  }
  mState.ActiveButtons = Buttons;
  mStateChanged        = TRUE;
  gBS->RestoreTPL (OldTpl);
}

STATIC
EFI_STATUS
EFIAPI
TouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ZeroMem (&mState, sizeof (mState));
  mStateChanged = FALSE;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TouchGetState (
  IN     EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN OUT EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mStateChanged) {
    CopyMem (State, &mState, sizeof (*State));
    mStateChanged = FALSE;
    Status        = EFI_SUCCESS;
  } else {
    Status = EFI_NOT_READY;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
VOID
EFIAPI
TouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mStateChanged) {
    gBS->SignalEvent (Event);
  }
}

STATIC
VOID
EFIAPI
TouchPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mHavePen) {
    WacomPenPoll ();
  }
  if (mHaveTouch) {
    MxtTouchPoll ();
  }
}

EFI_STATUS
EFIAPI
TouchDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&gTouchI2c);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&gTouchGpio);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mMode.AbsoluteMaxX = TOUCH_MAX_X;
  mMode.AbsoluteMaxY = TOUCH_MAX_Y;
  mMode.AbsoluteMaxZ = TOUCH_MAX_Z;
  mMode.Attributes   = EFI_ABSP_SupportsAltActive | EFI_ABSP_SupportsPressureAsZ;

  mAbsolutePointer.Reset    = TouchReset;
  mAbsolutePointer.GetState = TouchGetState;
  mAbsolutePointer.Mode     = &mMode;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, TouchWaitForInput, NULL,
                             &mAbsolutePointer.WaitForInput);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Either one alone is still a pointer.
  //
  mHaveTouch = !EFI_ERROR (MxtTouchInit ());
  mHavePen   = !EFI_ERROR (WacomPenInit ());
  if (!mHaveTouch && !mHavePen) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TouchPoll, NULL,
                             &mPollEvent);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mPollEvent, TimerPeriodic, TOUCH_POLL_100NS);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (&Handle,
                                                 &gEfiAbsolutePointerProtocolGuid, &mAbsolutePointer,
                                                 NULL);
}
