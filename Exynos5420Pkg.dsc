#
#  Exynos5420Pkg platform description - Samsung Galaxy Tab Pro / Note Pro
#  12.2 WiFi (codename v1awifi, board universal5420), Exynos 5420.
#
#  Milestone 2: PrePi + SimpleFb + framebuffer console + BDS.  The only
#  Exynos-specific driver is SimpleFbDxe (the bootloader already programmed
#  FIMD1 + DSIM + the panel; we just read it back and paint).  Keypad, touch,
#  storage, USB, clock, GPIO and I2C come in later milestones.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = Exynos5420
  PLATFORM_GUID                  = 5ec0f4a1-7b3d-4c62-9a05-542000000441
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = ARM
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Exynos5420Pkg/Exynos5420Pkg.fdf

  # DEBUG() goes to the screen by default: milestone 2 watches the panel.
  # build.sh can flip these to use the Exynos UART2 (ttySAC2 @ 0x12C20000).
  DEFINE USE_UART_FOR_SERIAL_OUTPUT = 0
  DEFINE USE_SCREEN_FOR_SERIAL_OUTPUT = 1

!include Exynos5420Pkg/CommonDsc.dsc.inc

################################################################################
#
# Components Section - list of all EDK II Modules needed by this Platform
#
################################################################################
[Components.common]
  #
  # SEC phase: the PrePi that GenFv's reset vector branches into.
  #
  Exynos5420Pkg/PrePi/PeiUniCore.inf

  #
  # DXE Core
  #
  MdeModulePkg/Core/Dxe/DxeMain.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
      NULL|MdeModulePkg/Library/DxeCrc32GuidedSectionExtractLib/DxeCrc32GuidedSectionExtractLib.inf
  }

  #
  # Architectural Protocols
  #
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf

  #
  # Timer: MCT global comparator 0 -> combiner 23.3 -> GIC INTID 55.  Gives the
  # DXE Core a real tick so timed events (boot timeout, Stall, SetTimer)
  # work.
  #
  Exynos5420Pkg/Drivers/MctTimerDxe/MctTimerDxe.inf

  #
  # Keypad / GPIO / I2C (v1awifi buttons: power + vol up/down + home).
  #
  Exynos5420Pkg/Drivers/GpioDxe/GpioDxe.inf
  Exynos5420Pkg/Drivers/I2CDxe/I2CDxe.inf
  Exynos5420Pkg/Drivers/SpiDxe/SpiDxe.inf
  Exynos5420Pkg/Drivers/SensorDxe/SensorDxe.inf
  Exynos5420Pkg/Drivers/CpuFreqDxe/CpuFreqDxe.inf
  Exynos5420Pkg/Drivers/BatteryDxe/BatteryDxe.inf
  Exynos5420Pkg/Drivers/MpParkDxe/MpParkDxe.inf
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf
  Exynos5420Pkg/Drivers/SmbiosPlatformDxe/SmbiosPlatformDxe.inf
  Exynos5420Pkg/Drivers/GenericKeypadDeviceDxe/GenericKeypadDeviceDxe.inf
  Exynos5420Pkg/Drivers/KeypadDxe/KeypadDxe.inf

  #
  # Touchscreen (mXT1664S, I2C0) + S Pen (Wacom G9PLL, hs-i2c4) as one
  # EFI_ABSOLUTE_POINTER_PROTOCOL.
  #
  Exynos5420Pkg/Drivers/TouchDxe/TouchDxe.inf

  #
  # Touch test app (not in the FD: copy TouchTest.efi to a FAT volume).
  #
  Exynos5420Pkg/Application/TouchTestApp/TouchTestApp.inf
  Exynos5420Pkg/Application/BatteryTestApp/BatteryTestApp.inf
  Exynos5420Pkg/Application/WlanTestApp/WlanTestApp.inf
  Exynos5420Pkg/Application/AudioTestApp/AudioTestApp.inf
  Exynos5420Pkg/Application/SensorTestApp/SensorTestApp.inf
  Exynos5420Pkg/Application/DisplayTestApp/DisplayTestApp.inf

  #
  # ACPI for Windows: FADT, MADT, empty DSDT, CSRT (MCT timer HAL extension).
  #
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  MdeModulePkg/Universal/Acpi/AcpiPlatformDxe/AcpiPlatformDxe.inf
  Exynos5420Pkg/AcpiTables/AcpiTables.inf

  #
  # Storage: eMMC (dw_mmc ch0) + microSD (dw_mmc ch2, only when the card-detect
  # pin reports a card), then the disk stack (partition + FAT) on BlockIo.
  #
  Exynos5420Pkg/Drivers/SdCardDxe/SdCardDxe.inf
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf

  #
  # USB peripheral (device mode only): EFI_USBFN_IO_PROTOCOL over the DWC3
  # USB3.0 DRD0 at 0x12000000.  UsbLogDxe streams the boot log ring to a host PC.
  #
  Exynos5420Pkg/Drivers/UsbFnDxe/UsbFnDxe.inf
  Exynos5420Pkg/Drivers/UsbLogDxe/UsbLogDxe.inf

  #
  # Interrupt controller (GIC-400 at 0x10481000/0x10482000).
  #
  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf

  #
  # PCD runtime driver for the dynamic PCDs (video mode / boot timeout)
  #
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  #
  # Consoles: SimpleFbDxe publishes the bootloader's framebuffer as a GOP,
  # ConSplitter wires it into ConIn/ConOut, GraphicsConsole draws text.
  #
  Exynos5420Pkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  #
  # Variables (RAM only for now)
  #
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf

  #
  # Status code plumbing + BDS
  #
  MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf
  MdeModulePkg/Universal/DriverHealthManagerDxe/DriverHealthManagerDxe.inf
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }

  #
  # UEFI Shell.
  #
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellInstall1CommandsLib/UefiShellInstall1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellAcpiViewCommandLib/UefiShellAcpiViewCommandLib.inf
      HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
      PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
      BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0xFF
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
      gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|8000
  }
