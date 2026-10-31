/** @file
  v1awifi board glue for the USB device port: clocks and the USB3.0 DRD0
  PHY (0x12100000) that feeds the DWC3 core at 0x12000000.

  The Samsung bootloader does not leave USB in a known state (download mode
  uses it, a normal boot does not), so everything is brought up from
  scratch.  Sequences follow the downstream kernel:

    clock-exynos5420.c   "usbdrd30" (exynos-dwc3.0): CLK_GATE_IP_FSYS bits
                         19 + 25; "sclk_usbdrd30"/"sclk_usbphy30": mux
                         CLK_SRC_FSYS[22:20], dividers CLK_DIV_FSYS0[27:24]
                         and [19:16]
    setup-usb-phy.c      exynos5_usb_phy30_init(), exynos5_usb_phy30_tune()
                         (peripheral branch), exynos5_usb_phy_crport_ctrl()

  Reference clock: 24 MHz XusbXTI (mach-universal5420.c).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UsbFnDxe.h"

//
// CMU (EXYNOS5_PA_CMU = 0x10010000, regs-clock.h offsets).
//
#define CMU_BASE                 0x10010000
#define CLK_SRC_FSYS             (CMU_BASE + 0x10244)
#define CLK_SRC_MASK_FSYS        (CMU_BASE + 0x10340)
#define CLK_DIV_FSYS0            (CMU_BASE + 0x10548)
#define CLK_GATE_IP_FSYS         (CMU_BASE + 0x10944)

#define SRC_FSYS_USBDRD30_MASK   (0x7u << 20)     // 0 = XXTI (24 MHz)
#define SRC_MASK_FSYS_USBDRD30   (1u << 20)
#define DIV_FSYS0_USBDRD30_MASK  (0xFu << 24)
#define DIV_FSYS0_USBPHY30_MASK  (0xFu << 16)
#define GATE_IP_FSYS_USBDRD30_0  ((1u << 19) | (1u << 25))

//
// PMU: DRD0 PHY isolation (regs-pmu.h EXYNOS5_USBDEV_PHY_CONTROL).
//
#define PMU_USBDEV_PHY_CONTROL   0x10040704
#define PMU_PHY_ENABLE           (1u << 0)

//
// DRD PHY registers (regs-usb3-drd-phy.h).
//
#define PHY(x)                   (DWC3_PHY_BASE + (x))
#define PHY_LINKSYSTEM           0x04
#define PHY_UTMI                 0x08
#define PHY_CLKRST               0x10
#define PHY_REG0                 0x14
#define PHY_REG1                 0x18
#define PHY_PARAM0               0x1C
#define PHY_PARAM1               0x20
#define PHY_TEST                 0x28
#define PHY_BATCHG               0x30
#define PHY_RESUME               0x34

#define LINKSYSTEM_XHCI_VERSION_CONTROL (1u << 27)
#define LINKSYSTEM_FLADJ(x)      ((x) << 1)

#define UTMI_OTGDISABLE          (1u << 6)
#define UTMI_FORCESUSPEND        (1u << 1)
#define UTMI_FORCESLEEP          (1u << 0)

#define CLKRST_SSC_REF_CLK_SEL(x) ((x) << 23)
#define CLKRST_SSC_EN            (1u << 20)
#define CLKRST_REF_SSP_EN        (1u << 19)
#define CLKRST_MPLL_MULTIPLIER(x) ((x) << 11)
#define CLKRST_FSEL(x)           ((x) << 5)
#define CLKRST_RETENABLEN        (1u << 4)
#define CLKRST_REFCLKSEL(x)      ((x) << 2)
#define CLKRST_PORTRESET         (1u << 1)
#define CLKRST_COMMONONN         (1u << 0)

#define CLKSEL_24M               0x5             // EXYNOS5_CLKSEL_24M

#define REG0_CR_WRITE            (1u << 19)
#define REG0_CR_DATA_IN(x)       ((x) << 2)
#define REG0_CR_CAP_DATA         (1u << 1)
#define REG0_CR_CAP_ADDR         (1u << 0)
#define REG1_CR_ACK              (1u << 0)

#define TEST_POWERDOWN_SSP       (1u << 3)
#define TEST_POWERDOWN_HSP       (1u << 2)

#define PARAM0_SQRXTUNE_MASK     (0x7u << 6)
#define PARAM0_SQRXTUNE(x)       ((x) << 6)
#define PARAM0_TXVREFTUNE_MASK   (0xFu << 22)
#define PARAM0_TXVREFTUNE(x)     ((x) << 22)

/**
  Gate the DRD0 link/PHY clocks on and run the suspend/PHY reference clocks
  straight from the 24 MHz crystal (Dwc3.c programs GCTL.PwrDnScale for it).
**/
EFI_STATUS
UsbBoardClockInit (
  VOID
  )
{
  MmioAnd32 (CLK_SRC_FSYS, ~SRC_FSYS_USBDRD30_MASK);
  MmioAnd32 (CLK_DIV_FSYS0, ~(DIV_FSYS0_USBDRD30_MASK | DIV_FSYS0_USBPHY30_MASK));
  MmioOr32 (CLK_SRC_MASK_FSYS, SRC_MASK_FSYS_USBDRD30);
  MmioOr32 (CLK_GATE_IP_FSYS, GATE_IP_FSYS_USBDRD30_0);
  return EFI_SUCCESS;
}

