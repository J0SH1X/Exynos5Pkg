/** @file
  v1awifi internal display: Samsung LSL122DL01 2560x1600 eDP panel behind
  FIMD1 -> DP1 (DP TX + video PHY), LP8556 backlight on PWM0.  Shared by
  UEFI (Application/DisplayTestApp) and Windows (ExynosWinDrivers), built
  unchanged in both through <ExynosEnv.h>; the caller maps the register
  blocks and fills EXYNOS_DISPLAY.

  Cold bring-up, as the v1awifi kernel does it (board-v1-display.c,
  s5p-dp-core.c s5p_dp_enable(), lp855x_bl.c), with mainline's FIMD
  programming for the 5420 (exynos_drm_fimd.c):
    1. backlight off, video off, panel power off (LCD_EN low), 500 ms
    2. panel power on (LCD_EN GPG1(2) high), 200 ms
    3. DP PHY on, DP TX reset / analog / HPD / AUX init
    4. scrambling and enhanced framing off, link training (2.7 Gbps x4)
    5. FIMD1 timing, window 0 on the framebuffer, video on
    6. DP video in slave mode (timing from FIMD), wait for the stream
    7. backlight: LED_BL_RST GPG1(1) high, LP8556 registers, PWM0 duty
  A failure in 3..6 power-cycles the panel and retries (3 times), like the
  kernel's s5p_dp_enable().

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_DISPLAY_H__
#define __EXYNOS_DISPLAY_H__

#include <ExynosEnv.h>

//
// Physical addresses of the blocks the caller maps (sizes in comments).
//
#define EXYNOS_DISP_FIMD1_PHYS    0x14400000   // 0x30000 (timing block at +0x20000)
#define EXYNOS_DISP_DP1_PHYS      0x145B0000   // 0x1000
#define EXYNOS_DISP_CMU_TOP_PHYS  0x10020000   // 0x1000: SRC/DIV/GATE DISP10/DISP1 at +0x22C/+0x52C/+0x828/+0x928
#define EXYNOS_DISP_PMU_PHYS      0x10040000   // 0x5000: DPTX_PHY_CONTROL +0x728, DISP domain +0x40C0
#define EXYNOS_DISP_SYSREG_PHYS   0x10050000   // 0x1000: DISP1BLK_CFG +0x214
#define EXYNOS_DISP_GPIO_LB_PHYS  0x14000000   // 0x1000: GPG1 +0xA0
#define EXYNOS_DISP_GPIO_RT_PHYS  0x13400000   // 0x1000: GPX0 +0xC00
#define EXYNOS_DISP_GPIO_B_PHYS   0x14010000   // 0x1000: GPB2 +0xA0
#define EXYNOS_DISP_PWM_PHYS      0x12DD0000   // 0x100

#define EXYNOS_DISP_WIDTH         2560
#define EXYNOS_DISP_HEIGHT        1600

//
// LP8556 on the bit-banged I2C bus 19 (GPA2(4)/(5), the kernel's bus 24);
// the host provides the transfers (EnvI2cWrite, as for BatteryCore).
//
#define EXYNOS_DISP_BL_I2C_BUS    19
#define EXYNOS_DISP_BL_I2C_ADDR   0x2C

BOOLEAN
EnvI2cWrite (
  IN UINT32  Bus,
  IN UINT8   Addr,
  IN UINT8   *Buf,
  IN UINTN   Count
  );

typedef struct {
  //
  // Register blocks (virtual addresses), filled by the caller.
  //
  UINTN    Fimd;
  UINTN    Dp;
  UINTN    CmuTop;
  UINTN    Pmu;
  UINTN    Sysreg;
  UINTN    GpioLb;
  UINTN    GpioRt;
  UINTN    GpioB;
  UINTN    Pwm;
  //
  // Scanout: physical address of a 2560x1600 XRGB8888 buffer, stride in
  // bytes (>= 2560 * 4).
  //
  UINT32   FbPhys;
  UINT32   Stride;
  //
  // TRUE: ExynosDisplayDisable / the power cycle in ExynosDisplayEnable
  // leave LCD_EN (the panel supply) on and skip the 500 ms off time.
  //
  BOOLEAN  KeepSupply;
  //
  // Results, for the logs.
  //
  UINT8    DpcdRev;
  UINT8    LinkRate;           // 0x0A = 2.7 Gbps, 0x06 = 1.62 Gbps
  UINT8    LaneCount;
  UINT8    Tries;
  UINT32   Step;               // EXYNOS_DISP_STEP_* reached / failed
  INT32    Error;
} EXYNOS_DISPLAY;

enum {
  EXYNOS_DISP_STEP_POWER = 1,
  EXYNOS_DISP_STEP_DP_INIT,
  EXYNOS_DISP_STEP_AUX,
  EXYNOS_DISP_STEP_TRAINING,
  EXYNOS_DISP_STEP_FIMD,
  EXYNOS_DISP_STEP_VIDEO,
  EXYNOS_DISP_STEP_BACKLIGHT,
  EXYNOS_DISP_STEP_DONE
};

//
// PanelCore.c: the whole sequence.
//
INTN
ExynosDisplayEnable (
  IN OUT EXYNOS_DISPLAY  *Disp,
  IN     UINT8           Brightness        // 0..255
  );

VOID
ExynosDisplayDisable (
  IN OUT EXYNOS_DISPLAY  *Disp
  );

VOID
ExynosDisplayBacklight (
  IN OUT EXYNOS_DISPLAY  *Disp,
  IN     UINT8           Brightness        // 0: backlight off, panel kept running
  );

VOID
ExynosDisplaySetBrightness (
  IN OUT EXYNOS_DISPLAY  *Disp,
  IN     UINT8           Brightness        // 0: backlight off
  );

VOID
ExynosDisplayLogState (
  IN EXYNOS_DISPLAY  *Disp
  );

//
// FimdCore.c
//
VOID
FimdStop (
  IN EXYNOS_DISPLAY  *Disp
  );

INTN
FimdStart (
  IN EXYNOS_DISPLAY  *Disp
  );

VOID
FimdSetScanout (
  IN EXYNOS_DISPLAY  *Disp,
  IN UINT32          FbPhys
  );

//
// ExynosDpCore.c
//
VOID
DpPhyPower (
  IN EXYNOS_DISPLAY  *Disp,
  IN BOOLEAN         On
  );

INTN
DpLinkUp (
  IN OUT EXYNOS_DISPLAY  *Disp
  );

INTN
DpVideoOn (
  IN EXYNOS_DISPLAY  *Disp
  );

VOID
DpVideoOff (
  IN EXYNOS_DISPLAY  *Disp
  );

INTN
DpcdRead (
  IN  EXYNOS_DISPLAY  *Disp,
  IN  UINT32          Addr,
  OUT UINT8           *Data,
  IN  UINT32          Count
  );

INTN
DpcdWrite (
  IN EXYNOS_DISPLAY  *Disp,
  IN UINT32          Addr,
  IN CONST UINT8     *Data,
  IN UINT32          Count
  );

#endif // __EXYNOS_DISPLAY_H__
