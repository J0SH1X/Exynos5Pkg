/** @file
  v1awifi (SM-P900) Wi-Fi test (UEFI shell application) over the shared
  Broadcom core (Drivers/BcmWlan/BcmSdio.c, the same code the Windows driver
  exynoswlan builds).  The chip is powered only while this runs.

    WlanTest.efi                    attach, firmware + NVRAM from this app's
                                    volume (\bcmdhd_sta.bin, \nvram_net.txt),
                                    start it, firmware version and MAC, radio up
    WlanTest.efi join SSID [PASS]   the same, then join (WPA2-PSK with PASS) and
                                    wait 20 s for the link events

  Board (board-universal5420-wlan.c, board-universal5420-mmc.c): WLAN_EN
  GPY7(7); dw_mmc channel 1 at 0x12210000 on GPC1(0,1,3-6) function 2
  (GPC1(2) is GPS_EN: left alone); sclk_dwmci1 from CPLL.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include "../../Drivers/BcmWlan/BcmSdio.h"

#include <Guid/FileInfo.h>

#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiLib.h>

#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosI2c.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#define EXYNOS5420_PA_DWMCI1  0x12210000
#define FIRMWARE_FILE         L"\\bcmdhd_sta.bin"
#define NVRAM_FILE            L"\\nvram_net.txt"

#define CMU_BASE            0x10010000
#define CLK_CPLL_CON0       (CMU_BASE + 0x10120)
#define CLK_SRC_FSYS        (CMU_BASE + 0x10244)
#define CLK_SRC_MASK_FSYS   (CMU_BASE + 0x10340)
#define CLK_DIV_FSYS1       (CMU_BASE + 0x1054C)
#define CLK_GATE_IP_FSYS    (CMU_BASE + 0x10944)
#define XXTI_HZ             24000000U
#define MUX_SRC_CPLL        1
#define MUX_SHIFT           12
#define DIV_SHIFT           10
#define GATE_BIT            13
#define DWMCI_SDR_TIMING    0x03040000U
#define CLKSEL_CIU_DIV(x)   ((((x) >> 24) & 0x7) + 1)
#define GPC1_DRV            (0x13410000 + 0x020 + 0x0C)
#define DRV_LV3             1

#define PMIC_I2C_BUS   7
#define PMIC_I2C_ADDR  0x09
#define PMIC_REG_32KHZ 0xB5

STATIC BCM_SDIO  mBcm;
STATIC BOOLEAN   mLinkUp;

STATIC
UINT32
PllRate (
  IN UINTN  Con0
  )
{
  UINT32  Reg;

  Reg = MmioRead32 (Con0);
  if (((Reg & BIT31) == 0) || (((Reg >> 8) & 0x3F) == 0)) {
    return 0;
  }
  return (UINT32)DivU64x32 (MultU64x32 (XXTI_HZ, (Reg >> 16) & 0x3FF), ((Reg >> 8) & 0x3F) << (Reg & 7));
}

/**
  sclk_dwmci1 from CPLL / 1 like the kernel; returns the CIU clock.
**/
STATIC
UINT32
ClockInit (
  VOID
  )
{
  UINT32  Rate;

  MmioOr32 (CLK_GATE_IP_FSYS, 1U << GATE_BIT);
  MmioOr32 (CLK_SRC_MASK_FSYS, 1U << MUX_SHIFT);
  Rate = PllRate (CLK_CPLL_CON0);
  if (Rate == 0) {
    return 0;
  }
  MmioAndThenOr32 (CLK_SRC_FSYS, ~(0x7U << MUX_SHIFT), MUX_SRC_CPLL << MUX_SHIFT);
  MmioAnd32 (CLK_DIV_FSYS1, ~(0x3FFU << DIV_SHIFT));
  return Rate / CLKSEL_CIU_DIV (DWMCI_SDR_TIMING);
}

STATIC
VOID
PinsInit (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio
  )
{
  UINTN  Pin;

  for (Pin = 0; Pin <= 6; Pin++) {
    if (Pin == 2) {
      continue;                   // GPS_EN
    }
    Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC1, Pin), 2,
                  (Pin == 0) ? EXYNOS_GPIO_PULL_NONE : EXYNOS_GPIO_PULL_UP);
    MmioAndThenOr32 (GPC1_DRV, ~(0x3U << (Pin * 2)), (UINT32)DRV_LV3 << (Pin * 2));
  }
}

