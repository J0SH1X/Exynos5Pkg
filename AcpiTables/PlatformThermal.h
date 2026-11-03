/** @file
  Switch for the ACPI thermal zones (DSDT \_TZ.TZ00 CPU, \_TZ.TZ01 GPU)
  that read the Exynos5420 TMU directly, and TmuDxe (FDF, which starts the
  sensors at boot and arms the 115 °C hardware trip).  Both come and go
  together: the zones read garbage without TmuDxe.  Usable from ASL.

  1: thermal variant (build-thermal.sh); 0: default images.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_PLATFORM_THERMAL_H__
#define __EXYNOS_PLATFORM_THERMAL_H__

#define EXYNOS_THERMAL  0

#endif
