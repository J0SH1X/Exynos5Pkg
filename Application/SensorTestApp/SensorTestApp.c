/** @file
  Sensor test for v1awifi: switches on the sensor supply, muxes I2C3,
  probes the four I2C sensors and the hall (cover) input, then logs ten
  readings a second apart (tilt / turn the tablet, cover the light sensor,
  hold a magnet to the cover edge while it runs).

  Chips and addresses from the v1awifi kernel
  (arch/arm/mach-exynos/board-universal5420-sensor.c, drivers/sensors/):
    BMA255 accelerometer   I2C3 0x18, chip ID 0xFA (reg 0x00), data 0x02..0x07
    BMG160 gyroscope       I2C3 0x68, chip ID 0x0F (reg 0x00), data 0x02..0x07
    AK8963C magnetometer   I2C3 0x0C, WIA 0x48 (reg 0x00), single measure
                           CNTL1 0x0A = 0x11, ST1 0x02, HXL..HZH 0x03..0x08
    CM3323 RGBW light      I2C3 0x10, CS_CONF1 0x00 (0 = on), R/G/B/W 0x08..0x0B
                           (16-bit, LSB first)
    Hall (cover)           GPX0(1), active low (gpio_keys flip cover)
  Supply: vcc_sensor_2.8v = MAX77802 LDO34 (CTRL1 0x81, P-channel LDO,
  800 mV + 50 mV steps, mode bits [7:6] = 3 for on), PMIC on hs-i2c3 = bus 7.
  I2C3 pins: GPA1(2) SDA / GPA1(3) SCL, function 3, pull-up (setup-i2c3.c).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/ExynosI2c.h>

#define SENSOR_BUS        3
#define PMIC_BUS          7
#define PMIC_ADDR         0x09
#define PMIC_LDO34_CTRL1  0x81
#define PMIC_LDO_ON_2V8   (0xC0 | ((2800000 - 800000) / 50000))

#define GPA1_BASE         0x14010020
#define GPX0_BASE         0x13400C00
#define GPG2_BASE         0x140000C0       // GPG2(0) = AK8963C reset, active low
#define GPIO_CON          0x0
#define GPIO_DAT          0x4
#define GPIO_PUD          0x8

#define BMA255_ADDR       0x18
#define BMG160_ADDR       0x68
#define AK8963_ADDR       0x0C
#define CM3323_ADDR       0x10

STATIC EXYNOS_I2C_PROTOCOL  *mI2c;

#define LOG(...)  do { DEBUG ((DEBUG_INFO, __VA_ARGS__)); } while (0)

STATIC
INTN
ReadRegs (
  IN  UINT16  Chip,
  IN  UINT8   Reg,
  OUT UINT8   *Buf,
  IN  UINTN   Count
  )
{
  return mI2c->Read (mI2c, SENSOR_BUS, Chip, Reg, Buf, Count);
}

STATIC
INTN
WriteReg (
  IN UINT16  Chip,
  IN UINT8   Reg,
  IN UINT8   Value
  )
{
  UINT8  Buf[2];

  Buf[0] = Reg;
  Buf[1] = Value;
  return mI2c->Write (mI2c, SENSOR_BUS, Chip, Buf, 2);
}

STATIC
VOID
SensorPower (
  VOID
  )
{
  UINT8  Ctrl;
  UINT8  Buf[2];

  if (mI2c->Read (mI2c, PMIC_BUS, PMIC_ADDR, PMIC_LDO34_CTRL1, &Ctrl, 1) != 1) {
    LOG ("Sensors: MAX77802 not answering on I2C%u\n", PMIC_BUS);
    return;
  }
  LOG ("Sensors: LDO34 CTRL1 0x%02x (%a, %u mV)\n", Ctrl, ((Ctrl >> 6) == 3) ? "on" : "not on",
       800 + (Ctrl & 0x3F) * 50);
  if (Ctrl != PMIC_LDO_ON_2V8) {
    Buf[0] = PMIC_LDO34_CTRL1;
    Buf[1] = PMIC_LDO_ON_2V8;
    if (mI2c->Write (mI2c, PMIC_BUS, PMIC_ADDR, Buf, 2) != 2) {
      LOG ("Sensors: LDO34 enable failed\n");
      return;
    }
    LOG ("Sensors: LDO34 set to 2.8 V\n");
    gBS->Stall (20 * 1000);
  }
}

STATIC
VOID
SensorPins (
  VOID
  )
{
  //
  // GPA1(2), GPA1(3): function 3 (I2C3), pull-up.
  //
  MmioAndThenOr32 (GPA1_BASE + GPIO_CON, ~(0xFFu << 8), (0x3u << 8) | (0x3u << 12));
  MmioAndThenOr32 (GPA1_BASE + GPIO_PUD, ~(0xFu << 4), (0x3u << 4) | (0x3u << 6));
  //
  // GPX0(1) hall: input, pull-up.
  //
  MmioAnd32 (GPX0_BASE + GPIO_CON, ~(0xFu << 4));
  MmioAndThenOr32 (GPX0_BASE + GPIO_PUD, ~(0x3u << 2), 0x3u << 2);
  //
  // GPG2(0) AK8963C reset: output, low then high (out of reset).
  //
  MmioAnd32 (GPG2_BASE + GPIO_PUD, ~0x3u);
  MmioAnd32 (GPG2_BASE + GPIO_DAT, ~1u);
  MmioAndThenOr32 (GPG2_BASE + GPIO_CON, ~0xFu, 0x1u);
  gBS->Stall (1000);
  MmioOr32 (GPG2_BASE + GPIO_DAT, 1u);
  gBS->Stall (10 * 1000);
}

STATIC
BOOLEAN
Probe (
  IN CONST CHAR8  *Name,
  IN UINT16       Chip,
  IN INTN         Expected       // -1: any answer
  )
{
  UINT8  Id;

  if (ReadRegs (Chip, 0x00, &Id, 1) != 1) {
    LOG ("Sensors: %a at 0x%02x: no answer\n", Name, Chip);
    return FALSE;
  }
  if ((Expected >= 0) && (Id != (UINT8)Expected)) {
    LOG ("Sensors: %a at 0x%02x: ID 0x%02x, expected 0x%02x\n", Name, Chip, Id, (UINT8)Expected);
    return FALSE;
  }
  LOG ("Sensors: %a at 0x%02x: ID 0x%02x - ok\n", Name, Chip, Id);
  return TRUE;
}

STATIC
INT16
Le16 (
  IN UINT8  *Buf
  )
{
  return (INT16)(Buf[0] | (Buf[1] << 8));
}

EFI_STATUS
EFIAPI
SensorTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  BOOLEAN     HaveAcc;
  BOOLEAN     HaveGyro;
  BOOLEAN     HaveMag;
  BOOLEAN     HaveLight;
  UINT8       Buf[8];
  UINT8       St1;
  UINTN       Round;
  UINTN       Wait;

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&mI2c);
  if (EFI_ERROR (Status)) {
    LOG ("Sensors: I2C protocol missing: %r\n", Status);
    return Status;
  }

  SensorPower ();
  SensorPins ();

  HaveAcc   = Probe ("BMA255 accelerometer", BMA255_ADDR, 0xFA);
  HaveGyro  = Probe ("BMG160 gyroscope", BMG160_ADDR, 0x0F);
  HaveMag   = Probe ("AK8963C magnetometer", AK8963_ADDR, 0x48);
  HaveLight = Probe ("CM3323 light sensor", CM3323_ADDR, -1);
  if (HaveAcc) {
    UINT8  Mode;

    ReadRegs (BMA255_ADDR, 0x11, &Mode, 1);
    LOG ("Sensors: BMA255 PMU_LPW 0x%02x, soft reset + normal mode\n", Mode);
    WriteReg (BMA255_ADDR, 0x14, 0xB6);            // BGW_SOFTRESET
    gBS->Stall (5 * 1000);                         // 1.8 ms start-up after reset
    WriteReg (BMA255_ADDR, 0x11, 0x00);            // PMU_LPW: normal
    gBS->Stall (5 * 1000);
    ReadRegs (BMA255_ADDR, 0x11, &Mode, 1);
    LOG ("Sensors: BMA255 PMU_LPW now 0x%02x\n", Mode);
  }
  if (HaveGyro) {
    UINT8  Mode;

    ReadRegs (BMG160_ADDR, 0x11, &Mode, 1);
    LOG ("Sensors: BMG160 MODE_LPM1 0x%02x, setting normal mode\n", Mode);
    WriteReg (BMG160_ADDR, 0x11, 0x00);            // MODE_LPM1: normal
    gBS->Stall (60 * 1000);                        // start-up from suspend: 30 ms typ.
  }
  if (HaveLight) {
    UINT8  Conf[3];

    Conf[0] = 0x00;                                // CS_CONF1 (16 bits, LSB first): on, 40 ms
    Conf[1] = 0x00;
    Conf[2] = 0x00;
    mI2c->Write (mI2c, SENSOR_BUS, CM3323_ADDR, Conf, 3);
    gBS->Stall (100 * 1000);
    ReadRegs (CM3323_ADDR, 0x00, Conf, 2);
    LOG ("Sensors: CM3323 CS_CONF1 now 0x%02x%02x\n", Conf[1], Conf[0]);
  }
  LOG ("Sensors: hall GPX0(1) = %u (%a)\n", (MmioRead32 (GPX0_BASE + GPIO_DAT) >> 1) & 1,
       (((MmioRead32 (GPX0_BASE + GPIO_DAT) >> 1) & 1) == 0) ? "magnet / cover closed" : "open");

  for (Round = 0; Round < 15; Round++) {
    gBS->Stall (1000 * 1000);
    LOG ("Sensors: --- reading %u\n", (UINT32)Round + 1);
    if (HaveAcc && (ReadRegs (BMA255_ADDR, 0x02, Buf, 6) == 6)) {
      // 12-bit left-aligned; +-2 g range: 1 g = 1024 counts
      LOG ("Sensors:   accel x %5d y %5d z %5d (1 g = 1024)\n", Le16 (&Buf[0]) >> 4, Le16 (&Buf[2]) >> 4,
           Le16 (&Buf[4]) >> 4);
    }
    if (HaveGyro && (ReadRegs (BMG160_ADDR, 0x02, Buf, 6) == 6)) {
      // +-2000 dps range: 16.4 counts per dps
      LOG ("Sensors:   gyro  x %6d y %6d z %6d (16.4 per dps)\n", Le16 (&Buf[0]), Le16 (&Buf[2]), Le16 (&Buf[4]));
    }
    if (HaveMag) {
      WriteReg (AK8963_ADDR, 0x0A, 0x11);          // CNTL1: 16-bit, single measurement
      St1 = 0;
      for (Wait = 0; (Wait < 20) && ((St1 & 1) == 0); Wait++) {
        gBS->Stall (1000);
        ReadRegs (AK8963_ADDR, 0x02, &St1, 1);
      }
      if (((St1 & 1) != 0) && (ReadRegs (AK8963_ADDR, 0x03, Buf, 7) == 7)) {
        // HXL..HZH, then ST2 (must be read to finish the measurement); 0.15 uT per count
        LOG ("Sensors:   mag   x %6d y %6d z %6d (0.15 uT) ST2 0x%02x\n", Le16 (&Buf[0]), Le16 (&Buf[2]),
             Le16 (&Buf[4]), Buf[6]);
      } else {
        LOG ("Sensors:   mag   no data ready (ST1 0x%02x)\n", St1);
      }
    }
    if (HaveLight) {
      UINT8  R[2], G[2], B[2], W[2];

      if ((ReadRegs (CM3323_ADDR, 0x08, R, 2) == 2) && (ReadRegs (CM3323_ADDR, 0x09, G, 2) == 2) &&
          (ReadRegs (CM3323_ADDR, 0x0A, B, 2) == 2) && (ReadRegs (CM3323_ADDR, 0x0B, W, 2) == 2))
      {
        LOG ("Sensors:   light R %5u G %5u B %5u W %5u\n", (UINT16)Le16 (R), (UINT16)Le16 (G), (UINT16)Le16 (B),
             (UINT16)Le16 (W));
      }
    }
    LOG ("Sensors:   hall  %a\n", (((MmioRead32 (GPX0_BASE + GPIO_DAT) >> 1) & 1) == 0) ? "closed" : "open");
  }
  LOG ("Sensors: done\n");
  return EFI_SUCCESS;
}