STATIC
VOID
Ensure32k (
  VOID
  )
{
  EXYNOS_I2C_PROTOCOL  *I2c;
  UINT8                Value;
  UINT8                Buf[2];

  if (EFI_ERROR (gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&I2c))) {
    DEBUG ((DEBUG_INFO, "Wlan: no I2C protocol, 32 kHz clock not checked\n"));
    return;
  }
  if (I2c->Read (I2c, PMIC_I2C_BUS, PMIC_I2C_ADDR, PMIC_REG_32KHZ, &Value, 1) != 1) {
    DEBUG ((DEBUG_INFO, "Wlan: MAX77802 32 kHz register not readable\n"));
    return;
  }
  DEBUG ((DEBUG_INFO, "Wlan: MAX77802 32 kHz register 0x%02x (AP %a, CP %a)\n", Value,
          (Value & BIT0) ? "on" : "off", (Value & BIT1) ? "on" : "off"));
  if ((Value & (BIT0 | BIT1)) != (BIT0 | BIT1)) {
    Buf[0] = PMIC_REG_32KHZ;
    Buf[1] = (UINT8)(Value | BIT0 | BIT1);
    I2c->Write (I2c, PMIC_I2C_BUS, PMIC_I2C_ADDR, Buf, 2);
    I2c->Read (I2c, PMIC_I2C_BUS, PMIC_I2C_ADDR, PMIC_REG_32KHZ, &Value, 1);
    DEBUG ((DEBUG_INFO, "Wlan: 32 kHz register now 0x%02x\n", Value));
    gBS->Stall (10000);
  }
}

STATIC
VOID
WlanPower (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio,
  IN BOOLEAN               On
  )
{
  Gpio->Set (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPY7, 7), On ? 1 : 0);
  Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPY7, 7), EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
}

STATIC
EFI_STATUS
ReadFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Name,
  OUT UINT8              **Data,
  OUT UINT32             *Size
  )
{
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              Length;
  EFI_STATUS         Status;

  Status = Root->Open (Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  InfoSize = SIZE_OF_EFI_FILE_INFO + 256;
  Info     = AllocatePool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    File->Close (File);
    return Status;
  }
  Length = (UINTN)Info->FileSize;
  FreePool (Info);

  *Data = AllocateZeroPool (Length + 8);      // room for the padding callers add
  if (*Data == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = File->Read (File, &Length, *Data);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    FreePool (*Data);
    return Status;
  }
  *Size = (UINT32)Length;
  return EFI_SUCCESS;
}


STATIC
UINT32
GetBe32 (
  IN CONST UINT8  *P
  )
{
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) | ((UINT32)P[2] << 8) | P[3];
}

/**
  Events from the firmware (BDC payload = ether header + bcmeth header +
  big-endian wl_event_msg_t); data frames are only counted.
**/
STATIC
VOID
RxHandler (
  IN VOID    *Context,
  IN UINT8   Channel,
  IN UINT8   *Payload,
  IN UINT32  Length
  )
{
  UINT8   *E;
  UINT32  Type;
  UINT32  Status;
  UINT32  Reason;
  UINT16  Flags;

  if ((Channel != BCM_CHANNEL_EVENT) || (Length < BCM_EVENT_MSG_OFFSET + 40)) {
    return;
  }
  E      = Payload + BCM_EVENT_MSG_OFFSET;
  Flags  = (UINT16)((E[2] << 8) | E[3]);
  Type   = GetBe32 (E + 4);
  Status = GetBe32 (E + 8);
  Reason = GetBe32 (E + 12);
  DEBUG ((DEBUG_INFO, "Wlan: event %u status %u reason %u flags 0x%x\n", Type, Status, Reason, Flags));
  if (Type == WLC_E_LINK) {
    mLinkUp = (Flags & WLC_EVENT_MSG_LINK) != 0;
    if (!mLinkUp) {
      BcmWpaLinkDown (&mBcm);
    }
    DEBUG ((DEBUG_INFO, "Wlan: LINK %a\n", mLinkUp ? "UP" : "down"));
  } else if (Type == WLC_E_SET_SSID) {
    DEBUG ((DEBUG_INFO, "Wlan: join %a\n", (Status == 0) ? "succeeded" : "failed"));
  }
}

/**
  Argument N (0 = first after the image name) of a space separated command
  line, as ASCII into Out.
**/
STATIC
BOOLEAN
GetArg (
  IN  CONST CHAR16  *Line,
  IN  UINTN         N,
  OUT CHAR8         *Out,
  IN  UINTN         OutSize
  )
{
  UINTN  Index;
  UINTN  Arg;
  UINTN  Length;

  Arg = 0;
  for (Index = 0; Line[Index] != L'\0'; ) {
    while (Line[Index] == L' ') {
      Index++;
    }
    if (Line[Index] == L'\0') {
      break;
    }
    Length = 0;
    while ((Line[Index] != L'\0') && (Line[Index] != L' ')) {
      if ((Arg == N + 1) && (Length + 1 < OutSize)) {
        Out[Length++] = (CHAR8)Line[Index];
      }
      Index++;
    }
    if (Arg == N + 1) {
      Out[Length] = '\0';
      return TRUE;
    }
    Arg++;
  }
  return FALSE;
}

