/** @file
  S Pen digitizer (Wacom G9PLL) on v1awifi.

  Wiring (board-vienna-input.c, gpio-v1a-3g-rev00.h): hs-i2c4 = I2C bus 8
  (GPB3[4:5] func 2), address 0x56; IRQ GPX3(5) (rising edge = packet
  ready, stays high until it is read); PDCT GPD1(2); FWE1 GPD1(3) (firmware
  flash mode, kept low); power switch PEN_LDO_EN GPD1(4).  No reset line:
  wacom_reset_hw() is a power cycle.

  Packets (drivers/input/wacom/wacom_i2c_func.c, COM_COORD_NUM = 12 on V1A):
    [0] status: 0x80 ready/in range, 0x40 eraser, 0x20 side button, 0x10 tip
    [1-2] X, [3-4] Y, [5-6] pressure (big endian), rest unused here
  already in screen orientation on V1A (x_invert = y_invert = xy_switch = 0).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "TouchDxe.h"

#define PEN_I2C_BUS   8
#define PEN_I2C_ADDR  0x56

#define PEN_IRQ_GPIO     EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX3, 5)
#define PEN_PDCT_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPD1, 2)
#define PEN_FWE1_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPD1, 3)
#define PEN_LDO_EN_GPIO  EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPD1, 4)
#define PEN_SDA_GPIO     EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB3, 4)
#define PEN_SCL_GPIO     EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB3, 5)
#define GPIO_FUNC_HSI2C4 2

#define WACOM_COM_QUERY       0x2A
#define WACOM_QUERY_SIZE      9
#define WACOM_QUERY_DELAY_MS  100
#define WACOM_COORD_SIZE      12
#define WACOM_POWER_OFF_MS    100     // wacom_reset_hw()
#define WACOM_POWER_ON_MS     200

#define WACOM_IN_RANGE  0x80
#define WACOM_ERASER    0x40
#define WACOM_SIDE      0x20
#define WACOM_TIP       0x10

//
// include/linux/wacom_i2c.h, CONFIG_V1A.
//
#define WACOM_MAX_COORD_X   26266
#define WACOM_MAX_COORD_Y   16416
#define WACOM_MAX_PRESSURE  1023

STATIC UINT32  mMaxX = WACOM_MAX_COORD_X;
STATIC UINT32  mMaxY = WACOM_MAX_COORD_Y;
STATIC UINT32  mMaxPressure = WACOM_MAX_PRESSURE;

STATIC
EFI_STATUS
PenRead (
  OUT UINT8   *Buf,
  IN  UINT16  Len
  )
{
  EXYNOS_I2C_MSG  Msg;

  Msg.Addr  = PEN_I2C_ADDR;
  Msg.Flags = I2C_M_RD;
  Msg.Len   = Len;
  Msg.Buf   = Buf;

  return (gTouchI2c->Xfer (gTouchI2c, PEN_I2C_BUS, &Msg, 1) == 1) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
VOID
PenHandlePacket (
  IN UINT8  *Data
  )
{
  UINT32  X;
  UINT32  Y;
  UINT32  Pressure;
  UINT32  Buttons;

  if ((Data[0] & WACOM_IN_RANGE) == 0) {
    TouchReport (TouchSourcePen, FALSE, 0, 0, 0, 0);
    return;
  }

  X        = ((UINT32)Data[1] << 8) | Data[2];
  Y        = ((UINT32)Data[3] << 8) | Data[4];
  Pressure = ((UINT32)Data[5] << 8) | Data[6];
  if ((X > mMaxX) || (Y > mMaxY)) {
    return;      // wacom_i2c_coord_range(): outside the active area
  }

  Buttons = 0;
  if ((Data[0] & WACOM_TIP) != 0) {
    Buttons |= EFI_ABSP_TouchActive;
  }
  if ((Data[0] & (WACOM_SIDE | WACOM_ERASER)) != 0) {
    Buttons |= EFI_ABS_AltActive;
  }

  TouchReport (TouchSourcePen, TRUE,
               X * TOUCH_MAX_X / mMaxX,
               Y * TOUCH_MAX_Y / mMaxY,
               MIN (Pressure, mMaxPressure) * TOUCH_MAX_Z / mMaxPressure,
               Buttons);
}

/**
  IRQ high: a packet is waiting.
**/
VOID
WacomPenPoll (
  VOID
  )
{
  UINT8  Data[WACOM_COORD_SIZE];
  UINTN  Count;

  for (Count = 0; Count < 4; Count++) {
    if (gTouchGpio->Get (gTouchGpio, PEN_IRQ_GPIO) == 0) {
      break;
    }
    if (EFI_ERROR (PenRead (Data, sizeof (Data)))) {
      break;
    }
    PenHandlePacket (Data);
  }
}

