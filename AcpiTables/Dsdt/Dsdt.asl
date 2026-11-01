/** @file
  DSDT for v1awifi (Exynos5420).  The HAL only needs the MADT and the CSRT
  (MCT timer extension); devices are added as their Windows drivers appear
  (~/exynos5/ExynosWinDrivers).

  _HIDs EXYN5xxx are ours (EXYN0xxx are the Exynos4 / GT-N7000 ones).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

DefinitionBlock ("DSDT.aml", "DSDT", 2, "SAMSNG", "EXY5420 ", 1)
{
  Scope (\_SB)
  {
    //
    // USB3.0 DRD0 (Synopsys DWC3), peripheral mode only: the USB log and
    // debug channel (exynosdwc3).  UEFI leaves clocks and the DRD PHY up;
    // SPI 72 = GSI 104.
    //
    Device (UFN0)
    {
      Name (_HID, "EXYN5001")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12000000, 0x00010000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 104 }
      })
    }
  }
}