/**
  exynos5_usb_phy30_init() for DRD0, 24 MHz reference.
  Called with the DWC3 core and both PHY interfaces held in soft reset.
**/
VOID
UsbBoardPhyInit (
  VOID
  )
{
  UINT32  Reg;

  MmioWrite32 (PMU_USBDEV_PHY_CONTROL, PMU_PHY_ENABLE);

  MmioWrite32 (PHY (PHY_REG0), 0x00000000);
  MmioWrite32 (PHY (PHY_PARAM0), 0x24d4e6e4);
  MmioWrite32 (PHY (PHY_RESUME), 0x00000000);

  MmioWrite32 (PHY (PHY_LINKSYSTEM),
               LINKSYSTEM_XHCI_VERSION_CONTROL | LINKSYSTEM_FLADJ (0x20));
  MmioWrite32 (PHY (PHY_PARAM1), 0x03fff020);     // not 5250
  MmioWrite32 (PHY (PHY_BATCHG), 0x00000004);

  Reg  = MmioRead32 (PHY (PHY_TEST));
  Reg &= ~(TEST_POWERDOWN_SSP | TEST_POWERDOWN_HSP);
  MmioWrite32 (PHY (PHY_TEST), Reg);

  MmioWrite32 (PHY (PHY_UTMI), UTMI_OTGDISABLE);

  Reg = CLKRST_REFCLKSEL (3) | CLKRST_FSEL (CLKSEL_24M) |
        CLKRST_MPLL_MULTIPLIER (0x68) | CLKRST_SSC_REF_CLK_SEL (0x88) |
        CLKRST_PORTRESET | CLKRST_RETENABLEN | CLKRST_REF_SSP_EN |
        CLKRST_SSC_EN | CLKRST_COMMONONN;
  MmioWrite32 (PHY (PHY_CLKRST), Reg);
  gBS->Stall (10);
  Reg &= ~CLKRST_PORTRESET;
  MmioWrite32 (PHY (PHY_CLKRST), Reg);
}

/**
  exynos5_usb_phy30_tune(), peripheral branch (non-N1A): squelch 0%,
  TX vref +10%.
**/
VOID
UsbBoardPhyTune (
  VOID
  )
{
  UINT32  Reg;

  Reg  = MmioRead32 (PHY (PHY_PARAM0));
  Reg &= ~(PARAM0_SQRXTUNE_MASK | PARAM0_TXVREFTUNE_MASK);
  Reg |= PARAM0_SQRXTUNE (0x3) | PARAM0_TXVREFTUNE (0xB);
  MmioWrite32 (PHY (PHY_PARAM0), Reg);
}

STATIC
VOID
PhyCrHandshake (
  IN UINT32  Reg,
  IN UINT32  Cmd
  )
{
  UINTN  Timeout;

  MmioWrite32 (PHY (PHY_REG0), Reg | Cmd);
  for (Timeout = 100; Timeout > 0; Timeout--) {
    if (MmioRead32 (PHY (PHY_REG1)) & REG1_CR_ACK) {
      break;
    }
    gBS->Stall (1);
  }
  if (Timeout == 0) {
    DEBUG ((DEBUG_ERROR, "UsbFn: PHY CR handshake timeout 1 (0x%08x)\n", Reg));
  }

  MmioWrite32 (PHY (PHY_REG0), Reg);
  for (Timeout = 100; Timeout > 0; Timeout--) {
    if (!(MmioRead32 (PHY (PHY_REG1)) & REG1_CR_ACK)) {
      break;
    }
    gBS->Stall (1);
  }
  if (Timeout == 0) {
    DEBUG ((DEBUG_ERROR, "UsbFn: PHY CR handshake timeout 2 (0x%08x)\n", Reg));
  }
}

/**
  exynos5_usb_phy_crport_ctrl(): write one PHY control register.
**/
VOID
UsbBoardPhyCrWrite (
  IN UINT16  Address,
  IN UINT16  Data
  )
{
  UINT32  Reg;

  Reg = REG0_CR_DATA_IN (Address);
  MmioWrite32 (PHY (PHY_REG0), Reg);
  PhyCrHandshake (Reg, REG0_CR_CAP_ADDR);

  Reg = REG0_CR_DATA_IN (Data);
  MmioWrite32 (PHY (PHY_REG0), Reg);
  PhyCrHandshake (Reg, REG0_CR_CAP_DATA);
  PhyCrHandshake (Reg, REG0_CR_WRITE);
}

/**
  exynos5_usb_phy30_exit(): power the PHY down for the next OS.
**/
VOID
UsbBoardPhyExit (
  VOID
  )
{
  MmioWrite32 (PHY (PHY_UTMI), UTMI_OTGDISABLE | UTMI_FORCESUSPEND | UTMI_FORCESLEEP);
  MmioAnd32 (PHY (PHY_CLKRST), ~(CLKRST_REF_SSP_EN | CLKRST_SSC_EN | CLKRST_COMMONONN));
  MmioOr32 (PHY (PHY_TEST), TEST_POWERDOWN_SSP | TEST_POWERDOWN_HSP);
  MmioWrite32 (PMU_USBDEV_PHY_CONTROL, 0);
}
