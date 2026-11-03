/** @file
  Sensor board setup for v1awifi (see SensorDxe.inf).  From the v1awifi
  kernel: board-universal5420-sensor.c (I2C3 devices, AK8963C reset
  GPG2(0)), setup-i2c3.c (GPA1(2)/(3) function 3, pull-up),
  board-v1-pmic.c (vcc_sensor_2.8v = LDO34, always on) and
  board-vienna-input.c (hall = GPX0(1), active low, pull-up).

    BMA255  accelerometer  I2C3 0x18   INT1 GPX1(3), INT2 GPX1(7)
    BMG160  gyroscope      I2C3 0x68   INT GPX3(1), DRDY GPX3(4)
    AK8963C magnetometer   I2C3 0x0C   INT GPX3(3), reset GPG2(0)
    CM3323  RGBW light     I2C3 0x10   no interrupt

  The chips themselves are left to the OS drivers (the BMA255 comes up in
  suspend; its driver resets it).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosI2c.h>

#define PMIC_BUS          7
#define PMIC_ADDR         0x09
#define PMIC_LDO34_CTRL1  0x81
#define PMIC_LDO_ON_2V8   (0xC0 | ((2800000 - 800000) / 50000))   // P-channel LDO: 800 mV + 50 mV steps

#define GPA1_BASE         0x14010020       // I2C3 SDA = GPA1(2), SCL = GPA1(3)
#define GPB2_BASE         0x140100A0       // HS-I2C3 (bus 7, PMIC) SDA = GPB2(2), SCL = GPB2(3)
#define GPG2_BASE         0x140000C0       // AK8963C reset = GPG2(0), active low
#define GPX0_BASE         0x13400C00       // hall = GPX0(1)
#define GPIO_CON          0x0
#define GPIO_DAT          0x4
#define GPIO_PUD          0x8

EFI_STATUS
EFIAPI
SensorDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EXYNOS_I2C_PROTOCOL  *I2c;
  EFI_STATUS           Status;
  UINT8                Ctrl;
  UINT8                Buf[2];

  //
  // Supply: the bootloader leaves LDO34 on; switch it on if not.
  //
  // PMIC bus pins first (function 3, pull-up, as CpuFreqDxe sets them):
  // a transfer on unmuxed pins left HS-I2C3 unusable, CpuFreqDxe then
  // could not reach the PMIC and the cores stayed at 800 MHz.
  //
  MmioAndThenOr32 (GPB2_BASE + GPIO_CON, ~(0xFFu << 8), (0x3u << 8) | (0x3u << 12));
  MmioAndThenOr32 (GPB2_BASE + GPIO_PUD, ~(0xFu << 4), (0x3u << 4) | (0x3u << 6));

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&I2c);
  if (!EFI_ERROR (Status) && (I2c->Read (I2c, PMIC_BUS, PMIC_ADDR, PMIC_LDO34_CTRL1, &Ctrl, 1) == 1)) {
    if (Ctrl != PMIC_LDO_ON_2V8) {
      Buf[0] = PMIC_LDO34_CTRL1;
      Buf[1] = PMIC_LDO_ON_2V8;
      I2c->Write (I2c, PMIC_BUS, PMIC_ADDR, Buf, 2);
      gBS->Stall (20 * 1000);
      DEBUG ((DEBUG_INFO, "SensorDxe: LDO34 was 0x%02x, now 2.8 V\n", Ctrl));
    }
  } else {
    DEBUG ((DEBUG_ERROR, "SensorDxe: MAX77802 not reachable, sensor supply unchecked\n"));
  }

  //
  // I2C3 pins: function 3, pull-up.
  //
  MmioAndThenOr32 (GPA1_BASE + GPIO_CON, ~(0xFFu << 8), (0x3u << 8) | (0x3u << 12));
  MmioAndThenOr32 (GPA1_BASE + GPIO_PUD, ~(0xFu << 4), (0x3u << 4) | (0x3u << 6));

  //
  // Hall input with pull-up (the OS's GPIO driver switches it to EINT).
  //
  MmioAnd32 (GPX0_BASE + GPIO_CON, ~(0xFu << 4));
  MmioAndThenOr32 (GPX0_BASE + GPIO_PUD, ~(0x3u << 2), 0x3u << 2);

  //
  // AK8963C out of reset: output, low for 1 ms, then high.
  //
  MmioAnd32 (GPG2_BASE + GPIO_PUD, ~0x3u);
  MmioAnd32 (GPG2_BASE + GPIO_DAT, ~1u);
  MmioAndThenOr32 (GPG2_BASE + GPIO_CON, ~0xFu, 0x1u);
  gBS->Stall (1000);
  MmioOr32 (GPG2_BASE + GPIO_DAT, 1u);

  DEBUG ((DEBUG_INFO, "SensorDxe: I2C3 pins, hall input and AK8963C reset set up\n"));
  return EFI_SUCCESS;
}
