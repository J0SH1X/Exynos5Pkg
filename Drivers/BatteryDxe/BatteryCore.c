/** @file
  v1awifi battery: MAX17050 fuel gauge (I2C bus 18, 0x36) and the charger
  block of the MAX77888 interface PMIC (bus 17, 0x66; MUIC at 0x25).

  Shared by UEFI (BatteryDxe.c) and Windows (ExynosWinDrivers/exynosbatt),
  built unchanged against each one's <ExynosEnv.h>; I2C goes through
  EnvI2cRead / EnvI2cWrite.  Charging after sec_battery / max77803_charger
  / max77888-muic:
    - cable type from the MUIC (STATUS2 CHGTYP): USB -> 475 mA input /
      480 mA charge, CDP -> 1000/1000, dedicated/1A/500 mA/special charger
      (TA) -> 1800/2200 (charging_current_table[] of board-vienna-battery.c)
    - the input limit is ramped up like max77803_set_input_current(), and
      backed off if VBUS (CHG_DTLS_00 CHGIN_DTLS) drops from valid to UVLO
      on the way; a supply that already sits at UVLO (a sagging USB port or
      adapter) is left to the soft-regulation handling below
    - soft regulation (CHG_DTLS_02 bit 3): the input limit is lowered in
      100 mA steps to >= 400 mA, raised again after a quiet minute
    - charging stops above 51.0 degC / below -5.0 degC (gauge thermistor)
      and resumes below 46.0 / above 0.0 degC
    - the charger's restart threshold is disabled (CHG_CNFG_01, as the
      kernel sets it), so after "done" charging is restarted in software
      once RepSOC falls below 98 %
  The caller runs BatteryCorePoll() every 5 s.  The charger keeps its
  settings across the hand-over from UEFI to the OS.

  Register map and units from the v1awifi kernel:
    - max17050_fuelgauge.c: Rsense 10 mOhm -> current 156.25 uA/LSB,
      capacity 0.5 mAh/LSB; VCELL 78.125 uV/LSB; RepSOC and temperature
      1/256 % and 1/256 degC.  16-bit registers, low byte first.
    - max77803_charger.c (MAX77888 variant, "max77888_charger"): CHG_CNFG_09
      input limit 25 mA/step, CHG_CNFG_02[5:0] charge current 40 mA/step,
      CHG_CNFG_04[4:0] CV = 3725 + 25 mV/step.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <ExynosEnv.h>

#include "BatteryCore.h"

#define FG_BUS              18
#define FG_ADDR             0x36
#define FG_STATUS           0x00
#define FG_REMCAP_REP       0x05
#define FG_SOC_REP          0x06
#define FG_TEMP             0x08
#define FG_VCELL            0x09
#define FG_CURRENT          0x0A
#define FG_AVG_CURRENT      0x0B
#define FG_FULLCAP          0x10
#define FG_TTE              0x11
#define FG_CYCLES           0x17
#define FG_DESIGNCAP        0x18
#define FG_AVG_VCELL        0x19
#define FG_CONFIG           0x1D
#define FG_VERSION          0x21
#define FG_SOC_VF           0xFF
#define FG_STATUS_POR       (1u << 1)

#define IFPMIC_BUS          17
#define CHG_ADDR            0x66
#define CHG_PMIC_ID1        0x20
#define CHG_PMIC_ID2        0x21
#define CHG_INT_OK          0xB2
#define CHG_DTLS_00         0xB3
#define CHG_DTLS_01         0xB4
#define CHG_CNFG_00         0xB7
#define CHG_CNFG_02         0xB9
#define CHG_CNFG_06         0xBD
#define CHG_CNFG_09         0xC0
#define CHG_DTLS_02         0xB5
#define BYP_DTLS_SOFTREG    0x08     // input voltage regulation: adapter/cable sagging
#define SOFTREG_STEP_MA     100      // max77803_charger.c REDUCE_CURRENT_STEP
#define SOFTREG_MIN_MA      400      // MINIMUM_INPUT_CURRENT
#define SOFTREG_RECOVER_POLLS 12     // 60 s without regulation: step back up

#define CHGIN_DTLS(d)       (((d) >> 5) & 0x3)     // 0, 1: UVLO; 3: valid
#define CHG_DTLS(d)         ((d) & 0xF)
#define CHG_DTLS_DONE       0x4
#define MODE_CHGR           0x01
#define MODE_BUCK           0x04
#define CHG_CC_MASK         0x3F
#define CHGIN_LIM_MASK      0x7F
#define CHGPROT_UNLOCK      (0x3 << 2)
#define INPUT_STEP_MA       25
#define CHARGE_STEP_MA      40
#define MIN_INPUT_MA        300
#define SOFT_START_MA       100

#define MUIC_ADDR           0x25
#define MUIC_ID             0x00
#define MUIC_STATUS1        0x04
#define MUIC_STATUS2        0x05
#define MUIC_CHGTYP(s)      ((s) & 0x7)
#define MUIC_VBVOLT         (1u << 6)

#define TEMP_STOP_HIGH      510      // 0.1 degC
#define TEMP_RESUME_HIGH    460
#define TEMP_STOP_LOW       (-50)
#define TEMP_RESUME_LOW     0
#define RECHARGE_SOC        98

typedef enum {
  CableNone,
  CableUsb,
  CableCdp,
  CableTa
} CABLE;

STATIC CONST struct {
  CONST CHAR8  *Name;
  UINT32       InputMa;
  UINT32       ChargeMa;
} mCable[] = {
  [CableNone] = { "none", 0,    0    },
  [CableUsb]  = { "USB",  475,  480  },
  [CableCdp]  = { "CDP",  1000, 1000 },
  [CableTa]   = { "TA",   1800, 2200 },
};

STATIC CABLE    mCurrentCable = (CABLE)-1;
STATIC BOOLEAN  mTempHold;
STATIC UINT8    mInputReg;        // CHG_CNFG_09 as we left it
STATIC UINTN    mInputRetries;    // re-applies after the charger changed it
#define MAX_INPUT_RETRIES  3
STATIC UINTN    mSoftRegQuiet;    // polls since the charger last regulated its input


STATIC
BOOLEAN
FgRead (
  IN  UINT8   Reg,
  OUT UINT16  *Value
  )
{
  UINT8  Buf[2];

  if (!EnvI2cRead (FG_BUS, FG_ADDR, Reg, Buf, 2)) {
    return FALSE;
  }
  *Value = (UINT16)(Buf[0] | (Buf[1] << 8));
  return TRUE;
}

STATIC
BOOLEAN
LogGauge (
  VOID
  )
{
  UINT16  Status, Version, Config, Soc, VfSoc, Vcell, AvgVcell, Cur, AvgCur;
  UINT16  Temp, RemCap, FullCap, DesignCap, Tte, Cycles;

  if (!FgRead (FG_VERSION, &Version)) {
    DEBUG ((DEBUG_ERROR, "Battery: MAX17050 not answering on I2C%u\n", FG_BUS));
    return FALSE;
  }
  FgRead (FG_STATUS, &Status);
  FgRead (FG_CONFIG, &Config);
  FgRead (FG_SOC_REP, &Soc);
  FgRead (FG_SOC_VF, &VfSoc);
  FgRead (FG_VCELL, &Vcell);
  FgRead (FG_AVG_VCELL, &AvgVcell);
  FgRead (FG_CURRENT, &Cur);
  FgRead (FG_AVG_CURRENT, &AvgCur);
  FgRead (FG_TEMP, &Temp);
  FgRead (FG_REMCAP_REP, &RemCap);
  FgRead (FG_FULLCAP, &FullCap);
  FgRead (FG_DESIGNCAP, &DesignCap);
  FgRead (FG_TTE, &Tte);
  FgRead (FG_CYCLES, &Cycles);

  DEBUG ((DEBUG_INFO, "Battery: MAX17050 version 0x%04x status 0x%04x%a config 0x%04x\n",
          Version, Status, (Status & FG_STATUS_POR) ? " (POR)" : "", Config));
  DEBUG ((DEBUG_INFO, "Battery: SOC %u.%02u%% (VF %u%%), %u mV (avg %u), %d mA (avg %d), %d.%u C\n",
          Soc >> 8, ((Soc & 0xFF) * 100) >> 8, VfSoc >> 8,
          (UINT32)Vcell * 625 / 8000, (UINT32)AvgVcell * 625 / 8000,
          (INT32)(INT16)Cur * 15625 / 100000, (INT32)(INT16)AvgCur * 15625 / 100000,
          (INT32)(INT16)Temp / 256, (UINT32)((Temp & 0xFF) * 10) >> 8));
  DEBUG ((DEBUG_INFO, "Battery: %u / %u mAh (design %u), time to empty %u min, %u.%02u cycles\n",
          RemCap / 2, FullCap / 2, DesignCap / 2, (UINT32)Tte * 5625 / 60000,
          Cycles / 100, Cycles % 100));
  return TRUE;
}

STATIC
VOID
LogCharger (
  VOID
  )
{
  UINT8  Id[2];
  UINT8  Chg[0xC6 - CHG_INT_OK + 1];
  UINT8  Muic[0x16];
  UINTN  Index;

  if (!EnvI2cRead (IFPMIC_BUS, CHG_ADDR, CHG_PMIC_ID1, Id, 2)) {
    DEBUG ((DEBUG_ERROR, "Battery: MAX77888 not answering on I2C%u\n", IFPMIC_BUS));
    return;
  }
  DEBUG ((DEBUG_INFO, "Battery: MAX77888 PMIC_ID 0x%02x 0x%02x\n", Id[0], Id[1]));

  if (EnvI2cRead (IFPMIC_BUS, CHG_ADDR, CHG_INT_OK, Chg, sizeof (Chg))) {
    DEBUG ((DEBUG_INFO, "Battery: CHG INT_OK 0x%02x DTLS 0x%02x 0x%02x 0x%02x 0x%02x\n",
            Chg[0], Chg[1], Chg[2], Chg[3], Chg[4]));
    for (Index = CHG_CNFG_00 - CHG_INT_OK; Index < sizeof (Chg); Index++) {
      DEBUG ((DEBUG_INFO, "Battery: CHG reg 0x%02x = 0x%02x\n", CHG_INT_OK + Index, Chg[Index]));
    }
    DEBUG ((DEBUG_INFO, "Battery: input limit %u mA, charge current %u mA, CV %u mV, mode 0x%x\n",
            (Chg[0xC0 - CHG_INT_OK] & 0x7F) * 25, (Chg[0xB9 - CHG_INT_OK] & 0x3F) * 40,
            3725 + (Chg[0xBB - CHG_INT_OK] & 0x1F) * 25, Chg[0xB7 - CHG_INT_OK] & 0xF));
  }

  if (EnvI2cRead (IFPMIC_BUS, MUIC_ADDR, MUIC_ID, Muic, sizeof (Muic))) {
    DEBUG ((DEBUG_INFO, "Battery: MUIC ID 0x%02x STATUS 0x%02x 0x%02x 0x%02x CTRL1..3 0x%02x 0x%02x 0x%02x\n",
            Muic[MUIC_ID], Muic[MUIC_STATUS1], Muic[MUIC_STATUS1 + 1], Muic[MUIC_STATUS1 + 2],
            Muic[0x0C], Muic[0x0D], Muic[0x0E]));
  } else {
    DEBUG ((DEBUG_ERROR, "Battery: MUIC not answering at 0x%02x\n", MUIC_ADDR));
  }
}

STATIC
BOOLEAN
ChgRead (
  IN  UINT8  Reg,
  OUT UINT8  *Value
  )
{
  return EnvI2cRead (IFPMIC_BUS, CHG_ADDR, Reg, Value, 1);
}

STATIC
BOOLEAN
ChgWrite (
  IN UINT8  Reg,
  IN UINT8  Value
  )
{
  UINT8  Buf[2];

  Buf[0] = Reg;
  Buf[1] = Value;
  return EnvI2cWrite (IFPMIC_BUS, CHG_ADDR, Buf, 2);
}

STATIC
VOID
ChgUpdate (
  IN UINT8  Reg,
  IN UINT8  Mask,
  IN UINT8  Value
  )
{
  UINT8  Old;

  if (ChgRead (Reg, &Old)) {
    ChgWrite (Reg, (UINT8)((Old & ~Mask) | (Value & Mask)));
  }
}

STATIC
UINT8
VbusState (
  VOID
  )
{
  UINT8  Dtls;

  return ChgRead (CHG_DTLS_00, &Dtls) ? (UINT8)CHGIN_DTLS (Dtls) : 0;
}

/**
  max77803_set_input_current(): ramp CHG_CNFG_09 up to the target, backing
  off when VBUS falls into UVLO (a weak adapter or cable).
**/
STATIC
VOID
SetInputCurrent (
  IN UINT32  TargetMa
  )
{
  UINT8    Now;
  UINT8    Target;
  UINT8    Step;
  BOOLEAN  VbusWasValid;

  Target = (UINT8)(TargetMa / INPUT_STEP_MA);
  if (!ChgRead (CHG_CNFG_09, &Now)) {
    return;
  }
  Now &= CHGIN_LIM_MASK;

  if (Now >= Target) {
    ChgWrite (CHG_CNFG_09, Target);
    return;
  }
  if (Now == 0) {
    Now = SOFT_START_MA / INPUT_STEP_MA;
    ChgWrite (CHG_CNFG_09, Now);
    EnvSleepMs (100);
  }

  //
  // Only treat UVLO as "too much" if VBUS was fine before we started:
  // some USB hosts sit at UVLO even at the default limit.
  //
  VbusWasValid = (VbusState () == 0x3);
  Step = (TargetMa <= 1000) ? 1 : (SOFT_START_MA / INPUT_STEP_MA);
  while (Now < Target) {
    Now = (UINT8)MIN ((UINTN)Now + Step, (UINTN)Target);
    ChgWrite (CHG_CNFG_09, Now);
    EnvSleepMs ((TargetMa <= 1000) ? 50 : 20);
    if (VbusWasValid && (VbusState () <= 0x1)) {
      Now = (UINT8)MAX ((INTN)Now - 3 * Step, (INTN)(MIN_INPUT_MA / INPUT_STEP_MA));
      ChgWrite (CHG_CNFG_09, Now);
      DEBUG ((DEBUG_INFO, "Battery: VBUS UVLO while ramping, input limit back to %u mA\n",
              Now * INPUT_STEP_MA));
      break;
    }
  }
}

