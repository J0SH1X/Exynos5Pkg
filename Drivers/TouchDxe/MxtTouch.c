/** @file
  Atmel maXTouch mXT1664S touchscreen (v1awifi).

  Wiring (board-vienna-input.c, gpio-v1a-3g-rev00.h): I2C bus 0 (GPB3[0:1]),
  address 0x4A; CHG GPD1(0), active low, low while messages are queued;
  power: tsp_vdd_1.8v (MAX77802 LDO29, also the I2C pull-ups), then
  GPIO_TOUCH_EN_1 GPX1(6), GPIO_TOUCH_EN GPX0(6) (XVDD), GPIO_TOUCH_RESET
  GPY7(4) (mxt_power_on()).

  The Linux driver (drivers/input/touchscreen/mxts.c) keeps the chip's
  config in its NVM, so this only powers it, reads the object table and
  decodes the first finger from T100 (multi-touch screen) messages, or T9
  on firmware that still has it.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "TouchDxe.h"

#define MXT_I2C_BUS   0
#define MXT_I2C_ADDR  0x4A

#define MXT_CHG_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPD1, 0)
#define MXT_EN1_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX1, 6)
#define MXT_EN_GPIO     EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX0, 6)
#define MXT_RESET_GPIO  EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPY7, 4)
#define MXT_SDA_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB3, 0)
#define MXT_SCL_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB3, 1)
#define GPIO_FUNC_I2C0  2

//
// MAX77802 PMIC on hs-i2c3 (bus 7, GPB2[2:3] func 3), address 0x09.
// LDO29CTRL1: [7:6] mode (3 = on), [5:0] (uV - 800000) / 50000.
//
#define PMIC_I2C_BUS      7
#define PMIC_I2C_ADDR     0x09
#define PMIC_SDA_GPIO     EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB2, 2)
#define PMIC_SCL_GPIO     EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB2, 3)
#define PMIC_LDO29_CTRL1  0x7C
#define PMIC_LDO_ON_1V8   (0xC0 | ((1800000 - 800000) / 50000))

//
// Object types.
//
#define MXT_GEN_MESSAGEPROCESSOR_T5    5
#define MXT_TOUCH_MULTITOUCHSCREEN_T9  9
#define MXT_TOUCH_MULTITOUCHSCREEN_T100  100
#define MXT_REPORT_NONE  0xFF

#define MXT_INFO_SIZE          7
#define MXT_OBJECT_TABLE       7
#define MXT_OBJECT_ENTRY_SIZE  6
#define MXT_MAX_MSG_SIZE       16

//
// T9 message: [1] status (0x80 detect, 0x20 release), [2-4] 12-bit X/Y,
// [6] amplitude.
//
#define MXT_T9_DETECT   0x80
#define MXT_T9_RELEASE  0x20

//
// T100: the first two report IDs are screen status, then one per touch.
// Touch message: [1] bit 7 detect, [6:4] type, [3:0] event; [2-3] X,
// [4-5] Y (little endian).  Config: [13-14] XRANGE, [24-25] YRANGE.
//
#define MXT_T100_SCREEN_REPORTS  2
#define MXT_T100_DETECT          0x80
#define MXT_T100_XRANGE          13
#define MXT_T100_YRANGE          24
#define MXT_T100_CFG_SIZE        26

STATIC UINT16   mMsgAddr;          // T5
STATIC UINT8    mMsgSize;
STATIC UINT8    mFingerReport;     // report ID of the first finger
STATIC BOOLEAN  mIsT100;
STATIC UINT16   mXRange;
STATIC UINT16   mYRange;
STATIC UINT8    mXDropBits;        // T9 only
STATIC UINT8    mYDropBits;

/**
  mXT registers are addressed with a 16-bit little-endian offset.
**/
STATIC
EFI_STATUS
MxtRead (
  IN  UINT16  Reg,
  OUT UINT8   *Buf,
  IN  UINT16  Len
  )
{
  UINT8           Addr[2];
  EXYNOS_I2C_MSG  Msgs[2];

  Addr[0]       = (UINT8)Reg;
  Addr[1]       = (UINT8)(Reg >> 8);
  Msgs[0].Addr  = MXT_I2C_ADDR;
  Msgs[0].Flags = 0;
  Msgs[0].Len   = sizeof (Addr);
  Msgs[0].Buf   = Addr;
  Msgs[1].Addr  = MXT_I2C_ADDR;
  Msgs[1].Flags = I2C_M_RD;
  Msgs[1].Len   = Len;
  Msgs[1].Buf   = Buf;

  return (gTouchI2c->Xfer (gTouchI2c, MXT_I2C_BUS, Msgs, 2) == 2) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
VOID
MxtReportFinger (
  IN BOOLEAN  Down,
  IN UINT32   X,
  IN UINT32   Y,
  IN UINT32   Z
  )
{
  if (!Down) {
    TouchReport (TouchSourceFinger, FALSE, 0, 0, 0, 0);
    return;
  }

  X = MIN (X, (UINT32)mXRange) * (TOUCH_MAX_X + 1) / ((UINT32)mXRange + 1);
  Y = MIN (Y, (UINT32)mYRange) * (TOUCH_MAX_Y + 1) / ((UINT32)mYRange + 1);
  TouchReport (TouchSourceFinger, TRUE, X, Y, Z, EFI_ABSP_TouchActive);
}

STATIC
VOID
MxtHandleMessage (
  IN UINT8  *Msg
  )
{
  //
  // First finger only: an absolute pointer has one contact.
  //
  if (Msg[0] != mFingerReport) {
    return;
  }

  if (mIsT100) {
    MxtReportFinger ((Msg[1] & MXT_T100_DETECT) != 0,
                     Msg[2] | ((UINT32)Msg[3] << 8),
                     Msg[4] | ((UINT32)Msg[5] << 8),
                     TOUCH_MAX_Z / 2);
    return;
  }

  if ((Msg[1] & MXT_T9_RELEASE) != 0) {
    MxtReportFinger (FALSE, 0, 0, 0);
  } else if ((Msg[1] & MXT_T9_DETECT) != 0) {
    MxtReportFinger (TRUE,
                     (((UINT32)Msg[2] << 4) | (Msg[4] >> 4)) >> mXDropBits,
                     (((UINT32)Msg[3] << 4) | (Msg[4] & 0xF)) >> mYDropBits,
                     (UINT32)Msg[6] * TOUCH_MAX_Z / 255);
  }
}

/**
  Drain the message queue while CHG is low.
**/
VOID
MxtTouchPoll (
  VOID
  )
{
  UINT8  Msg[MXT_MAX_MSG_SIZE];
  UINTN  Count;

  for (Count = 0; Count < 32; Count++) {
    if (gTouchGpio->Get (gTouchGpio, MXT_CHG_GPIO) != 0) {
      break;
    }
    if (EFI_ERROR (MxtRead (mMsgAddr, Msg, mMsgSize)) || (Msg[0] == MXT_REPORT_NONE)) {
      break;
    }
    MxtHandleMessage (Msg);
  }
}

/**
  Read the info block and object table: find T5 and the touch object, and
  the coordinate range.
**/
STATIC
EFI_STATUS
MxtProbe (
  VOID
  )
{
  UINT8       Info[MXT_INFO_SIZE];
  UINT8       *Table;
  UINT8       *Obj;
  UINT8       Cfg[MXT_T100_CFG_SIZE];
  UINT16      T9Addr;
  UINT16      T100Addr;
  UINT8       T9Report;
  UINT8       T100Report;
  UINT8       ReportId;
  UINTN       Index;
  UINTN       Try;
  EFI_STATUS  Status;

  //
  // The chip needs a while after reset before it answers.
  //
  Status = EFI_DEVICE_ERROR;
  for (Try = 0; (Try < 10) && EFI_ERROR (Status); Try++) {
    Status = MxtRead (0, Info, sizeof (Info));
    if (EFI_ERROR (Status)) {
      gBS->Stall (50000);
    }
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "mXT: no answer at 0x%02x on I2C%u\n", MXT_I2C_ADDR, MXT_I2C_BUS));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "mXT: family 0x%02x variant 0x%02x version 0x%02x build 0x%02x, %u objects\n",
          Info[0], Info[1], Info[2], Info[3], Info[6]));

  Table = AllocatePool (Info[6] * MXT_OBJECT_ENTRY_SIZE);
  if (Table == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = MxtRead (MXT_OBJECT_TABLE, Table, Info[6] * MXT_OBJECT_ENTRY_SIZE);
  if (EFI_ERROR (Status)) {
    FreePool (Table);
    return Status;
  }

  T9Addr     = 0;
  T100Addr   = 0;
  T9Report   = 0;
  T100Report = 0;
  ReportId   = 1;
  for (Index = 0; Index < Info[6]; Index++) {
    Obj = Table + Index * MXT_OBJECT_ENTRY_SIZE;
    switch (Obj[0]) {
      case MXT_GEN_MESSAGEPROCESSOR_T5:
        mMsgAddr = Obj[1] | ((UINT16)Obj[2] << 8);
        mMsgSize = (UINT8)MIN (Obj[3] + 1, MXT_MAX_MSG_SIZE);
        break;
      case MXT_TOUCH_MULTITOUCHSCREEN_T9:
        T9Addr   = Obj[1] | ((UINT16)Obj[2] << 8);
        T9Report = ReportId;
        break;
      case MXT_TOUCH_MULTITOUCHSCREEN_T100:
        T100Addr   = Obj[1] | ((UINT16)Obj[2] << 8);
        T100Report = ReportId;
        break;
    }
    // Obj[4] = instances - 1, Obj[5] = report IDs per instance.
    ReportId += Obj[5] * (Obj[4] + 1);
  }
  FreePool (Table);

  if ((mMsgAddr == 0) || ((T9Addr == 0) && (T100Addr == 0))) {
    DEBUG ((DEBUG_ERROR, "mXT: no T5 or touch object\n"));
    return EFI_UNSUPPORTED;
  }

  if (T100Addr != 0) {
    mIsT100       = TRUE;
    mFingerReport = T100Report + MXT_T100_SCREEN_REPORTS;
    Status        = MxtRead (T100Addr, Cfg, sizeof (Cfg));
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mXRange = Cfg[MXT_T100_XRANGE] | ((UINT16)Cfg[MXT_T100_XRANGE + 1] << 8);
    mYRange = Cfg[MXT_T100_YRANGE] | ((UINT16)Cfg[MXT_T100_YRANGE + 1] << 8);
  } else {
    //
    // T9: [9] ORIENT (bit 0 swaps X/Y), [18-19] XRANGE, [20-21] YRANGE; a
    // range below 1024 is reported in 12-bit units (2 extra bits).
    //
    mIsT100       = FALSE;
    mFingerReport = T9Report;
    Status        = MxtRead (T9Addr, Cfg, 22);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mXRange = Cfg[18] | ((UINT16)Cfg[19] << 8);
    mYRange = Cfg[20] | ((UINT16)Cfg[21] << 8);
    if ((Cfg[9] & BIT0) != 0) {
      mXDropBits = ((Cfg[21] & 0xC) == 0) ? 2 : 0;
      mYDropBits = ((Cfg[19] & 0xC) == 0) ? 2 : 0;
    } else {
      mXDropBits = ((Cfg[19] & 0xC) == 0) ? 2 : 0;
      mYDropBits = ((Cfg[21] & 0xC) == 0) ? 2 : 0;
    }
  }

  //
  // mxt_platform_data max_x/max_y = 4095 if the config does not say.
  //
  if (mXRange == 0) {
    mXRange = 4095;
  }
  if (mYRange == 0) {
    mYRange = 4095;
  }

  DEBUG ((DEBUG_INFO, "mXT: %a, T5 @0x%x (%u bytes), finger report %u, range %ux%u\n",
          mIsT100 ? "T100" : "T9", mMsgAddr, mMsgSize, mFingerReport,
          mXRange + 1, mYRange + 1));
  return EFI_SUCCESS;
}

