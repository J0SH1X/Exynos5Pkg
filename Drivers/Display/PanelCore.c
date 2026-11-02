/** @file
  v1awifi panel sequencing (see ExynosDisplay.h): power domain, clock
  gates, pins, panel power, the DP / FIMD bring-up with retries, and the
  LP8556 backlight on PWM0.

  From the v1awifi kernel: board-v1-display.c (v1_lcd_on / off: LCD_EN
  GPG1(2), 200 ms after on, 500 ms off before on; v1_backlight_set_power:
  LED_BL_RST GPG1(1), 10 ms; HPD GPX0(7) function 3; DISP1BLK_CFG),
  lp855x_bl.c + board data (DEVICE_CTRL 0x80 = PWM only / fast config,
  EPROM 0x98 0xA1, 0xA1 0x5F, 0xA3 0x5E, 0xA5 0x04, 0xA8 0x00, 0xA9 0xA0,
  PWM0 at 10 kHz on GPB2(0) function 2).  Clock gates and the DISP power
  domain from mainline (clk-exynos5420.c, exynos5420.dtsi).  The clock
  muxes and dividers are left as the bootloader set them (266 MHz).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "ExynosDisplay.h"

#define PMU_DISP_CONFIG     0x40C0
#define PMU_DISP_STATUS     0x40C4
#define PD_ON               0x7

#define CMU_SRC_DISP10      0x22C
#define CMU_DIV_DISP10      0x52C
#define CMU_GATE_TOP_SCLK_DISP1  0x828
#define CMU_GATE_IP_DISP1   0x928
#define GATE_SCLK_FIMD1     (1u << 0)
#define GATE_SCLK_DP1       (1u << 20)
#define GATE_FIMD1          (1u << 0)
#define GATE_DP1            (1u << 4)

#define SYSREG_DISP1BLK_CFG 0x214
#define DISP1BLK_FIMD_BYPASS (1u << 15)         // FIMD straight to DP, not through MDNIE
#define DISP1BLK_MIE_DISP1  (1u << 14)
#define DISP1BLK_MIC_BYPASS (1u << 11)
#define DISP1BLK_MIE_LBLK1  (1u << 0)

#define GPG1                0x0A0              // in the LB controller
#define PIN_BL_RST          1
#define PIN_LCD_EN          2
#define GPX0                0xC00              // in the RT controller
#define PIN_HPD             7
#define GPB2                0x0A0              // in the B controller
#define PIN_PWM0            0
#define GPIO_CON            0x0
#define GPIO_DAT            0x4
#define GPIO_PUD            0x8

#define PWM_TCFG0           0x00
#define PWM_TCFG1           0x04
#define PWM_TCON            0x08
#define PWM_TCNTB0          0x0C
#define PWM_TCMPB0          0x10
#define TCON0_START         (1u << 0)
#define TCON0_MANUAL_UPDATE (1u << 1)
#define TCON0_INVERT        (1u << 2)
#define TCON0_AUTO_RELOAD   (1u << 3)
#define PWM_COUNTS_10KHZ    6666u               // PCLK (aclk66, ~66.6 MHz) / 10 kHz, prescaler / mux 1

#define LP855X_BRIGHTNESS   0x00
#define LP855X_DEVICE_CTRL  0x01

#define LCD_OFF_ON_MS       500
#define LCD_ON_MS           200
#define BL_ON_MS            10
#define MAX_TRIES           3

STATIC
VOID
PinFunc (
  IN UINTN   Bank,
  IN UINT32  Pin,
  IN UINT32  Func,
  IN UINT32  Pud
  )
{
  MmioAndThenOr32 (Bank + GPIO_PUD, ~(3u << (Pin * 2)), Pud << (Pin * 2));
  MmioAndThenOr32 (Bank + GPIO_CON, ~(0xFu << (Pin * 4)), Func << (Pin * 4));
}

STATIC
VOID
PinOut (
  IN UINTN    Bank,
  IN UINT32   Pin,
  IN BOOLEAN  High
  )
{
  if (High) {
    MmioOr32 (Bank + GPIO_DAT, 1u << Pin);
  } else {
    MmioAnd32 (Bank + GPIO_DAT, ~(1u << Pin));
  }
  PinFunc (Bank, Pin, 1, 0);
}

STATIC
INTN
BlockUp (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  UINTN   Wait;
  UINT32  Cfg;

  if ((MmioRead32 (Disp->Pmu + PMU_DISP_STATUS) & PD_ON) != PD_ON) {
    MmioWrite32 (Disp->Pmu + PMU_DISP_CONFIG, PD_ON);
    for (Wait = 0; ((MmioRead32 (Disp->Pmu + PMU_DISP_STATUS) & PD_ON) != PD_ON) && (Wait < 100); Wait++) {
      MicroSecondDelay (100);
    }
    if ((MmioRead32 (Disp->Pmu + PMU_DISP_STATUS) & PD_ON) != PD_ON) {
      DEBUG ((DEBUG_ERROR, "Display: DISP power domain does not come up\n"));
      return -1;
    }
  }
  MmioOr32 (Disp->CmuTop + CMU_GATE_TOP_SCLK_DISP1, GATE_SCLK_FIMD1 | GATE_SCLK_DP1);
  MmioOr32 (Disp->CmuTop + CMU_GATE_IP_DISP1, GATE_FIMD1 | GATE_DP1);

  Cfg  = MmioRead32 (Disp->Sysreg + SYSREG_DISP1BLK_CFG);
  Cfg &= ~(DISP1BLK_MIE_DISP1 | DISP1BLK_MIE_LBLK1);
  Cfg |= DISP1BLK_FIMD_BYPASS | DISP1BLK_MIC_BYPASS;
  MmioWrite32 (Disp->Sysreg + SYSREG_DISP1BLK_CFG, Cfg);

  PinFunc (Disp->GpioRt + GPX0, PIN_HPD, 3, 0);     // DP_HPD
  PinFunc (Disp->GpioB + GPB2, PIN_PWM0, 2, 0);     // TOUT0
  return 0;
}

/**
  PWM0 for Brightness (0..255).  Programs the timer at 10 kHz if the
  bootloader left it unconfigured.  As pwm-samsung, the output is high for
  TCNTB - TCMPB ticks; the backlight follows the low part.
**/
STATIC
VOID
PwmSet (
  IN EXYNOS_DISPLAY  *Disp,
  IN UINT8           Brightness
  )
{
  UINT32  Counts;
  UINT32  Duty;
  UINT32  Tcon;

  //
  // The bootloader leaves timer 0 stopped with TCNTB0 = 0xFFFFFFFF (the
  // pin then sits at one level): anything but a sane period gets 10 kHz.
  //
  Counts = MmioRead32 (Disp->Pwm + PWM_TCNTB0);
  if ((Counts < 100) || (Counts > 0xFFFF)) {
    MmioAnd32 (Disp->Pwm + PWM_TCFG0, ~0xFFu);       // prescaler 0: /1
    MmioAnd32 (Disp->Pwm + PWM_TCFG1, ~0xFu);        // mux 0: /1
    Counts = PWM_COUNTS_10KHZ - 1;
    MmioWrite32 (Disp->Pwm + PWM_TCNTB0, Counts);
  }
  //
  // The board inverts the PWM towards the LP8556 (measured: a high level
  // dims): the backlight follows the low time, TCMPB0 ticks.
  //
  Duty = (UINT32)(((UINT64)Counts * Brightness) / 255);
  if (Duty >= Counts) {
    Duty = Counts - 1;                  // TCMPB = TCNTB never matches: the output would stay high
  }
  MmioWrite32 (Disp->Pwm + PWM_TCMPB0, Duty);
  Tcon  = MmioRead32 (Disp->Pwm + PWM_TCON) & ~0xFu;
  MmioWrite32 (Disp->Pwm + PWM_TCON, Tcon | TCON0_MANUAL_UPDATE | TCON0_AUTO_RELOAD);
  MmioWrite32 (Disp->Pwm + PWM_TCON, Tcon | TCON0_AUTO_RELOAD | TCON0_START);   // 0: high all along = dark
}

