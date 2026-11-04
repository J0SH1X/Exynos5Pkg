/** @file
  FIMC-IS host protocol on top of FimcIsCore (fimc-is-interface.c /
  fimc-is-device-ischain.c): commands with their ISR_DONE replies, the
  enum answer, HIC_OPEN_SENSOR with sensor_open_extended, the setfile, the
  firmware's text log.  Replies are polled (FimcIsWaitMessage), so the
  caller must not take general messages elsewhere meanwhile (Windows:
  keep the combiner input off during these calls).  Logs through DEBUG.
  Shared by UEFI (CameraTestApp) and Windows (exynoscam).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __FIMC_IS_HOST_H__
#define __FIMC_IS_HOST_H__

#include "FimcIsCore.h"

#define FIMC_IS_FW_LOG_OFFSET  0x01340000u   // FIMC_IS_DEBUG_REGION_ADDR
#define FIMC_IS_FW_LOG_SIZE    0x0007D000u   // DEBUG_CNT; write offset (absolute) at LOG + SIZE

//
// New firmware log text since the last call, as "fw| ..." lines (at most
// ~12 KB per call).
//
VOID
FimcIsLogDump (
  IN OUT FIMC_IS  *Is
  );

//
// One command, then every general message until its reply.  0 = ISR_DONE,
// < 0: -1 A5 busy, -2 no reply in 5 s, -3 ISR_NDONE.  Reply optional.
//
INTN
FimcIsCommand (
  IN OUT FIMC_IS      *Is,
  IN     CONST CHAR8  *Name,
  IN     UINT32       Cmd,
  IN     UINT32       Group,
  IN     UINT32       P1,
  IN     UINT32       P2,
  IN     UINT32       P3,
  IN     UINT32       P4,
  OUT    FIMC_IS_MSG  *Reply OPTIONAL
  );

//
// After IHC_GET_SENSOR_NUMBER: the enum answer, then HIC_OPEN_SENSOR for
// the rear IMX134 module on Group (FIMC_IS_GROUP_3A0 / 3A1).  0 when the
// firmware accepted it (ISR_DONE and the shared[] magic).
//
INTN
FimcIsOpenRearSensor (
  IN OUT FIMC_IS  *Is,
  IN     UINT32   Group
  );

//
// Front S5K6B2 on ISP I2C channel 2 (fimc-is-device-sensor.c: ext all zero but
// I2CChannel / I2CSclk).  Same return as FimcIsOpenRearSensor.
//
INTN
FimcIsOpenFrontSensor (
  IN OUT FIMC_IS  *Is,
  IN     UINT32   Group
  );

//
// HIC_GET_SET_FILE_ADDR, copy, HIC_LOAD_SET_FILE, then stream off and
// process stop (fimc_is_ischain_init tail).  0 or < 0.
//
INTN
FimcIsLoadSetfile (
  IN OUT FIMC_IS     *Is,
  IN     UINT32      Group,
  IN     CONST VOID  *Setfile,
  IN     UINTN       Size
  );

#endif // __FIMC_IS_HOST_H__
