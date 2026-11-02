/** @file
  DSDT for v1awifi (Exynos5420).  The HAL only needs the MADT and the CSRT
  (MCT timer extension); processors are listed for the OS's processor
  driver, other devices are added as their Windows drivers appear
  (~/exynos5/ExynosWinDrivers).

  _HIDs EXYN5xxx are ours (EXYN0xxx are the Exynos4 / GT-N7000 ones).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "../PlatformDma.h"

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
    // controllers, one over the interrupt combiner (EINT0-15 go through
    // it), one interrupt per bank, in bank order.
    //   bank 0, pins 0-7:   GPD1, GPIO group interrupt SPI 78 = GSI 110
    //   bank 1, pins 8-15:  GPX3, EINT16-31 SPI 32 = GSI 64
    //   bank 2, pins 16-23: GPX2, the same EINT16-31 line
    //   bank 3, pins 24-31: GPX0 pins 2-3 (EINT2-3), combiner 25 = GSI 57
    //   bank 4, pins 32-39: GPX0 pins 4-5 (EINT4-5), combiner 26 = GSI 58
    //   bank 5, pins 40-47: GPG0 (LB controller, third window), I/O only
    //   bank 6, pins 48-55: GPJ4 (LB controller), I/O only
    //   bank 7, pins 56-63: GPY7, I/O only
    // The I/O-only banks get a Shared copy of bank 0's line (GpioClx binds
    // one interrupt per bank); they never enable a pin on it.
    //
    Device (GPIO)
    {
      Name (_HID, "EXYN5003")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x13400000, 0x00020000)
        Memory32Fixed (ReadWrite, 0x10440000, 0x00001000)   // interrupt combiner
        Memory32Fixed (ReadWrite, 0x14000000, 0x00001000)   // LB controller: GPE0..GPJ4
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 110 }  // bank 0 GPD1
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 64 }   // bank 1 GPX3
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 64 }   // bank 2 GPX2
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 57 }   // bank 3 GPX0 / combiner 25
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 58 }   // bank 4 GPX0 / combiner 26
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 110 }  // bank 5 GPG0 (I/O only)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 110 }  // bank 6 GPJ4 (I/O only)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 110 }  // bank 7 GPY7 (I/O only)
      })
    }

    //
    // Battery (exynosbatt, battery class miniport): MAX17050 fuel gauge on
    // GPB0(3)/(4) and the MAX77888 charger + MUIC on GPB3(6)/(7), both
    // bit-banged I2C like the kernel's i2c-gpio buses 18 and 17.  The window
    // is the GPIO controller holding GPB0 and GPB3.
    //
    Device (BAT0)
    {
      Name (_HID, "EXYN5008")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x14010000, 0x00001000)
      })
    }

    //
    // Wi-Fi (exynoswlan, NDIS): Broadcom BCM4354 on the SDIO port of dw_mmc
    // channel 1.  The driver powers the chip (WLAN_EN GPY7(7)) and sets up
    // the SDIO pins and sclk_dwmci1 itself.  Frames are signalled by the
    // card's in-band SDIO interrupt through dw_mmc1's line, SPI 76 = GSI 108.
    //
    Device (WLAN)
    {
      Name (_HID, "EXYN5009")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12210000, 0x00001000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 108 }
      })
    }

    //
    // SPI2 (exynosspi, SpbCx, polled): the WM5102 codec's bus.  UEFI's
    // SpiDxe sets the clock (SCK 6 MHz) and the GPB1 pin functions; the
    // chip select GPB1(2) is a plain GPIO the driver maps itself (BAT0
    // already claims that GPIO controller's page).
    //
    Device (SPI2)
    {
      Name (_HID, "EXYN500A")
      Name (_UID, 2)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12D40000, 0x00000100)
      })
    }

    //
    // Audio (exynosaudio, WaveRT): Wolfson WM5102 on SPI2 (mode 0, chip
    // select 0) + Exynos I2S0 as clock slave fed by its internal DMA.
    // Windows: I2S0, the audio subsystem clock block, the audio GPIO block
    // (GPZ = I2S0 pins), the I2S0 interrupt SPI 115 = GSI 147, the codec's
    // SPI connection, LDOENA GPG0(1) = pin 41, /RESET GPJ4(0) = pin 48 and
    // its interrupt GPX2(5) = pin 21 (level high; jack / button events),
    // the main mic's external bias MAINMIC_BIAS GPY7(6) = pin 62, and the
    // capture DMA: I2S0 RX FIFO through ADMA0 (system DMA, halextpl330),
    // request line 2, channel 0, 32-bit.  MCLK1 (CLKOUT) and the MAU power
    // domain are in the PMU, which the driver maps itself.
    //
    Device (AUDI)
    {
      Name (_HID, "EXYN500B")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x03830000, 0x00000100)   // I2S0
        Memory32Fixed (ReadWrite, 0x03810000, 0x00000100)   // audio subsystem clocks
        Memory32Fixed (ReadWrite, 0x03860000, 0x00000100)   // GPZ
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 147 }
        SpiSerialBus (0, PolarityLow, FourWireMode, 8, ControllerInitiated, 6000000, ClockPolarityLow,
                      ClockPhaseFirst, "\\_SB.SPI2", 0, ResourceConsumer)
        GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly, "\\_SB.GPIO") { 41 }   // LDOENA
        GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly, "\\_SB.GPIO") { 48 }   // /RESET
        GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly, "\\_SB.GPIO") { 62 }   // MAINMIC_BIAS
#if EXYNOS_PL330_DMA
        FixedDMA (2, 0, Width32bit)                                                      // I2S0 RX capture
#endif
        GpioInt (Level, ActiveHigh, Exclusive, PullNone, 0, "\\_SB.GPIO") { 21 }
      })
    }

    //
    // Hardware buttons as a Windows Button Array (inbox driver).  The
    // GpioInts are in the order Windows expects: power, Windows (home),
    // volume up, volume down.  All four keys are active low with pull-ups
    // (KeypadDeviceImplLib): power GPX2(2) = pin 18, home GPX0(5) = 37,
    // volume up GPX0(2) = 26, volume down GPX0(3) = 27 (exynosgpio banks).
    //
    Device (BTNS)
    {
      Name (_HID, "PNP0C40")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        GpioInt (Edge, ActiveBoth, ExclusiveAndWake, PullUp, 0, "\\_SB.GPIO") { 18 }
        GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0, "\\_SB.GPIO") { 37 }
        GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0, "\\_SB.GPIO") { 26 }
        GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0, "\\_SB.GPIO") { 27 }
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
