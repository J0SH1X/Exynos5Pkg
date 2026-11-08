/** @file
  DSDT for v1awifi (Exynos5420).  The HAL only needs the MADT and the CSRT
  (MCT timer extension); processors are listed for the OS's processor
  driver, other devices are added as their Windows drivers appear
  (~/exynos5/ExynosWinDrivers).

  _HIDs EXYN5xxx are ours (EXYN0xxx are the Exynos4 / GT-N7000 ones).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "../PlatformDma.h"
#include "../PlatformThermal.h"

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
    // Power engine plugin (exynospep, boot start): processor idle / perf
    // states for fxppm, device power coordination.  No resources yet (CPU
    // DVFS will add the PMIC's I2C for VDD_ARM).
    //
    Device (PEP0)
    {
      Name (_HID, "EXYN5014")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
    }

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
    // microSD slot on dw_mmc channel 2 (exynossd, Storport, polled IDMAC;
    // no interrupt).  Unlike EMMC the card is optional and UEFI only sets
    // the slot up when one is present at boot, so the driver does the
    // clock, pins and slot power itself.
    //
    Device (SDC0)
    {
      Name (_HID, "EXYN5017")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12220000, 0x00002000)
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

    //
    // I2C3 (s3c2410-style): the sensor bus (BMA255, BMG160, AK8963C,
    // CM3323).  UEFI's SensorDxe sets the GPA1(2)/(3) pin functions.
    //
    Device (I2C3)
    {
      Name (_HID, "EXYN5002")
      Name (_UID, 3)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12C90000, 0x00000100)
      })
    }

    //
    // hs-i2c3 (UEFI bus 7): MAX77802 PMIC at 0x09, owned by the PMIC device
    // (exynospmic) below.  Pins GPB2(2/3) are muxed by UEFI (SensorDxe /
    // CpuFreqDxe).
    //
    Device (HSI3)
    {
      Name (_HID, "EXYN5004")
      Name (_UID, 3)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12CD0000, 0x00000100)
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
    //   bank 8, pins 64-71: GPX0 pin 1 (EINT1), combiner 24 = GSI 56
    //   bank 9, pins 72-79: GPX1 pins 2-3 (EINT10-11), combiner 29 = GSI 61
    //   bank 10, pins 80-87: GPX1 pins 6-7 (EINT14-15), combiner 31 = GSI 63
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
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 56 }   // bank 8 GPX0 / combiner 24
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 61 }   // bank 9 GPX1 / combiner 29
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 63 }   // bank 10 GPX1 / combiner 31
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
    // UART0 (exynosuart, SerCx2): the Broadcom Bluetooth controller's
    // transport, RTS/CTS on GPA0[2]/[3] (the driver muxes GPA0[0..3] and
    // sets the clock).  SPI 51 = GSI 83; PL330 PDMA0 request lines 45 / 46
    // (peripherals 13 RX / 14 TX) on its channels 0 / 1.
    //
    Device (UAR0)
    {
      Name (_HID, "EXYN5011")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12C00000, 0x00000100)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 83 }
#if EXYNOS_PL330_DMA
        FixedDMA (45, 0, Width8bit)                                                      // RX
        FixedDMA (46, 1, Width8bit)                                                      // TX
#endif
      })
    }

    //
    // Bluetooth (exynosbt, serial HCI bus driver): Broadcom BCM4350-family
    // controller on UART0, 115200 8N1 with RTS/CTS until the driver raises
    // the rate.  BT_EN GPG0(7) (pin 47), BT_WAKE GPX3(2) (pin 10), both
    // active high; HOST_WAKE GPX2(1) (pin 17) only connected for its
    // PullNone, as board-bluetooth-bcm4339.c sets it before power-up.
    //
    Device (BTH0)
    {
      Name (_HID, "EXYN5012")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        UartSerialBus (115200, DataBitsEight, StopBitsOne, 0xC0, LittleEndian, ParityTypeNone,
                       FlowControlHardware, 0x100, 0x100, "\\_SB.UAR0", 0, ResourceConsumer, , )
        GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly, "\\_SB.GPIO") { 47 }   // BT_EN
        GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly, "\\_SB.GPIO") { 10 }   // BT_WAKE
        GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionInputOnly, "\\_SB.GPIO") { 17 }    // HOST_WAKE: no pull (the reset pull-down kept the controller from starting)
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

    //
    // Sensors on I2C3, one driver each (HID sensor minidrivers under
    // mshidkmdf; Windows' HID sensor class driver on top).  Interrupt pins
    // per board-universal5420-sensor.c; the chips' interrupt outputs are
    // push-pull, active high as their drivers program them.  The Bosch
    // data-ready outputs pulse once per sample (rising edge, as mainline's
    // DTS); the AK8963C's DRDY stays up until the sample is read (level).
    //
    // BMA255 accelerometer: INT1 GPX1(3) = pin 75, INT2 GPX1(7) = pin 87.
    Device (ACC0)
    {
      Name (_HID, "EXYN500C")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        I2cSerialBus (0x18, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C3", 0, ResourceConsumer)
        GpioInt (Edge, ActiveHigh, Exclusive, PullDown, 0, "\\_SB.GPIO") { 75 }
      })
    }

    // BMG160 gyroscope: INT GPX3(1) = pin 9 (DRDY GPX3(4) = pin 12 unused).
    Device (GYR0)
    {
      Name (_HID, "EXYN500D")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        I2cSerialBus (0x68, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C3", 0, ResourceConsumer)
        GpioInt (Edge, ActiveHigh, Exclusive, PullDown, 0, "\\_SB.GPIO") { 9 }
      })
    }

    // AK8963C magnetometer: DRDY GPX3(3) = pin 11 (high while data is
    // ready, cleared by reading ST2).  UEFI releases its reset GPG2(0).
    Device (MAG0)
    {
      Name (_HID, "EXYN500E")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        I2cSerialBus (0x0C, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C3", 0, ResourceConsumer)
        GpioInt (Level, ActiveHigh, Exclusive, PullDown, 0, "\\_SB.GPIO") { 11 }
      })
    }

    // CM3323 RGBW light sensor: no interrupt line.
    Device (ALS0)
    {
      Name (_HID, "EXYN500F")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        I2cSerialBus (0x10, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C3", 0, ResourceConsumer)
      })
    }

    //
    // Mali-T628 MP6 (exynosgpu): registers 0x11800000 + 0x5000; job, MMU,
    // GPU interrupts SPI 219 / 74 / 117 (GSI 251 / 106 / 149, panfrost
    // order); vdd_g3d (MAX77802 BUCK4) through exynospmic (PMIC device).  The driver sets the
    // clock (VPLL -> aclk_g3d) and switches the G3D power domain itself.
    //
    Device (GPU0)
    {
      Name (_HID, "EXYN5013")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x11800000, 0x00005000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 251 }   // job
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 106 }   // MMU
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 149 }   // GPU
      })
    }

    //
    // MAX77802 PMIC (exynospmic): the only owner of its I2C connection (the
    // connection is exclusive); key backlight LDO23 (mxt1664s), vdd_g3d BUCK4
    // (exynosgpu) and the camera rails LDO18/19/24 (exynoscam) go through
    // its interface (ExynosWinDrivers/common/ExPmicIf.h).
    //
    Device (PMIC)
    {
      Name (_HID, "EXYN5016")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        I2cSerialBus (0x09, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.HSI3", 0, ResourceConsumer)
      })
    }

    //
    // Camera ISP (exynoscam): FIMC-IS 0x13000000 with its Cortex-A5 (MCUCTL at
    // +0x180000).  The A5's mailbox interrupt goes through combiner group 19
    // input 1 (ARMISP) = SPI 19 = GSI 51, verified by CameraTest.efi; the
    // combiner group is shared, the driver enables only its input.  PMU, CMU
    // and SYSREG are shared windows the driver maps itself.
    //
    Device (CAM0)
    {
      Name (_HID, "EXYN5015")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x13000000, 0x001A0000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 51 }
      })
    }

    //
    // Internal display (exynosdisp, WDDM display-only): LSL122DL01 eDP panel
    // on FIMD1 0x14400000 + DP1 0x145B0000, LP8556 backlight on PWM0 and the
    // bit-banged I2C GPA2(4)/(5).  The driver maps its blocks itself (the
    // GPIO / PMU / CMU windows are shared with other devices) and brings the
    // panel up from cold; scanout is UEFI's GOP framebuffer.
    //
    Device (DISP)
    {
      Name (_HID, "EXYN5010")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      // FIMD1 frame interrupt for exynosdisp2: combiner group 18 (SPI 18 =
      // GSI 50), input 5 enabled by the driver.  exynosdisp (v1) leaves it off.
      Name (_CRS, ResourceTemplate () {
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 50 }
      })
    }

    //
    // Cover (hall sensor GPX0(1) = pin 65, low while the cover's magnet is
    // there) as the ACPI lid: GPIO-signalled ACPI events on the GPIO
    // controller (_AEI, both edges) notify LID0, whose _LID reads the pin
    // through the GPIO operation region (GpioClx).  Windows turns the
    // screen off / locks on close and wakes on open.
    //
    Scope (\_SB.GPIO)
    {
      Name (_AEI, ResourceTemplate () {
        GpioInt (Edge, ActiveBoth, SharedAndWake, PullUp, 0, "\\_SB.GPIO") { 65 }
      })

      Method (_EVT, 1, Serialized)
      {
        If (LEqual (Arg0, 65)) {
          Notify (\_SB.LID0, 0x80)
        }
      }

      OperationRegion (GPOR, GeneralPurposeIo, Zero, One)
      Field (GPOR, ByteAcc, NoLock, Preserve)
      {
        Connection (GpioIo (Shared, PullUp, 0, 0, IoRestrictionInputOnly, "\\_SB.GPIO") { 65 }),
        HALL, 1,
      }

      Name (GPAV, Zero)                 // GPIO operation region usable
      Method (_REG, 2)
      {
        If (LEqual (Arg0, 8)) {         // GeneralPurposeIo
          Store (Arg1, GPAV)
        }
      }
    }

    Device (LID0)
    {
      Name (_HID, "PNP0C0D")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Method (_LID, 0)
      {
        If (LEqual (\_SB.GPIO.GPAV, One)) {
          Return (\_SB.GPIO.HALL)      // 1: open, 0: cover closed
        }
        Return (One)
      }
    }
  }

#if EXYNOS_THERMAL
  //
  // Thermal zones on the Exynos5420 TMU (started by TmuDxe): one-point trim,
  // T = code - trim + 25 (°C), codes 31..146; trim = TRIMINFO[7:0], 55 if
  // outside 16..76 or TRIMINFO[15:8] != 0 (Samsung exynos_thermal.c).  The
  // 5420 trim ring: CPU2 uses block 3's TRIMINFO, CPU3 block 4's, the GPU
  // block 2's.  Units: tenths of a kelvin.  Passive cooling acts through the
  // processors' performance states (exynospep); 115 °C is the TMU's own
  // hardware trip.
  //
  Scope (\_TZ)
  {
    OperationRegion (TMU0, SystemMemory, 0x10060000, 0x44)
    Field (TMU0, DWordAcc, NoLock, Preserve) { TI0, 32, Offset (0x40), CT0, 32 }
    OperationRegion (TMU1, SystemMemory, 0x10064000, 0x44)
    Field (TMU1, DWordAcc, NoLock, Preserve) { TI1, 32, Offset (0x40), CT1, 32 }
    OperationRegion (TMU2, SystemMemory, 0x10068000, 0x44)
    Field (TMU2, DWordAcc, NoLock, Preserve) { TI2, 32, Offset (0x40), CT2, 32 }
    OperationRegion (TMU3, SystemMemory, 0x1006C000, 0x44)
    Field (TMU3, DWordAcc, NoLock, Preserve) { TI3, 32, Offset (0x40), CT3, 32 }
    OperationRegion (TMU4, SystemMemory, 0x100A0000, 0x44)
    Field (TMU4, DWordAcc, NoLock, Preserve) { TI4, 32, Offset (0x40), CT4, 32 }

    // Arg0 TRIMINFO -> trim
    Method (TRMV, 1, Serialized)
    {
      Local0 = (Arg0 & 0xFF)
      If (((Local0 < 16) || (Local0 > 76)) || (((Arg0 >> 8) & 0xFF) != 0)) {
        Local0 = 55
      }
      Return (Local0)
    }

    // Arg0 CURRENT_TEMP, Arg1 TRIMINFO -> tenths of a kelvin
    Method (TEMP, 2, Serialized)
    {
      Local0 = (Arg0 & 0xFF)
      If ((Local0 < 31) || (Local0 > 146)) {
        Return (2982)                  // no reading: report 25 °C
      }
      Local0 += 25
      Local1 = TRMV (Arg1)
      If (Local0 < Local1) {
        Return (2732)
      }
      Return (((Local0 - Local1) * 10) + 2732)
    }

    ThermalZone (TZ00)                 // A15 cluster: the hottest of CPU0..3
    {
      Method (_TMP, 0, Serialized)
      {
        Local0 = TEMP (CT0, TI0)
        Local1 = TEMP (CT1, TI1)
        If (Local1 > Local0) { Local0 = Local1 }
        Local1 = TEMP (CT2, TI3)
        If (Local1 > Local0) { Local0 = Local1 }
        Local1 = TEMP (CT3, TI4)
        If (Local1 > Local0) { Local0 = Local1 }
        Return (Local0)
      }
      Name (_PSV, 3632)                // 90 °C (Samsung trigger level 1)
      Name (_PSL, Package () { \_SB.CPU0, \_SB.CPU1, \_SB.CPU2, \_SB.CPU3 })
      Name (_TC1, 2)
      Name (_TC2, 3)
      Name (_TSP, 10)                  // 1 s while passive
      Name (_CRT, 3832)                // 110 °C (trigger level 2): orderly shutdown
      Name (_TZP, 30)                  // poll every 3 s
      Name (_STR, Unicode ("CPU (A15 cluster)"))
    }

    ThermalZone (TZ01)                 // GPU
    {
      Method (_TMP, 0, Serialized)
      {
        Return (TEMP (CT4, TI2))
      }
      Name (_CRT, 3832)
      Name (_TZP, 30)
      Name (_STR, Unicode ("GPU"))
    }
  }
#endif
}
