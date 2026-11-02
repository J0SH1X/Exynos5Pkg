/** @file
  UEFI environment for the code shared with the Windows drivers
  (Drivers/I2CDxe/GpioI2c.c, Drivers/BatteryDxe/BatteryCore.c).  The
  Windows tree has its own ExynosEnv.h with the same names (found through
  the include path, hence the angle brackets in the shared files).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_ENV_H__
#define __EXYNOS_ENV_H__

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosI2c.h>

//
// GPIO controller with banks GPA0..GPB4 (bit-banged I2C pins): identity
// mapped in UEFI.
//
#define EXYNOS_GPIO_B_BASE  ((UINTN)0x14010000)

//
// Sleeps of a few to a few hundred milliseconds (charger input ramp).
//
#define EnvSleepMs(Ms)  gBS->Stall ((UINTN)(Ms) * 1000)

#endif // __EXYNOS_ENV_H__