STATIC
VOID
SetCharging (
  IN BOOLEAN  Enable
  )
{
  ChgUpdate (CHG_CNFG_00, MODE_CHGR | MODE_BUCK, Enable ? (MODE_CHGR | MODE_BUCK) : MODE_BUCK);
}

STATIC
CABLE
DetectCable (
  VOID
  )
{
  UINT8  Status2;

  if (!EnvI2cRead (IFPMIC_BUS, MUIC_ADDR, MUIC_STATUS2, &Status2, 1)) {
    return CableNone;
  }
  if ((Status2 & MUIC_VBVOLT) == 0) {
    return CableNone;
  }
  switch (MUIC_CHGTYP (Status2)) {
    case 1:
      return CableUsb;
    case 2:
      return CableCdp;
    case 3:
    case 4:
    case 5:
    case 6:
      return CableTa;
    default:
      return CableNone;
  }
}

STATIC
VOID
ApplyCable (
  IN CABLE  Cable
  )
{
  if (Cable == CableNone) {
    DEBUG ((DEBUG_INFO, "Battery: cable removed\n"));
    return;
  }

  ChgWrite (CHG_CNFG_06, CHGPROT_UNLOCK);
  ChgUpdate (CHG_CNFG_02, CHG_CC_MASK, (UINT8)(mCable[Cable].ChargeMa / CHARGE_STEP_MA));
  SetInputCurrent (mCable[Cable].InputMa);
  SetCharging (!mTempHold);

  {
    UINT8  In, Cc;

    ChgRead (CHG_CNFG_09, &In);
    ChgRead (CHG_CNFG_02, &Cc);
    mInputReg = In & CHGIN_LIM_MASK;
    DEBUG ((DEBUG_INFO, "Battery: %a cable: input limit %u mA, charge current %u mA, VBUS %u%a\n",
            mCable[Cable].Name, (In & CHGIN_LIM_MASK) * INPUT_STEP_MA, (Cc & CHG_CC_MASK) * CHARGE_STEP_MA,
            VbusState (), mTempHold ? " (held: temperature)" : ""));
  }
}

