/** @file
  Bit-banged I2C on GPIO pins (GpioI2c.c).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __GPIO_I2C_H__
#define __GPIO_I2C_H__

INTN
GpioI2cXfer (
  IN UINT32          Bus,
  IN EXYNOS_I2C_MSG  *Msgs,
  IN INTN            Num
  );

#endif
