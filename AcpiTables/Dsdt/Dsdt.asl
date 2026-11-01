/** @file
  DSDT for v1awifi (Exynos5420).  The HAL only needs the MADT and the CSRT
  (MCT timer extension); processors are listed for the OS's processor
  driver, other devices are added as their Windows drivers appear
  (~/exynos5/ExynosWinDrivers).

  _HIDs EXYN5xxx are ours (EXYN0xxx are the Exynos4 / GT-N7000 ones).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

DefinitionBlock ("DSDT.aml", "DSDT", 2, "SAMSNG", "EXY5420 ", 1)
{
  Scope (\_SB)
  {
    //
    // The four Cortex-A15 cores of cluster 0.  _UID = the MADT GICC's ACPI
    // processor UID (= GIC CPU interface number, MpParkDxe).  The A7
    // cluster is not used.
    //
    Device (CPU0) { Name (_HID, "ACPI0007") Name (_UID, 0) Method (_STA) { Return (0xF) } }
    Device (CPU1) { Name (_HID, "ACPI0007") Name (_UID, 1) Method (_STA) { Return (0xF) } }
    Device (CPU2) { Name (_HID, "ACPI0007") Name (_UID, 2) Method (_STA) { Return (0xF) } }
    Device (CPU3) { Name (_HID, "ACPI0007") Name (_UID, 3) Method (_STA) { Return (0xF) } }

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

    //
    // eMMC on dw_mmc channel 0 (exynosemmc, Storport, polled IDMAC; no
    // interrupt).  The window includes the eMMCP security unit at +0x1000.
    // UEFI leaves clocks, pins and power set up.
    //
    Device (EMMC)
    {
      Name (_HID, "EXYN5007")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12200000, 0x00002000)
      })
    }

    //
    // I2C0 (s3c2410-style), touchscreen bus, and hs-i2c4, S Pen bus
    // (exynosi2c, polled).  Pins and clock gates are left set up by UEFI.
    //
    Device (I2C0)
    {
      Name (_HID, "EXYN5002")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12C60000, 0x00000100)
      })
    }

    Device (HSI4)
    {
      Name (_HID, "EXYN5004")
      Name (_UID, 4)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12E00000, 0x00000100)
      })
    }

    //
    // GPIO interrupt lines (exynosgpio): one window over the RT and T
    // controllers, one interrupt per bank, in bank order.
    //   bank 0, pins 0-7:  GPD1, GPIO group interrupt SPI 78 = GSI 110
    //   bank 1, pins 8-15: GPX3, EINT16-31 SPI 32 = GSI 64
    //
    Device (GPIO)
    {
      Name (_HID, "EXYN5003")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x13400000, 0x00020000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 110 }
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 64 }
      })
    }

    //
    // Atmel mXT1664S touchscreen: I2C0 address 0x4A, CHG = GPD1(0), level
    // low (board-vienna-input.c).  UEFI powers it and leaves it on.
    //
    Device (TSC0)
    {
      Name (_HID, "EXYN5005")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        I2cSerialBus (0x4A, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C0", 0, ResourceConsumer)
        GpioInt (Level, ActiveLow, Exclusive, PullNone, 0, "\\_SB.GPIO") { 0 }
      })
    }

    //
    // Wacom G9PLL S Pen digitizer: hs-i2c4 address 0x56, IRQ = GPX3(5),
    // rising edge (pin 13).  UEFI powers it and leaves it on.
    //
    Device (PEN0)
    {
      Name (_HID, "EXYN5006")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        I2cSerialBus (0x56, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.HSI4", 0, ResourceConsumer)
        GpioInt (Edge, ActiveHigh, Exclusive, PullNone, 0, "\\_SB.GPIO") { 13 }
      })
    }
  }
}
