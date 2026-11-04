/** @file
  Bit-banged I2C master for the camera sensor's own bus (the ISP I2C pins
  GPF0(0/1) rear, GPF0(4/5) front), to read and set the sensor's exposure and
  gain registers while the A5 firmware streams the sensor but sends it no
  exposure control (it only does so for shot requests).  16-bit register
  addresses, 8-bit data (SMIA / Sony register maps).

  The pins are taken from the ISP while a transfer runs (GPIO outputs /
  inputs, the board has the pull-ups) and handed back after it.  Callers
  serialise transfers.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __CAM_SENSOR_I2C_H__
#define __CAM_SENSOR_I2C_H__

#include <ExynosEnv.h>

typedef struct {
  UINTN   Gpio;       // mapping of the GPE/GPF controller (CAM_GPIO_LB_PHYS)
  UINT32  Bank;       // bank offset in it: GPF0 = 0x40
  UINT32  Sda;        // pin numbers in the bank
  UINT32  Scl;
  UINT8   Addr;       // 7-bit slave address
} CAM_I2C;

//
// 0 on success, < 0: -1 no ACK for the address, -2 no ACK for data, -3 bus stuck.
//
INTN
CamI2cRead (
  IN  CAM_I2C  *I2c,
  IN  UINT16   Reg,
  OUT UINT8    *Value
  );

INTN
CamI2cWrite (
  IN CAM_I2C  *I2c,
  IN UINT16   Reg,
  IN UINT8    Value
  );

INTN
CamI2cRead16 (
  IN  CAM_I2C  *I2c,
  IN  UINT16   Reg,
  OUT UINT16   *Value
  );

INTN
CamI2cWrite16 (
  IN CAM_I2C  *I2c,
  IN UINT16   Reg,
  IN UINT16   Value
  );

#endif // __CAM_SENSOR_I2C_H__
