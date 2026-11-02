/** @file
  v1awifi battery and charger logic shared by UEFI (BatteryDxe) and Windows
  (ExynosWinDrivers/exynosbatt): MAX17050 fuel gauge on bit-banged I2C bus
  18, MAX77888 charger + MUIC on bus 17.

  Only base types here; the environment (<ExynosEnv.h>) is included by
  BatteryCore.c.  The caller serialises all BatteryCore* calls (one I2C
  transfer at a time on the bit-banged buses).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __BATTERY_CORE_H__
#define __BATTERY_CORE_H__

typedef struct {
  UINT32   SocCentiPercent;   // RepSOC, 0.01 %
  UINT32   MilliVolts;        // VCELL
  INT32    MilliAmps;         // Current, + = into the battery
  INT32    AvgMilliAmps;      // AvgCurrent
  INT32    DeciCelsius;       // gauge thermistor
  UINT32   RemainingMah;      // RemCapREP
  UINT32   FullMah;           // FullCAP
  UINT32   DesignMah;         // DesignCap
  UINT32   CycleCentis;       // Cycles, 0.01 cycle
  UINT32   TimeToEmptyMin;    // TTE (gauge's estimate)
  CONST CHAR8  *Cable;        // "none", "USB", "CDP", "TA"
  BOOLEAN  ExternalPower;     // a cable with VBUS is attached
  UINT32   InputLimitMa;      // CHG_CNFG_09
  UINT32   ChargeCurrentMa;   // CHG_CNFG_02
  UINT8    ChargerDetails;    // CHG_DTLS_01[3:0]: 0 pre, 1 CC, 2 CV, 3 top-off, 4 done, 8 off
  UINT8    VbusDetails;       // CHG_DTLS_00 CHGIN_DTLS: 0/1 UVLO, 2 OVLO, 3 valid
  BOOLEAN  TemperatureHold;   // charging stopped for temperature
} EXYNOS_BATTERY_STATUS;

//
// Environment: I2C on the bit-banged buses (17, 18).
//
BOOLEAN
EnvI2cRead (
  IN  UINT32  Bus,
  IN  UINT8   Addr,
  IN  UINT8   Reg,
  OUT UINT8   *Buf,
  IN  UINTN   Count
  );

BOOLEAN
EnvI2cWrite (
  IN UINT32  Bus,
  IN UINT8   Addr,
  IN UINT8   *Buf,
  IN UINTN   Count
  );

/**
  Log the gauge and charger state.  FALSE if the gauge does not answer.
**/
BOOLEAN
BatteryCoreInit (
  VOID
  );

/**
  Every few seconds: cable changes (input / charge current), the charger's
  own input limit changes, soft regulation, temperature hold, recharge.
**/
VOID
BatteryCorePoll (
  VOID
  );

BOOLEAN
BatteryCoreGetStatus (
  OUT EXYNOS_BATTERY_STATUS  *Status
  );

#endif // __BATTERY_CORE_H__
