/** @file
  Exynos5420 camera receive path: see CamCapture.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "CamCapture.h"

//
// PMU MIPI D-PHY control (regs-pmu.h, 5420: 0x714 + 4n)
//
#define PMU_MIPI_DPHY(n)        (0x714 + (n) * 4)
#define   DPHY_ENABLE             (1u << 0)
#define   DPHY_SRESETN            (1u << 1)

//
// MIPI-CSIS (5420 layout, fimc-is-device-sensor.c)
//
#define CSIS_CTRL               0x00
#define   CSIS_CTRL_ALIGN_32BIT   (1u << 20)
#define   CSIS_CTRL_UPDATE_SHADOW (1u << 16)
#define   CSIS_CTRL_WCLK_EXTCLK   (1u << 8)
#define   CSIS_CTRL_RESET         (1u << 4)
#define   CSIS_CTRL_LANES(n)      (((n) - 1) << 2)
#define   CSIS_CTRL_ENABLE        (1u << 0)
#define CSIS_DPHYCTRL           0x04
#define   CSIS_DPHY_HSS_MASK      (0xFFu << 24)
#define   CSIS_DPHY_ENABLE        0x1Fu
#define CSIS_CONFIG             0x08
#define   CSIS_CFG_FMT_MASK       (0x3Fu << 2)
#define   CSIS_CFG_FMT_RAW10      (0x2Bu << 2)
#define   CSIS_CFG_START_INT(x)   ((x) << 20)
#define CSIS_INTMSK             0x10
#define   CSIS_INTMSK_EN_ALL      0xF1101117u
#define CSIS_INTSRC             0x14
#define CSIS_RESOL              0x2C

//
// FIMC-Lite (fimc-is-device-flite.c)
//
#define FL_CISRCSIZE            0x00
#define FL_CIGCTRL              0x04
#define   FL_RAW10                (0x2Bu << 24)
#define   FL_FMT_MASK             (0x3Fu << 24)
#define   FL_OLOCAL_DISABLE       (1u << 22)
#define   FL_ODMA_DISABLE         (1u << 20)
#define   FL_SWRST_REQ            (1u << 19)
#define   FL_SWRST_RDY            (1u << 18)
#define   FL_SWRST                (1u << 17)
#define   FL_INVPOL_MASK          (7u << 12)
#define   FL_IRQ_DISABLE_MASK     ((1u << 8) | (1u << 7) | (1u << 6) | (1u << 5))
#define   FL_SELCAM_MIPI          (1u << 3)
#define FL_CIIMGCPT             0x08
#define   FL_IMGCPTEN             (1u << 31)
#define FL_CIWDOFST             0x10
#define   FL_WINOFSEN             (1u << 31)
#define FL_CIWDOFST2            0x14
#define FL_CIODMAFMT            0x18
#define   FL_PACK12               (1u << 14)
#define FL_CIOCAN               0x20
#define FL_CIOSA                0x30
#define FL_CISTATUS             0x40
#define FL_CISTATUS2            0x44
#define   FL_LASTCAPEND           (1u << 1)
#define FL_CISTATUS3            0x48
#define FL_CIGENERAL            0xFC
#define FL_CIFCNTSEQ            0x100
#define FL_CIOSA_N(n)           (0x200 + ((n) - 1) * 4)

VOID
CamCaptureSnapshot (
  IN  CAM_CAPTURE       *Cap,
  OUT CAM_CAPTURE_REGS  *Regs
  )
{
  Regs->CsisCtrl     = MmioRead32 (Cap->Csis + CSIS_CTRL);
  Regs->CsisDphy     = MmioRead32 (Cap->Csis + CSIS_DPHYCTRL);
  Regs->CsisConfig   = MmioRead32 (Cap->Csis + CSIS_CONFIG);
  Regs->CsisIntSrc   = MmioRead32 (Cap->Csis + CSIS_INTSRC);
  Regs->CsisResol    = MmioRead32 (Cap->Csis + CSIS_RESOL);
  Regs->FliteCtrl    = MmioRead32 (Cap->Flite + FL_CIGCTRL);
  Regs->FliteCapture = MmioRead32 (Cap->Flite + FL_CIIMGCPT);
  Regs->FliteStatus  = MmioRead32 (Cap->Flite + FL_CISTATUS);
  Regs->FliteStatus2 = MmioRead32 (Cap->Flite + FL_CISTATUS2);
  Regs->FliteStatus3 = MmioRead32 (Cap->Flite + FL_CISTATUS3);
  Regs->FliteSeq     = MmioRead32 (Cap->Flite + FL_CIFCNTSEQ);
  Regs->Dphy         = MmioRead32 (Cap->Pmu + PMU_MIPI_DPHY (Cap->Dphy));
}

STATIC
VOID
FliteStart (
  IN CAM_CAPTURE  *Cap
  )
{
  UINTN   F = Cap->Flite;
  UINT32  Ctrl;
  UINTN   Retry;
  UINTN   Index;

  //
  // flite_hw_force_reset(): request, wait ready, reset.
  //
  MmioOr32 (F + FL_CIGCTRL, FL_SWRST_REQ);
  for (Retry = 0; (Retry < 1000) && ((MmioRead32 (F + FL_CIGCTRL) & FL_SWRST_RDY) == 0); Retry++) {
    MicroSecondDelay (1);
  }
  MmioOr32 (F + FL_CIGCTRL, FL_SWRST);
  MicroSecondDelay (10);

  //
  // init_fimc_lite(): no slot in use, all addresses invalid; then slot 0
  // (CIOSA) = our buffer, in use.
  //
  MmioWrite32 (F + FL_CIFCNTSEQ, 0);
  MmioWrite32 (F + FL_CIOSA, 0xFFFFFFFF);
  for (Index = 1; Index < 32; Index++) {
    MmioWrite32 (F + FL_CIOSA_N (Index), 0xFFFFFFFF);
  }
  MmioWrite32 (F + FL_CIOSA, Cap->BufPa);
  MmioWrite32 (F + FL_CIFCNTSEQ, 1);

  //
  // Output DMA on (normal 16-bit storage), local (OTF to 3AA) off.
  //
  Ctrl = MmioRead32 (F + FL_CIGCTRL);
  Ctrl &= ~(FL_ODMA_DISABLE | FL_SWRST | FL_SWRST_REQ);
  Ctrl |= FL_OLOCAL_DISABLE;
  MmioWrite32 (F + FL_CIGCTRL, Ctrl);
  MmioAnd32 (F + FL_CIODMAFMT, ~FL_PACK12);

  //
  // start_fimc_lite()
  //
  if (!Cap->FliteB) {
    MmioWrite32 (F + FL_CIGENERAL, 0);                                // CAM_A
  }
  MmioWrite32 (F + FL_CISRCSIZE, (Cap->Width << 16) | Cap->Height);
  MmioWrite32 (F + FL_CIOCAN, (Cap->Height << 16) | Cap->Width);
  Ctrl = MmioRead32 (F + FL_CIGCTRL);
  Ctrl &= ~(FL_FMT_MASK | FL_IRQ_DISABLE_MASK | FL_INVPOL_MASK);
  Ctrl |= FL_SELCAM_MIPI | FL_RAW10;
  MmioWrite32 (F + FL_CIGCTRL, Ctrl);
  MmioWrite32 (F + FL_CIWDOFST, FL_WINOFSEN);                         // offsets 0
  MmioWrite32 (F + FL_CIWDOFST2, 0);
  MmioAnd32 (F + FL_CISTATUS2, ~FL_LASTCAPEND);
  MmioWrite32 (F + FL_CISTATUS, 0);
  MmioOr32 (F + FL_CIIMGCPT, FL_IMGCPTEN);
}

STATIC
VOID
CsisStart (
  IN CAM_CAPTURE  *Cap
  )
{
  UINTN   C = Cap->Csis;
  UINT32  Val;

  // s5pcsis_reset()
  MmioOr32 (C + CSIS_CTRL, CSIS_CTRL_RESET);
  MicroSecondDelay (10);

  // s5pcsis_set_hsync_settle()
  MmioAndThenOr32 (C + CSIS_DPHYCTRL, ~CSIS_DPHY_HSS_MASK, Cap->Settle << 24);

  // __s5pcsis_set_format(): RAW10, start interval 1, resolution
  Val = MmioRead32 (C + CSIS_CONFIG);
  Val = (Val & ~CSIS_CFG_FMT_MASK) | CSIS_CFG_FMT_RAW10 | CSIS_CFG_START_INT (1);
  MmioWrite32 (C + CSIS_CONFIG, Val);
  MmioWrite32 (C + CSIS_RESOL, (Cap->Width << 16) | Cap->Height);

  // s5pcsis_set_params(): no 32-bit align, internal clock, shadow update
  MmioAnd32 (C + CSIS_CTRL, ~(CSIS_CTRL_ALIGN_32BIT | CSIS_CTRL_WCLK_EXTCLK));
  MmioOr32 (C + CSIS_CTRL, CSIS_CTRL_UPDATE_SHADOW);

  // s5pcsis_system_enable(): lanes, ext clock, enable; D-PHY lanes on
  MmioOr32 (C + CSIS_CTRL, CSIS_CTRL_WCLK_EXTCLK | CSIS_CTRL_LANES (Cap->Lanes) | CSIS_CTRL_ENABLE);
  MmioOr32 (C + CSIS_DPHYCTRL, CSIS_DPHY_ENABLE);

  // s5pcsis_enable_interrupts(): sources visible in INTSRC
  MmioOr32 (C + CSIS_INTMSK, CSIS_INTMSK_EN_ALL);
  MmioWrite32 (C + CSIS_INTSRC, 0xFFFFFFFF);
}

VOID
CamCaptureStart (
  IN CAM_CAPTURE  *Cap
  )
{
  // enable_mipi(): D-PHY out of reset, enabled
  MmioOr32 (Cap->Pmu + PMU_MIPI_DPHY (Cap->Dphy), DPHY_SRESETN);
  MmioOr32 (Cap->Pmu + PMU_MIPI_DPHY (Cap->Dphy), DPHY_ENABLE);

  // the sensor node's back start (FIMC-Lite) comes before the front start (CSIS)
  FliteStart (Cap);
  CsisStart (Cap);
}

UINT32
CamCaptureTakeStatus (
  IN  CAM_CAPTURE  *Cap,
  OUT UINT32       *CsisIntSrc
  )
{
  UINT32  Status = MmioRead32 (Cap->Flite + FL_CISTATUS);

  MmioWrite32 (Cap->Flite + FL_CISTATUS, 0);
  *CsisIntSrc = MmioRead32 (Cap->Csis + CSIS_INTSRC);
  MmioWrite32 (Cap->Csis + CSIS_INTSRC, *CsisIntSrc);
  return Status;
}

VOID
CamCaptureStop (
  IN CAM_CAPTURE  *Cap
  )
{
  MmioAnd32 (Cap->Flite + FL_CIIMGCPT, ~FL_IMGCPTEN);
  MmioOr32 (Cap->Flite + FL_CIGCTRL, FL_IRQ_DISABLE_MASK);
  MmioAnd32 (Cap->Csis + CSIS_INTMSK, ~CSIS_INTMSK_EN_ALL);
  MmioAnd32 (Cap->Csis + CSIS_CTRL, ~CSIS_CTRL_ENABLE);
  MmioAnd32 (Cap->Csis + CSIS_DPHYCTRL, ~CSIS_DPHY_ENABLE);
  MmioAnd32 (Cap->Pmu + PMU_MIPI_DPHY (Cap->Dphy), ~(DPHY_ENABLE | DPHY_SRESETN));
}

VOID
CamCaptureConfigure (
  IN CAM_CAPTURE  *Cap
  )
{
  CamCaptureStart (Cap);
  MmioAnd32 (Cap->Flite + FL_CIIMGCPT, ~FL_IMGCPTEN);
}

VOID
CamCaptureEnable (
  IN CAM_CAPTURE  *Cap
  )
{
  //
  // The buffer slot may be spent after a frame: arm slot 0 again.
  //
  MmioWrite32 (Cap->Flite + FL_CIOSA, Cap->BufPa);
  MmioWrite32 (Cap->Flite + FL_CIFCNTSEQ, 1);
  MmioWrite32 (Cap->Flite + FL_CISTATUS, 0);
  MmioOr32 (Cap->Flite + FL_CIIMGCPT, FL_IMGCPTEN);
}

VOID
CamCaptureDisable (
  IN CAM_CAPTURE  *Cap
  )
{
  MmioAnd32 (Cap->Flite + FL_CIIMGCPT, ~FL_IMGCPTEN);
}
