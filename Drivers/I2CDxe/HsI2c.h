/** @file
  Exynos5420 high-speed I2C polling master (HsI2c.c).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __HS_I2C_H__
#define __HS_I2C_H__

INTN
HsI2cXfer (
  IN UINT32          Channel,
  IN EXYNOS_I2C_MSG  *Msgs,
  IN INTN            Num
  );

#endif