EFI_STATUS
EFIAPI
WlanTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EXYNOS_GPIO_PROTOCOL             *Gpio;
  EFI_LOADED_IMAGE_PROTOCOL        *Image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  UINT8                            *Firmware;
  UINT8                            *Nvram;
  UINT32                           FirmwareSize;
  UINT32                           NvramSize;
  UINT32                           CiuHz;
  CHAR8                            Ver[128];
  UINT8                            Mac[6];
  CHAR8                            Mode[8];
  BOOLEAN                          Keep;
  CHAR8                            Ssid[40];
  CHAR8                            Pass[72];
  BOOLEAN                          Join;
  UINTN                            Wait;
  EFI_STATUS                       Status;

  Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&Gpio);
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Join    = FALSE;
  Keep    = FALSE;
  Pass[0] = '\0';
  // "keep": leave the chip powered with its firmware running (for tests of
  // the combo chip's Bluetooth side next to a live Wi-Fi side).
  if ((Image->LoadOptions != NULL) && GetArg (Image->LoadOptions, 0, Mode, sizeof (Mode)) &&
      (AsciiStrCmp (Mode, "keep") == 0)) {
    Keep = TRUE;
  }
  if ((Image->LoadOptions != NULL) && GetArg (Image->LoadOptions, 0, Mode, sizeof (Mode)) &&
      (AsciiStrCmp (Mode, "join") == 0) && GetArg (Image->LoadOptions, 1, Ssid, sizeof (Ssid))) {
    Join = TRUE;
    GetArg (Image->LoadOptions, 2, Pass, sizeof (Pass));
  }

  Status = gBS->HandleProtocol (Image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (!EFI_ERROR (Status)) {
    Status = Fs->OpenVolume (Fs, &Root);
  }
  if (!EFI_ERROR (Status)) {
    Status = ReadFile (Root, FIRMWARE_FILE, &Firmware, &FirmwareSize);
  }
  if (!EFI_ERROR (Status)) {
    Status = ReadFile (Root, NVRAM_FILE, &Nvram, &NvramSize);
  }
  if (EFI_ERROR (Status)) {
    Print (L"WlanTest: firmware / NVRAM files - %r\n", Status);
    return Status;
  }

  PinsInit (Gpio);
  Ensure32k ();
  WlanPower (Gpio, TRUE);
  gBS->Stall (200 * 1000);

  CiuHz = ClockInit ();
  ZeroMem (&mBcm, sizeof (mBcm));
  mBcm.RxHandler = RxHandler;
  Status = (CiuHz != 0) ? BcmSdioAttach (&mBcm, EXYNOS5420_PA_DWMCI1, CiuHz) : EFI_DEVICE_ERROR;
  if (!EFI_ERROR (Status)) {
    Status = BcmSdioDownload (&mBcm, Firmware, FirmwareSize, (CONST CHAR8 *)Nvram, NvramSize);
  }
  if (!EFI_ERROR (Status)) {
    Status = BcmSdioBusInit (&mBcm);
  }
  if (!EFI_ERROR (Status)) {
    ZeroMem (Ver, sizeof (Ver));
    Status = BcmIovarGet (&mBcm, "ver", Ver, sizeof (Ver) - 1);
    DEBUG ((DEBUG_INFO, "Wlan: ver - %r: %a\n", Status, Ver));
    ZeroMem (Mac, sizeof (Mac));
    Status = BcmIovarGet (&mBcm, "cur_etheraddr", Mac, sizeof (Mac));
    DEBUG ((DEBUG_INFO, "Wlan: MAC - %r: %02x:%02x:%02x:%02x:%02x:%02x\n", Status,
            Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]));
  }
  if (!EFI_ERROR (Status)) {
    Status = BcmIoctl (&mBcm, WLC_UP, TRUE, NULL, 0, 2000);
    DEBUG ((DEBUG_INFO, "Wlan: WLC_UP - %r\n", Status));
  }
  if (!EFI_ERROR (Status) && Join) {
    Status = BcmJoin (&mBcm, Ssid, Pass);
    DEBUG ((DEBUG_INFO, "Wlan: join \"%a\" (%a) - %r\n", Ssid, (Pass[0] != '\0') ? "WPA2-PSK" : "open", Status));
    for (Wait = 0; !EFI_ERROR (Status) && (Wait < 20000) && !(mLinkUp && (!mBcm.WpaActive || mBcm.WpaDone)); Wait += 10) {
      BcmSdioPoll (&mBcm);
      BcmWpaService (&mBcm);
      gBS->Stall (10 * 1000);
    }
    DEBUG ((DEBUG_INFO, "Wlan: link %a%a after %u ms\n", mLinkUp ? "up" : "not up",
            !mBcm.WpaActive ? "" : mBcm.WpaDone ? ", keys installed" : ", no keys", (UINT32)Wait));
  }
  BcmLogConsole (&mBcm);

  DEBUG ((DEBUG_INFO, "Wlan: finished - %r (%u frames, %u errors); chip %a\n", Status, mBcm.Frames, mBcm.Errors,
          Keep ? "left running" : "powered off"));
  Print (L"WlanTest: %r (details in the debug log)\n", Status);
  if (!Keep) {
    WlanPower (Gpio, FALSE);
  }
  return Status;
}