STATIC
BOOLEAN
Lp8556Write (
  IN UINT8  Reg,
  IN UINT8  Value
  )
{
  UINT8  Buf[2];

  Buf[0] = Reg;
  Buf[1] = Value;
  return EnvI2cWrite (EXYNOS_DISP_BL_I2C_BUS, EXYNOS_DISP_BL_I2C_ADDR, Buf, 2);
}

STATIC
INTN
BacklightOn (
  IN EXYNOS_DISPLAY  *Disp,
  IN UINT8           Brightness
  )
{
  STATIC CONST UINT8  Eprom[][2] = {
    { 0x98, 0xA1 }, { 0xA1, 0x5F }, { 0xA3, 0x5E }, { 0xA5, 0x04 }, { 0xA8, 0x00 }, { 0xA9, 0xA0 }
  };
  UINTN  Index;

  PinOut (Disp->GpioLb + GPG1, PIN_BL_RST, TRUE);
  EnvSleepMs (BL_ON_MS);
  if (!Lp8556Write (LP855X_BRIGHTNESS, 0x80) || !Lp8556Write (LP855X_DEVICE_CTRL, 0x80)) {
    DEBUG ((DEBUG_ERROR, "Display: LP8556 does not answer on I2C%u\n", EXYNOS_DISP_BL_I2C_BUS));
    PwmSet (Disp, Brightness);            // the default EPROM still follows PWM
    return -1;
  }
  for (Index = 0; Index < ARRAY_SIZE (Eprom); Index++) {
    Lp8556Write (Eprom[Index][0], Eprom[Index][1]);
  }
  PwmSet (Disp, Brightness);
  return 0;
}