/**
  Ask the digitizer who it is (also proves it is there).
**/
STATIC
EFI_STATUS
PenQuery (
  VOID
  )
{
  UINT8           Command;
  UINT8           Data[WACOM_QUERY_SIZE];
  EXYNOS_I2C_MSG  Msg;
  UINTN           Try;

  for (Try = 0; Try < 3; Try++) {
    Command   = WACOM_COM_QUERY;
    Msg.Addr  = PEN_I2C_ADDR;
    Msg.Flags = 0;
    Msg.Len   = 1;
    Msg.Buf   = &Command;
    if (gTouchI2c->Xfer (gTouchI2c, PEN_I2C_BUS, &Msg, 1) != 1) {
      gBS->Stall (WACOM_QUERY_DELAY_MS * 1000);
      continue;
    }
    gBS->Stall (WACOM_QUERY_DELAY_MS * 1000);
    if (!EFI_ERROR (PenRead (Data, sizeof (Data))) && (Data[0] == 0x0F)) {
      DEBUG ((DEBUG_INFO, "Wacom: max %u x %u, pressure %u, firmware 0x%02x%02x\n",
              (Data[1] << 8) | Data[2], (Data[3] << 8) | Data[4],
              (Data[5] << 8) | Data[6], Data[7], Data[8]));
      if (((Data[5] << 8) | Data[6]) != 0) {
        mMaxPressure = (Data[5] << 8) | Data[6];
      }
      return EFI_SUCCESS;
    }
  }

  DEBUG ((DEBUG_ERROR, "Wacom: no answer at 0x%02x on I2C%u\n", PEN_I2C_ADDR, PEN_I2C_BUS));
  return EFI_NOT_FOUND;
}

EFI_STATUS
WacomPenInit (
  VOID
  )
{
  gTouchGpio->Config (gTouchGpio, PEN_SDA_GPIO, GPIO_FUNC_HSI2C4, EXYNOS_GPIO_PULL_UP);
  gTouchGpio->Config (gTouchGpio, PEN_SCL_GPIO, GPIO_FUNC_HSI2C4, EXYNOS_GPIO_PULL_UP);

  //
  // Normal (not flash) mode.
  //
  gTouchGpio->Set (gTouchGpio, PEN_FWE1_GPIO, 0);
  gTouchGpio->Config (gTouchGpio, PEN_FWE1_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);

  //
  // wacom_reset_hw(): suspend (power off, IRQ pulled down), 100 ms, resume
  // (PDCT driven high while powering up, then back to input).
  //
  gTouchGpio->Set (gTouchGpio, PEN_LDO_EN_GPIO, 0);
  gTouchGpio->Config (gTouchGpio, PEN_LDO_EN_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  gTouchGpio->Config (gTouchGpio, PEN_IRQ_GPIO, EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_DOWN);
  gBS->Stall (WACOM_POWER_OFF_MS * 1000);

  gTouchGpio->Set (gTouchGpio, PEN_PDCT_GPIO, 1);
  gTouchGpio->Config (gTouchGpio, PEN_PDCT_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  gTouchGpio->Set (gTouchGpio, PEN_LDO_EN_GPIO, 1);
  gTouchGpio->Config (gTouchGpio, PEN_PDCT_GPIO, EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_NONE);
  gTouchGpio->Config (gTouchGpio, PEN_IRQ_GPIO, EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_NONE);
  gBS->Stall (WACOM_POWER_ON_MS * 1000);

  return PenQuery ();
}
