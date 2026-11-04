/** @file
  Exynos5420 camera receive path for raw frames: MIPI D-PHY (PMU), MIPI-CSIS
  and FIMC-Lite with output DMA to memory.  Programmed as the Samsung 3.4
  fimc-is-mc2 driver does (fimc-is-device-sensor.c start_mipi_csi /
  s5pcsis_*, fimc-is-device-flite.c fimc_is_flite_start / start_fimc_lite),
  RAW10 from the sensor, stored "normal" (one pixel per 16-bit word).
  Shared by UEFI and Windows through <ExynosEnv.h>.

  Rear camera: CSIS0 0x13C20000 + FIMC-Lite A 0x13C00000, D-PHY 0.
  Front camera: CSIS1 0x13C30000 + FIMC-Lite B 0x13C10000, D-PHY 1.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __CAM_CAPTURE_H__
#define __CAM_CAPTURE_H__

#include <ExynosEnv.h>

#define CAM_CSIS0_PHYS    0x13C20000u
#define CAM_FLITE_A_PHYS  0x13C00000u
#define CAM_CSIS1_PHYS    0x13C30000u
#define CAM_FLITE_B_PHYS  0x13C10000u
#define CAM_PMU_PHYS      0x10040000u     // MIPI_DPHY_CONTROL(n) at +0x714 + 4n

typedef struct {
  // filled by the caller
  UINTN   Csis;
  UINTN   Flite;
  UINTN   Pmu;
  UINT32  Dphy;           // D-PHY index (0 = CSIS0)
  UINT32  Width;          // sensor output incl. margins (IMX134 full: 3280)
  UINT32  Height;         // (2458)
  UINT32  Settle;         // HS settle (settle_imx134: 15 at 30 fps)
  UINT32  Lanes;          // 4
  UINT32  BufPa;          // frame buffer, Width * Height * 2 bytes
  BOOLEAN FliteB;         // FIMC-Lite B/C: no CIGENERAL (OTF source select is A's)
} CAM_CAPTURE;

typedef struct {
  UINT32  CsisCtrl, CsisDphy, CsisConfig, CsisIntSrc, CsisResol;
  UINT32  FliteCtrl, FliteCapture, FliteStatus, FliteStatus2, FliteStatus3, FliteSeq;
  UINT32  Dphy;
} CAM_CAPTURE_REGS;

VOID
CamCaptureSnapshot (
  IN  CAM_CAPTURE       *Cap,
  OUT CAM_CAPTURE_REGS  *Regs
  );

//
// D-PHY, CSIS and FIMC-Lite (one buffer slot) armed; frames are written
// as soon as the sensor streams.
//
VOID
CamCaptureStart (
  IN CAM_CAPTURE  *Cap
  );

//
// Clears and returns the FIMC-Lite status (CISTATUS) and CSIS interrupt
// sources, for polling: CISTATUS bit 4 = frame end, bit 5 = frame start,
// bit 7 = overflow.
//
UINT32
CamCaptureTakeStatus (
  IN  CAM_CAPTURE  *Cap,
  OUT UINT32       *CsisIntSrc
  );

//
// Streaming use: everything armed but FIMC-Lite capture off; then per frame
// Enable (the next frame start begins a capture), poll CamCaptureTakeStatus
// for bit 4 (frame end), Disable, read the buffer.  Disable before Stop.
//
VOID
CamCaptureConfigure (
  IN CAM_CAPTURE  *Cap
  );

VOID
CamCaptureEnable (
  IN CAM_CAPTURE  *Cap
  );

VOID
CamCaptureDisable (
  IN CAM_CAPTURE  *Cap
  );

VOID
CamCaptureStop (
  IN CAM_CAPTURE  *Cap
  );

#endif // __CAM_CAPTURE_H__