/**
  Backlight only (monitor off / on): the panel, the DP link and FIMD keep
  running, so turning it back on is immediate.
**/
VOID
ExynosDisplayBacklight (
  IN OUT EXYNOS_DISPLAY  *Disp,
  IN     UINT8           Brightness        // 0: off
  )
{
  if (Brightness == 0) {
    PwmSet (Disp, 0);
    PinOut (Disp->GpioLb + GPG1, PIN_BL_RST, FALSE);
  } else {
    BacklightOn (Disp, Brightness);
  }
}

VOID
ExynosDisplaySetBrightness (
  IN OUT EXYNOS_DISPLAY  *Disp,
  IN     UINT8           Brightness
  )
{
  PwmSet (Disp, Brightness);
}

STATIC
VOID
PanelOff (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  PwmSet (Disp, 0);
  PinOut (Disp->GpioLb + GPG1, PIN_BL_RST, FALSE);
  //
  // DP1's registers only while its PHY is enabled in the PMU: after a
  // previous PanelOff it is not, and touching DP1 then stalls the bus
  // (the whole SoC hangs; seen on the second monitor-on in Windows).
  //
  if ((MmioRead32 (Disp->Pmu + 0x728) & 1) != 0) {
    DpVideoOff (Disp);
  }
  FimdStop (Disp);
  DpPhyPower (Disp, FALSE);
  if (!Disp->KeepSupply) {
    PinOut (Disp->GpioLb + GPG1, PIN_LCD_EN, FALSE);
  }
}

