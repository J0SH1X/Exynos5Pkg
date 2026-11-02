/** @file
  Exynos5420 FIMD1 for the LSL122DL01: RGB video timing towards DP1 and
  window 0 scanning out one 2560x1600 XRGB8888 buffer.

  Register layout from mainline (include/video/samsung_fimd.h,
  drivers/gpu/drm/exynos/exynos_drm_fimd.c, exynos5420_fimd_driver_data:
  timing block at +0x20000, VIDOUT_CON, DP_MIE_CLKCON, the _E extension
  bits); panel timing from board-v1-display.c / panel-edp.c (HFP 62,
  HSYNC 22, HBP 70, VFP 6, VSYNC 6, VBP 21, 266 MHz).  WINCON0 / VIDCON0
  keep the values the bootloader runs the panel with (0x8035 / 0x8020).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "ExynosDisplay.h"

#define VIDCON0             0x000
#define VIDCON0_ENVID       (1u << 1)
#define VIDCON0_ENVID_F     (1u << 0)
#define VIDCON0_BOOT_BITS   0x8020u             // bootloader's VIDCON0 without ENVID
#define WINCON0             0x020
#define WINCON0_BOOT        0x8035u             // window on, 24-bit (A)RGB, word swap
#define SHADOWCON           0x034
#define VIDOSD0A            0x040
#define VIDOSD0B            0x044
#define VIDW00ADD0B0        0x0A0
#define VIDW00ADD1B0        0x0D0
#define VIDW00ADD2          0x100
#define WIN0MAP             0x180
#define DP_MIE_CLKCON       0x27C
#define DP_MIE_CLK_DP_ENABLE 0x2

#define TIMING              0x20000             // exynos5420: timing registers
#define VIDOUT_CON          (TIMING + 0x000)
#define VIDCON1             (TIMING + 0x004)
#define VIDTCON0            (TIMING + 0x010)
#define VIDTCON1            (TIMING + 0x014)
#define VIDTCON2            (TIMING + 0x018)

#define HFP   62
#define HSW   22
#define HBP   70
#define VFP   6
#define VSW   6
#define VBP   21

#define X_LO(v)   (((v) & 0x7FFu) << 11)
#define X_E(v)    ((((v) & 0x800u) >> 11) << 23)
#define Y_LO(v)   ((v) & 0x7FFu)
#define Y_E(v)    ((((v) & 0x800u) >> 11) << 22)

#define RD(o)     MmioRead32 (Disp->Fimd + (o))
#define WR(o, v)  MmioWrite32 (Disp->Fimd + (o), (v))

VOID
FimdStop (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  // Clearing ENVID_F stops at the end of the frame.
  WR (VIDCON0, RD (VIDCON0) & ~VIDCON0_ENVID_F);
  EnvSleepMs (20);
  WR (VIDCON0, RD (VIDCON0) & ~(VIDCON0_ENVID | VIDCON0_ENVID_F));
  WR (WINCON0, RD (WINCON0) & ~1u);
}

VOID
FimdSetScanout (
  IN EXYNOS_DISPLAY  *Disp,
  IN UINT32          FbPhys
  )
{
  UINT32  Line = EXYNOS_DISP_WIDTH * 4;
  UINT32  Offset = Disp->Stride - Line;

  WR (VIDW00ADD0B0, FbPhys);
  WR (VIDW00ADD1B0, FbPhys + Disp->Stride * EXYNOS_DISP_HEIGHT);
  WR (VIDW00ADD2, ((Offset & 0x1FFFu) << 13) | ((((Offset & 0x2000u) >> 13)) << 27) |
                  (Line & 0x1FFFu) | (((Line & 0x2000u) >> 13) << 26));
}

INTN
FimdStart (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  UINT32  W = EXYNOS_DISP_WIDTH - 1;
  UINT32  H = EXYNOS_DISP_HEIGHT - 1;

  // FIMD1's video goes to DP1.
  WR (DP_MIE_CLKCON, DP_MIE_CLK_DP_ENABLE);

  // RGB interface, no polarity inversion, timing for 2560x1600 @ 60 Hz.
  WR (VIDOUT_CON, 0);
  WR (VIDCON1, 0);
  WR (VIDTCON0, ((VBP - 1) << 16) | ((VFP - 1) << 8) | (VSW - 1));
  WR (VIDTCON1, ((HBP - 1) << 16) | ((HFP - 1) << 8) | (HSW - 1));
  WR (VIDTCON2, (((H & 0x7FFu) << 11) | (W & 0x7FFu)) | (((H & 0x800u) >> 11) << 23) | (((W & 0x800u) >> 11) << 22));

  // Window 0: full screen, the scanout buffer, no colour map.
  WR (VIDOSD0A, 0);
  WR (VIDOSD0B, X_LO (W) | X_E (W) | Y_LO (H) | Y_E (H));
  FimdSetScanout (Disp, Disp->FbPhys);
  WR (WIN0MAP, 0);
  WR (WINCON0, WINCON0_BOOT);
  WR (SHADOWCON, RD (SHADOWCON) | 1u);

  WR (VIDCON0, VIDCON0_BOOT_BITS | VIDCON0_ENVID | VIDCON0_ENVID_F);
  return 0;
}

#undef RD
#undef WR