/**
  Every 5 s: cable changes, temperature window, recharge after "done".
**/
VOID
BatteryCorePoll (
  VOID
  )
{
  CABLE   Cable;
  UINT16  Temp;
  UINT16  Soc;
  INT32   Deci;
  UINT8   Dtls;

  Cable = DetectCable ();
  if (Cable != mCurrentCable) {
    mCurrentCable = Cable;
    mInputRetries = 0;
    mSoftRegQuiet = 0;
    ApplyCable (Cable);
  }
  if (Cable == CableNone) {
    return;
  }

  //
  // The charger may lower the input limit itself after we set it (its own
  // detection / input current regulation on a weak adapter).  Say so, and
  // re-apply a few times before accepting its value.
  //
  {
    UINT8  In;

    if (ChgRead (CHG_CNFG_09, &In) && ((In & CHGIN_LIM_MASK) != mInputReg)) {
      DEBUG ((DEBUG_INFO, "Battery: charger changed the input limit %u -> %u mA (VBUS %u)%a\n",
              mInputReg * INPUT_STEP_MA, (In & CHGIN_LIM_MASK) * INPUT_STEP_MA, VbusState (),
              (mInputRetries < MAX_INPUT_RETRIES) ? ", re-applying" : ", keeping it"));
      if (mInputRetries < MAX_INPUT_RETRIES) {
        mInputRetries++;
        SetInputCurrent (mCable[Cable].InputMa);
        ChgRead (CHG_CNFG_09, &In);
      }
      mInputReg = In & CHGIN_LIM_MASK;
    }
  }

  //
  // While on anything but a PC port, log the raw state every poll: the
  // micro-USB port (and so the live USB log) is taken by the charger, but
  // the log is kept and read back once the PC is plugged in again.
  //
  if (Cable != CableUsb) {
    UINT8   Chg[5];
    UINT8   Muic[3];
    UINT8   In;
    UINT16  Cur;
    UINT16  Vcell;

    if ((EnvI2cRead (IFPMIC_BUS, CHG_ADDR, CHG_INT_OK, Chg, sizeof (Chg))) &&
        (EnvI2cRead (IFPMIC_BUS, MUIC_ADDR, MUIC_STATUS1, Muic, sizeof (Muic))) &&
        ChgRead (CHG_CNFG_09, &In) && FgRead (FG_CURRENT, &Cur) && FgRead (FG_VCELL, &Vcell)) {
      DEBUG ((DEBUG_INFO, "Battery: [%a] %d mA %u mV, in %u mA, INT_OK 0x%02x DTLS 0x%02x 0x%02x 0x%02x 0x%02x, "
              "MUIC 0x%02x 0x%02x 0x%02x\n",
              mCable[Cable].Name, (INT32)(INT16)Cur * 15625 / 100000, (UINT32)Vcell * 625 / 8000,
              (In & CHGIN_LIM_MASK) * INPUT_STEP_MA, Chg[0], Chg[1], Chg[2], Chg[3], Chg[4],
              Muic[0], Muic[1], Muic[2]));
    }
  }

  //
  // Soft regulation (max77803_bypass_irq -> reduce_input_current()): the
  // charger holds VBUS up by drawing less than the limit, i.e. the adapter
  // or cable cannot deliver it.  Lower the limit 100 mA at a time (not below
  // 400 mA) so the adapter is not run in collapse, and step back up towards
  // the cable's limit after a minute without regulation.
  //
  if (Cable != CableUsb) {
    UINT8  Dtls2;
    UINT8  In;

    if (ChgRead (CHG_DTLS_02, &Dtls2) && ChgRead (CHG_CNFG_09, &In)) {
      In &= CHGIN_LIM_MASK;
      if (((Dtls2 & BYP_DTLS_SOFTREG) != 0) && (VbusState () < 0x3)) {
        mSoftRegQuiet = 0;
        if (In > (SOFTREG_MIN_MA + SOFTREG_STEP_MA) / INPUT_STEP_MA) {
          In -= SOFTREG_STEP_MA / INPUT_STEP_MA;
          ChgWrite (CHG_CNFG_09, In);
          mInputReg = In;
          DEBUG ((DEBUG_INFO, "Battery: adapter sagging (soft regulation), input limit down to %u mA\n",
                  In * INPUT_STEP_MA));
        }
      } else if ((++mSoftRegQuiet >= SOFTREG_RECOVER_POLLS) &&
                 (In < mCable[Cable].InputMa / INPUT_STEP_MA)) {
        mSoftRegQuiet = 0;
        In = (UINT8)MIN ((UINTN)In + SOFTREG_STEP_MA / INPUT_STEP_MA, mCable[Cable].InputMa / INPUT_STEP_MA);
        ChgWrite (CHG_CNFG_09, In);
        mInputReg = In;
        DEBUG ((DEBUG_INFO, "Battery: no soft regulation for a minute, input limit up to %u mA\n",
                In * INPUT_STEP_MA));
      }
    }
  }

  if (FgRead (FG_TEMP, &Temp)) {
    Deci = (INT32)(INT16)Temp * 10 / 256;
    if (!mTempHold && ((Deci >= TEMP_STOP_HIGH) || (Deci <= TEMP_STOP_LOW))) {
      mTempHold = TRUE;
      SetCharging (FALSE);
      DEBUG ((DEBUG_INFO, "Battery: %d.%d C, charging stopped\n", Deci / 10, ABS (Deci % 10)));
    } else if (mTempHold && (Deci < TEMP_RESUME_HIGH) && (Deci > TEMP_RESUME_LOW)) {
      mTempHold = FALSE;
      SetCharging (TRUE);
      DEBUG ((DEBUG_INFO, "Battery: %d.%d C, charging resumed\n", Deci / 10, ABS (Deci % 10)));
    }
  }

  if (!mTempHold && ChgRead (CHG_DTLS_01, &Dtls) && (CHG_DTLS (Dtls) == CHG_DTLS_DONE) &&
      FgRead (FG_SOC_REP, &Soc) && ((Soc >> 8) < RECHARGE_SOC)) {
    DEBUG ((DEBUG_INFO, "Battery: done but at %u%%, recharging\n", Soc >> 8));
    SetCharging (FALSE);
    EnvSleepMs (10);
    SetCharging (TRUE);
  }
}