VOID
ExynosDisplayLogState (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  DEBUG ((DEBUG_INFO, "Display: FIMD VIDCON0 0x%x WINCON0 0x%x VIDTCON0..2 0x%x 0x%x 0x%x ADD0 0x%x ADD2 0x%x\n",
          MmioRead32 (Disp->Fimd + 0x0), MmioRead32 (Disp->Fimd + 0x20), MmioRead32 (Disp->Fimd + 0x20010),
          MmioRead32 (Disp->Fimd + 0x20014), MmioRead32 (Disp->Fimd + 0x20018), MmioRead32 (Disp->Fimd + 0xA0),
          MmioRead32 (Disp->Fimd + 0x100)));
  DEBUG ((DEBUG_INFO, "Display: CMU SRC 0x%x DIV 0x%x GATE_TOP 0x%x GATE_IP 0x%x, PD 0x%x, DISP1BLK 0x%x\n",
          MmioRead32 (Disp->CmuTop + CMU_SRC_DISP10), MmioRead32 (Disp->CmuTop + CMU_DIV_DISP10),
          MmioRead32 (Disp->CmuTop + CMU_GATE_TOP_SCLK_DISP1), MmioRead32 (Disp->CmuTop + CMU_GATE_IP_DISP1),
          MmioRead32 (Disp->Pmu + PMU_DISP_STATUS), MmioRead32 (Disp->Sysreg + SYSREG_DISP1BLK_CFG)));
  if ((MmioRead32 (Disp->Pmu + 0x728) & 1) == 0) {
    DEBUG ((DEBUG_INFO, "Display: DP PHY off (registers not read)\n"));
    return;
  }
  DEBUG ((DEBUG_INFO, "Display: DP PHY 0x%x, DP SYS_CTL_3 0x%x LINK_BW 0x%x LANES 0x%x, GPG1 DAT 0x%x, PWM TCNTB0 %u TCMPB0 %u TCON 0x%x\n",
          MmioRead32 (Disp->Pmu + 0x728), MmioRead32 (Disp->Dp + 0x608), MmioRead32 (Disp->Dp + 0x680),
          MmioRead32 (Disp->Dp + 0x684), MmioRead32 (Disp->GpioLb + GPG1 + GPIO_DAT),
          MmioRead32 (Disp->Pwm + PWM_TCNTB0), MmioRead32 (Disp->Pwm + PWM_TCMPB0), MmioRead32 (Disp->Pwm + PWM_TCON)));
}

INTN
ExynosDisplayEnable (
  IN OUT EXYNOS_DISPLAY  *Disp,
  IN     UINT8           Brightness
  )
{
  INTN  Result;

  Disp->Step  = EXYNOS_DISP_STEP_POWER;
  Disp->Error = 0;
  Result = BlockUp (Disp);
  if (Result != 0) {
    Disp->Error = (INT32)Result;
    return Result;
  }

  for (Disp->Tries = 1; Disp->Tries <= MAX_TRIES; Disp->Tries++) {
    //
    // Panel power cycle: everything off, 500 ms, LCD_EN on, 200 ms.
    //
    Disp->Step = EXYNOS_DISP_STEP_POWER;
    PanelOff (Disp);
    if (!Disp->KeepSupply || (Disp->Tries > 1)) {
      // a retry always power-cycles the panel for real
      PinOut (Disp->GpioLb + GPG1, PIN_LCD_EN, FALSE);
      EnvSleepMs (LCD_OFF_ON_MS);
      PinOut (Disp->GpioLb + GPG1, PIN_LCD_EN, TRUE);
      EnvSleepMs (LCD_ON_MS);
    }

    Disp->Step = EXYNOS_DISP_STEP_DP_INIT;
    DpPhyPower (Disp, TRUE);
    Disp->Step = EXYNOS_DISP_STEP_TRAINING;
    Result = DpLinkUp (Disp);
    if (Result == 0) {
      DEBUG ((DEBUG_INFO, "Display: link up, DPCD rev 0x%02x, rate 0x%02x x %u lanes\n", Disp->DpcdRev,
              Disp->LinkRate, Disp->LaneCount));
      Disp->Step = EXYNOS_DISP_STEP_FIMD;
      Result = FimdStart (Disp);
    }
    if (Result == 0) {
      Disp->Step = EXYNOS_DISP_STEP_VIDEO;
      Result = DpVideoOn (Disp);
    }
    if (Result == 0) {
      break;
    }
    DEBUG ((DEBUG_ERROR, "Display: try %u failed at step %u (%d)\n", Disp->Tries, Disp->Step, (INT32)Result));
    ExynosDisplayLogState (Disp);
  }
  if (Result != 0) {
    Disp->Error = (INT32)Result;
    PanelOff (Disp);
    return Result;
  }

  Disp->Step = EXYNOS_DISP_STEP_BACKLIGHT;
  if (BacklightOn (Disp, Brightness) != 0) {
    Disp->Error = -1;                     // picture is up; only the LP8556 setup failed
  }
  Disp->Step = EXYNOS_DISP_STEP_DONE;
  return 0;
}

VOID
ExynosDisplayDisable (
  IN OUT EXYNOS_DISPLAY  *Disp
  )
{
  PanelOff (Disp);
}
