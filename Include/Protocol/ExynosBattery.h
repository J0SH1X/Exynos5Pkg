/** @file
  v1awifi battery and charger state (BatteryDxe).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_BATTERY_PROTOCOL_H__
#define __EXYNOS_BATTERY_PROTOCOL_H__

#define EXYNOS_BATTERY_PROTOCOL_GUID                                           \
  {                                                                            \
    0x9d3f4a71, 0x5c20, 0x4b8e,                                                \
    {                                                                          \
      0xa1, 0x77, 0x6e, 0x24, 0x41, 0x10, 0x44, 0x04                           \
    }                                                                          \
  }

//
// EXYNOS_BATTERY_STATUS lives with the shared battery code.
//
#include "../../Drivers/BatteryDxe/BatteryCore.h"

typedef struct _EXYNOS_BATTERY_PROTOCOL EXYNOS_BATTERY_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EXYNOS_BATTERY_GET_STATUS)(
  IN  EXYNOS_BATTERY_PROTOCOL  *This,
  OUT EXYNOS_BATTERY_STATUS    *Status
  );

struct _EXYNOS_BATTERY_PROTOCOL {
  EXYNOS_BATTERY_GET_STATUS  GetStatus;
};

extern EFI_GUID gExynosBatteryProtocolGuid;

#endif