BOOLEAN
BatteryCoreInit (
  VOID
  )
{
  BOOLEAN  Present;

  Present = LogGauge ();
  LogCharger ();
  return Present;
}

BOOLEAN
BatteryCoreGetStatus (
  OUT EXYNOS_BATTERY_STATUS  *Status
  )
{
  UINT16  Soc, Vcell, Cur, AvgCur, Temp, Rem, Full, Design, Cycles, Tte;
  UINT8   In, Cc, Dtls0, Dtls1;

  if (!FgRead (FG_SOC_REP, &Soc) || !FgRead (FG_VCELL, &Vcell) || !FgRead (FG_CURRENT, &Cur) ||
      !FgRead (FG_AVG_CURRENT, &AvgCur) || !FgRead (FG_TEMP, &Temp) ||
      !FgRead (FG_REMCAP_REP, &Rem) || !FgRead (FG_FULLCAP, &Full) || !FgRead (FG_DESIGNCAP, &Design) ||
      !FgRead (FG_CYCLES, &Cycles) || !FgRead (FG_TTE, &Tte) ||
      !ChgRead (CHG_CNFG_09, &In) || !ChgRead (CHG_CNFG_02, &Cc) ||
      !ChgRead (CHG_DTLS_00, &Dtls0) || !ChgRead (CHG_DTLS_01, &Dtls1)) {
    return FALSE;
  }

  Status->SocCentiPercent = MIN (((UINT32)Soc * 100) >> 8, 10000u);
  Status->MilliVolts      = (UINT32)Vcell * 625 / 8000;
  Status->MilliAmps       = (INT32)(INT16)Cur * 15625 / 100000;
  Status->AvgMilliAmps    = (INT32)(INT16)AvgCur * 15625 / 100000;
  Status->DeciCelsius     = (INT32)(INT16)Temp * 10 / 256;
  Status->RemainingMah    = Rem / 2;
  Status->FullMah         = Full / 2;
  Status->DesignMah       = Design / 2;
  Status->CycleCentis     = Cycles;
  Status->TimeToEmptyMin  = (UINT32)Tte * 5625 / 60000;
  Status->Cable           = ((UINTN)mCurrentCable < ARRAY_SIZE (mCable)) ? mCable[mCurrentCable].Name : "?";
  Status->ExternalPower   = ((UINTN)mCurrentCable < ARRAY_SIZE (mCable)) && (mCurrentCable != CableNone);
  Status->InputLimitMa    = (In & CHGIN_LIM_MASK) * INPUT_STEP_MA;
  Status->ChargeCurrentMa = (Cc & CHG_CC_MASK) * CHARGE_STEP_MA;
  Status->ChargerDetails  = (UINT8)CHG_DTLS (Dtls1);
  Status->VbusDetails     = (UINT8)CHGIN_DTLS (Dtls0);
  Status->TemperatureHold = mTempHold;
  return TRUE;
}