/**
  Switch on tsp_vdd_1.8v (MAX77802 LDO29) if the bootloader left it off.
**/
STATIC
VOID
MxtPmicPowerOn (
  VOID
  )
{
  UINT8  Ctrl;
  UINT8  Buf[2];

  gTouchGpio->Config (gTouchGpio, PMIC_SDA_GPIO, 3, EXYNOS_GPIO_PULL_UP);
  gTouchGpio->Config (gTouchGpio, PMIC_SCL_GPIO, 3, EXYNOS_GPIO_PULL_UP);

  if (gTouchI2c->Read (gTouchI2c, PMIC_I2C_BUS, PMIC_I2C_ADDR, PMIC_LDO29_CTRL1, &Ctrl, 1) != 1) {
    DEBUG ((DEBUG_ERROR, "mXT: MAX77802 not answering on I2C%u\n", PMIC_I2C_BUS));
    return;
  }
  if ((Ctrl >> 6) != 0) {
    return;     // already on
  }

  Buf[0] = PMIC_LDO29_CTRL1;
  Buf[1] = PMIC_LDO_ON_1V8;
  if (gTouchI2c->Write (gTouchI2c, PMIC_I2C_BUS, PMIC_I2C_ADDR, Buf, 2) != 2) {
    DEBUG ((DEBUG_ERROR, "mXT: LDO29 enable failed\n"));
    return;
  }
  gBS->Stall (5000);
}

