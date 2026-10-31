/** @file
  v1awifi board glue for the USB device port: switch the shared USB2.0 PHY
  to device mode and bring up the UOTG PHY for 24 MHz XusbXTI.

  The 5420 has one shared USB2.0 PHY (HSPHY at 0x12130000) serving both the
  host controller and the UOTG (device) controller.  USB_CFG selects which
  role the PHY drives (1 = host, 0 = device) and the PHY must be powered and
  out of reset before the DWC2 core at 0x12140000 will enumerate.

  Register sequence follows the kernel's exynos_usb_dev_phy20_init()
  -> exynos5_usb_phy20_init() + exynos_usb_mux_change(0) for a device-mode
  bring-up with the 24 MHz reference clock (mach-universal5420.c:
  clk_xusbxti.rate = 24000000).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UsbFnDxe.h"

//
// PMU / syscon / PHY (map.h + regs-pmu.h + regs-usb-phy.h).
//
#define PMU_BASE                 0x10040000
#define USBHOST_PHY_CONTROL      (PMU_BASE + 0x070C)  // EXYNOS5 (not 5250)
#define USBHOST_PHY_ENABLE       (1u << 0)

#define SYSCON_BASE              0x10050100           // EXYNOS5_PA_SYSCON
#define USB_CFG                  (SYSCON_BASE + 0x230) // 1 = host, 0 = device

#define PHY_BASE                 0x12130000           // EXYNOS5_PA_HSPHY
#define HOST_CTRL0               (PHY_BASE + 0x00)
#define OTG_SYS                  (PHY_BASE + 0x38)

//
// HOST_CTRL0 bits (regs-usb-phy.h).
//
#define HOST_CTRL0_FSEL_MASK     (0x7u << 16)
#define HOST_CTRL0_CLKSEL_24M    (0x5u << 16)         // EXYNOS5_CLKSEL_24M
#define HOST_CTRL0_COMMONON_N    (1u << 9)
#define HOST_CTRL0_FORCESLEEP    (1u << 5)
#define HOST_CTRL0_FORCESUSPEND  (1u << 4)
#define HOST_CTRL0_SIDDQ         (1u << 6)
#define HOST_CTRL0_UTMISWRST     (1u << 2)
#define HOST_CTRL0_LINKSWRST     (1u << 1)
#define HOST_CTRL0_PHYSWRSTALL   (1u << 31)
#define HOST_CTRL0_PHYSWRST      (1u << 0)

//
// OTG_SYS bits (regs-usb-phy.h).
//
#define OTG_SYS_CTRL0_FSEL_MASK  (0x7u << 4)
#define OTG_SYS_CLKSEL_24M       (0x5u << 4)          // EXYNOS5_CLKSEL_24M
#define OTG_SYS_COMMON_ON        (1u << 7)
#define OTG_SYS_FORCE_SLEEP      (1u << 3)
#define OTG_SYS_SIDDQ_UOTG       (1u << 1)
#define OTG_SYS_FORCE_SUSPEND    (1u << 0)
#define OTG_SYS_OTGDISABLE       (1u << 2)
#define OTG_SYS_REF_CLK_SEL_MASK (0x3u << 9)
#define OTG_SYS_REF_CLK_SEL_2    (0x2u << 9)
#define OTG_SYS_PHYLINK_SW_RESET (1u << 14)
#define OTG_SYS_LINK_SW_RST_UOTG (1u << 13)
#define OTG_SYS_PHY0_SW_RST      (1u << 12)

/**
  Switch the shared PHY to device mode, power it and take it out of reset.
**/
STATIC
EFI_STATUS
UsbPhyInit (
  VOID
  )
{
  UINT32  Reg;

  //
  // USB_CFG = 0: device mode (exynos_usb_mux_change(pdev, 0)).
  //
  MmioWrite32 (USB_CFG, 0);

  //
  // Power the shared USB2.0 PHY.
  //
  MmioWrite32 (USBHOST_PHY_CONTROL, USBHOST_PHY_ENABLE);

  //
  // UOTG PHY reset sequence (otgphy_sys) for 24 MHz reference clock.
  //
  Reg = MmioRead32 (OTG_SYS);
  Reg &= ~OTG_SYS_CTRL0_FSEL_MASK;
  Reg |= OTG_SYS_CLKSEL_24M;
  Reg |= OTG_SYS_COMMON_ON;                       // not 5250: common on
  Reg &= ~(OTG_SYS_FORCE_SUSPEND | OTG_SYS_SIDDQ_UOTG | OTG_SYS_FORCE_SLEEP);
  Reg &= ~OTG_SYS_REF_CLK_SEL_MASK;
  Reg |= OTG_SYS_REF_CLK_SEL_2 | OTG_SYS_OTGDISABLE;
  Reg |= OTG_SYS_PHY0_SW_RST | OTG_SYS_LINK_SW_RST_UOTG |
         OTG_SYS_PHYLINK_SW_RESET;
  MmioWrite32 (OTG_SYS, Reg);
  gBS->Stall (10);
  Reg &= ~(OTG_SYS_PHY0_SW_RST | OTG_SYS_LINK_SW_RST_UOTG |
           OTG_SYS_PHYLINK_SW_RESET);
  MmioWrite32 (OTG_SYS, Reg);

  //
  // Host/UTMI side out of suspend/reset, link + utmi in reset then released.
  //
  Reg = MmioRead32 (HOST_CTRL0);
  Reg &= ~HOST_CTRL0_FSEL_MASK;
  Reg |= HOST_CTRL0_CLKSEL_24M;
  Reg &= ~HOST_CTRL0_COMMONON_N;                  // keep common block on
  Reg &= ~(HOST_CTRL0_PHYSWRST | HOST_CTRL0_PHYSWRSTALL | HOST_CTRL0_SIDDQ);
  Reg &= ~(HOST_CTRL0_FORCESUSPEND | HOST_CTRL0_FORCESLEEP);
  Reg |= HOST_CTRL0_LINKSWRST | HOST_CTRL0_UTMISWRST;
  MmioWrite32 (HOST_CTRL0, Reg);
  gBS->Stall (10);
  Reg &= ~(HOST_CTRL0_LINKSWRST | HOST_CTRL0_UTMISWRST);
  MmioWrite32 (HOST_CTRL0, Reg);

  //
  // PLL lock.
  //
  gBS->Stall (1000);
  return EFI_SUCCESS;
}

/**
  Board init for the USB device port.
**/
EFI_STATUS
UsbBoardInit (
  VOID
  )
{
  return UsbPhyInit ();
}