STATIC
VOID
MxtSetOutput (
  IN UINTN  Pin,
  IN UINTN  Value
  )
{
  gTouchGpio->Set (gTouchGpio, Pin, Value);
  gTouchGpio->Config (gTouchGpio, Pin, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  gTouchGpio->Set (gTouchGpio, Pin, Value);
}

EFI_STATUS
MxtTouchInit (
  VOID
  )
{
  //
  // mxt_power_on(): pull-up rail, XVDD (EN_1, then EN), reset released,
  // CHG as input.  Held in reset first so the chip starts clean.
  //
  MxtSetOutput (MXT_RESET_GPIO, 0);
  MxtPmicPowerOn ();

  gTouchGpio->Config (gTouchGpio, MXT_SDA_GPIO, GPIO_FUNC_I2C0, EXYNOS_GPIO_PULL_NONE);
  gTouchGpio->Config (gTouchGpio, MXT_SCL_GPIO, GPIO_FUNC_I2C0, EXYNOS_GPIO_PULL_NONE);
  gTouchGpio->Config (gTouchGpio, MXT_CHG_GPIO, EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_NONE);

  MxtSetOutput (MXT_EN1_GPIO, 1);
  gBS->Stall (1000);
  MxtSetOutput (MXT_EN_GPIO, 1);
  gBS->Stall (30000);
  MxtSetOutput (MXT_RESET_GPIO, 1);
  gBS->Stall (100000);

  return MxtProbe ();
}
