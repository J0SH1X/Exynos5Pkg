/** @file
  v1awifi Bluetooth bring-up test (shell application): the Broadcom
  BCM4350-family controller on UART0, polled, to find out what the chip
  needs before the Windows drivers (exynosuart / exynosbt) do it.

  Per configuration (BT_WAKE low / high, WLAN_EN off / on): BT_EN power
  cycle, CTS watched for 1 s (the controller signals "ready" by asserting
  it), HCI_Reset sent with and without hardware flow control, the answer
  dumped.  With an answer: Read_Local_Name, Read_Local_Version and, if the
  patch file is on the boot volume, the patch download (115200 baud).

    BtTest.efi [patch.hcd]

  UART0 0x12C00000, clock as exynosuart sets it (UART2's source, divider
  for 3 Mbaud); pins GPA0[0..3] function 2; BT_EN GPG0(7), BT_WAKE
  GPX3(2), HOST_WAKE GPX2(1), WLAN_EN GPY7(7).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Protocol/ExynosGpio.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#define UART       0x12C00000
#define ULCON      0x00
#define UCON       0x04
#define UFCON      0x08
#define UMCON      0x0C
#define UTRSTAT    0x10
#define UERSTAT    0x14
#define UFSTAT     0x18
#define UMSTAT     0x1C
#define UTXH       0x20
#define URXH       0x24
#define UBRDIV     0x28
#define UFRACVAL   0x2C
#define UINTM      0x38

#define CMU_TOP              0x10020000
#define SRC_PERIC0           0x250
#define DIV_PERIC0           0x558
#define GATE_TOP_SCLK_PERIC  0x850
#define GATE_IP_PERIC        0x950

#define PIN_BT_EN      EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPG0, 7)
#define PIN_BT_WAKE    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX3, 2)
#define PIN_HOST_WAKE  EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX2, 1)
#define PIN_WLAN_EN    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPY7, 7)

#define LOG(...)  do { DEBUG ((DEBUG_INFO, __VA_ARGS__)); } while (0)

STATIC EXYNOS_GPIO_PROTOCOL  *mGpio;
STATIC UINT32                mSclk;
STATIC CONST UINT16          mPllCon0[8] = { 0, 0x120, 0x128, 0x180, 0x160, 0x150, 0x130, 0x140 };

STATIC
UINT32
PllHz (
  UINT32  Con
  )
{
  UINT32  P = (Con >> 8) & 0x3F;

  return (P == 0) ? 0 : (UINT32)(((UINT64)24000000 * ((Con >> 16) & 0x3FF)) / ((UINT64)P << (Con & 7)));
}

STATIC
VOID
UartClock (
  VOID
  )
{
  UINT32  Src = MmioRead32 (CMU_TOP + SRC_PERIC0);
  UINT32  Mux = (Src >> 12) & 7;
  UINT32  Parent = (Mux == 0) ? 24000000 : PllHz (MmioRead32 (CMU_TOP + mPllCon0[Mux]));
  UINT32  Div = 3;                       // 532 MHz MPLL / 4 = 133 MHz, as exynosuart picked

  if (Parent / (Div + 1) > 150000000) {
    Div = Parent / 150000000;
  }
  MmioWrite32 (CMU_TOP + SRC_PERIC0, (Src & ~(7u << 4)) | (Mux << 4));
  MmioAndThenOr32 (CMU_TOP + DIV_PERIC0, ~(0xFu << 8), Div << 8);
  MmioOr32 (CMU_TOP + GATE_TOP_SCLK_PERIC, 1);
  MmioOr32 (CMU_TOP + GATE_IP_PERIC, 1);
  mSclk = Parent / (Div + 1);
  LOG ("BtTest: UART0 clock mux %u parent %u Hz / %u = %u Hz\n", Mux, Parent, Div + 1, mSclk);
}

STATIC
VOID
UartBaud (
  UINT32  Baud
  )
{
  UINT32  Sixteenths = (mSclk + Baud / 2) / Baud;

  MmioWrite32 (UART + UBRDIV, Sixteenths / 16 - 1);
  MmioWrite32 (UART + UFRACVAL, Sixteenths % 16);
}

STATIC
VOID
UartInit (
  UINT32   Baud,
  BOOLEAN  Afc
  )
{
  UINTN  Pin;

  for (Pin = 0; Pin < 4; Pin++) {
    mGpio->Config (mGpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPA0, Pin), 2, EXYNOS_GPIO_PULL_NONE);
  }
  MmioWrite32 (UART + UINTM, 0xF);
  MmioWrite32 (UART + UFCON, 0x7);       // FIFOs on, both reset
  MmioWrite32 (UART + ULCON, 0x3);       // 8N1
  MmioWrite32 (UART + UCON, 0x5);        // RX / TX polled
  UartBaud (Baud);
  MmioWrite32 (UART + UMCON, Afc ? ((1u << 4) | (3u << 5)) : 1u);   // AFC, or nRTS asserted by hand
}

STATIC
VOID
DumpPins (
  CONST CHAR8  *When
  )
{
  STATIC CONST struct { UINT32 Base; CONST CHAR8 *Name; } Banks[] = {
    { 0x14000080, "GPG0" }, { 0x13400C60, "GPX3" }, { 0x13400C40, "GPX2" }, { 0x14010000, "GPA0" },
    { 0x13400000 + 0x000 + 0x0, NULL }
  };
  UINTN  Index;

  LOG ("BtTest: [%a] BT_EN %u BT_WAKE %u HOST_WAKE %u WLAN_EN %u, UMSTAT 0x%x UFSTAT 0x%x UERSTAT 0x%x\n", When,
       mGpio->Get (mGpio, PIN_BT_EN), mGpio->Get (mGpio, PIN_BT_WAKE), mGpio->Get (mGpio, PIN_HOST_WAKE),
       mGpio->Get (mGpio, PIN_WLAN_EN), MmioRead32 (UART + UMSTAT), MmioRead32 (UART + UFSTAT),
       MmioRead32 (UART + UERSTAT));
  for (Index = 0; Banks[Index].Name != NULL; Index++) {
    LOG ("BtTest:   %a CON 0x%08x DAT 0x%02x PUD 0x%04x\n", Banks[Index].Name, MmioRead32 (Banks[Index].Base),
         MmioRead32 (Banks[Index].Base + 4), MmioRead32 (Banks[Index].Base + 8));
  }
}

STATIC
UINTN
RxCount (
  VOID
  )
{
  UINT32  Stat = MmioRead32 (UART + UFSTAT);

  return ((Stat & (1u << 8)) != 0) ? 256 : (Stat & 0xFF);
}

STATIC
VOID
Drain (
  VOID
  )
{
  while (RxCount () != 0) {
    (VOID)MmioRead32 (UART + URXH);
  }
}

STATIC
BOOLEAN
Send (
  CONST UINT8  *Data,
  UINTN        Length,
  UINTN        TimeoutMs
  )
{
  UINTN  Index;
  UINTN  Waited;

  for (Index = 0; Index < Length; Index++) {
    for (Waited = 0; (MmioRead32 (UART + UFSTAT) & (1u << 24)) != 0; Waited++) {
      if (Waited > TimeoutMs * 10) {
        return FALSE;
      }
      MicroSecondDelay (100);
    }
    MmioWrite32 (UART + UTXH, Data[Index]);
  }
  // Wait for the shifter to empty (TX FIFO and shift register, UTRSTAT bit 2).
  for (Waited = 0; (MmioRead32 (UART + UTRSTAT) & 4) == 0; Waited++) {
    if (Waited > TimeoutMs * 10) {
      return FALSE;
    }
    MicroSecondDelay (100);
  }
  return TRUE;
}

STATIC
UINTN
Receive (
  UINT8  *Data,
  UINTN  Want,
  UINTN  TimeoutMs
  )
{
  UINTN  Got = 0;
  UINTN  Idle = 0;

  while ((Got < Want) && (Idle < TimeoutMs * 10)) {
    if (RxCount () != 0) {
      Data[Got++] = (UINT8)MmioRead32 (UART + URXH);
      Idle = 0;
    } else {
      MicroSecondDelay (100);
      Idle++;
    }
  }
  return Got;
}

STATIC
VOID
Hex (
  CONST CHAR8  *What,
  CONST UINT8  *Data,
  UINTN        Length
  )
{
  CHAR8  Line[3 * 48 + 1];
  UINTN  Index;

  for (Index = 0; (Index < Length) && (Index < 48); Index++) {
    AsciiSPrint (Line + 3 * Index, 4, "%02x ", Data[Index]);
  }
  Line[3 * Index] = 0;
  LOG ("BtTest: %a (%u bytes): %a\n", What, Length, Line);
}

/**
  HCI command, then read one event (H4 header first, then the rest).
**/
STATIC
INTN
Command (
  UINT16       Opcode,
  CONST UINT8  *Params,
  UINT8        Length,
  UINT8        *Event,
  UINTN        TimeoutMs,
  BOOLEAN      Quiet
  )
{
  UINT8  Cmd[4 + 255];
  UINTN  Got;

  Cmd[0] = 1;
  Cmd[1] = (UINT8)Opcode;
  Cmd[2] = (UINT8)(Opcode >> 8);
  Cmd[3] = Length;
  CopyMem (Cmd + 4, Params, Length);
  if (!Send (Cmd, 4 + Length, 500)) {
    LOG ("BtTest: command 0x%04x: TX stuck (UMSTAT 0x%x UFSTAT 0x%x)\n", Opcode, MmioRead32 (UART + UMSTAT),
         MmioRead32 (UART + UFSTAT));
    return -1;
  }
  Got = Receive (Event, 3, TimeoutMs);
  if ((Got == 3) && (Event[0] == 4)) {
    Got += Receive (Event + 3, Event[2], 100);
  } else if (Got != 0) {
    Got += Receive (Event + Got, 64, 50);
  }
  if (!Quiet || (Got == 0)) {
    Hex ("answer", Event, Got);
  }
  return (INTN)Got;
}

STATIC
EFI_STATUS
LoadFile (
  EFI_HANDLE   ImageHandle,
  CONST CHAR16 *Name,
  VOID         **Data,
  UINTN        *Size
  )
{
  EFI_LOADED_IMAGE_PROTOCOL        *Image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize;
  EFI_STATUS                       Status;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (Image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  }
  if (!EFI_ERROR (Status)) {
    Status = Fs->OpenVolume (Fs, &Root);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Root->Open (Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    InfoSize = SIZE_OF_EFI_FILE_INFO + 512;
    Info = AllocatePool (InfoSize);
    Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
    if (!EFI_ERROR (Status)) {
      *Size = (UINTN)Info->FileSize;
      *Data = AllocatePool (*Size);
      Status = File->Read (File, Size, *Data);
    }
    FreePool (Info);
    File->Close (File);
  }
  Root->Close (Root);
  return Status;
}

/**
  One power-up of the controller and a reset, in a given configuration.
  Returns TRUE when it answered.
**/
STATIC
BOOLEAN
TryConfig (
  CONST CHAR8  *Name,
  BOOLEAN      Wake,
  UINTN        Wlan,
  BOOLEAN      Afc,
  UINT32       Baud,
  BOOLEAN      WakeAfter,              // BT_WAKE raised only after BT_EN (exynosbt's order)
  BOOLEAN      RtsOffAtBoot            // nRTS deasserted (UMCON 0) while BT_EN rises
  )
{
  UINT8   Event[300];
  UINT32  Cts;
  UINT32  Last;
  UINTN   Ms;
  INTN    Got;

  LOG ("BtTest: ---- %a: BT_WAKE %u, WLAN_EN %u, flow control %a, %u baud\n", Name, Wake, Wlan, Afc ? "on" : "off",
       Baud);
  mGpio->Set (mGpio, PIN_BT_EN, 0);
  if (Wlan != 2) {                       // 2: WLAN_EN left as it is (Wi-Fi up)
    mGpio->Set (mGpio, PIN_WLAN_EN, Wlan ? 1 : 0);
  }
  mGpio->Set (mGpio, PIN_BT_WAKE, (Wake && !WakeAfter) ? 1 : 0);
  MicroSecondDelay (200 * 1000);
  UartInit (Baud, Afc);
  if (RtsOffAtBoot) {
    MmioWrite32 (UART + UMCON, 0);
  }
  DumpPins ("BT_EN low");

  mGpio->Set (mGpio, PIN_BT_EN, 1);
  if (WakeAfter) {
    mGpio->Set (mGpio, PIN_BT_WAKE, Wake ? 1 : 0);
  }
  if (RtsOffAtBoot) {
    MicroSecondDelay (5000);
    MmioWrite32 (UART + UMCON, Afc ? ((1u << 4) | (3u << 5)) : 1u);
  }
  Last = MmioRead32 (UART + UMSTAT) & 1;
  LOG ("BtTest: BT_EN high, CTS %u\n", Last);
  for (Ms = 0; Ms < 1000; Ms += 2) {
    MicroSecondDelay (2000);
    Cts = MmioRead32 (UART + UMSTAT) & 1;
    if (Cts != Last) {
      LOG ("BtTest:   CTS %u at %u ms\n", Cts, Ms);
      Last = Cts;
    }
  }
  if (RxCount () != 0) {
    Got = (INTN)Receive (Event, sizeof (Event), 20);
    Hex ("bytes after power-up", Event, (UINTN)Got);
  }
  DumpPins ("BT_EN high, 1 s");

  Drain ();
  Got = Command (0x0C03, NULL, 0, Event, 1000, FALSE);
  if ((Got >= 7) && (Event[0] == 4) && (Event[1] == 0x0E)) {
    LOG ("BtTest: HCI_Reset answered (status 0x%02x)\n", Event[6]);
    return TRUE;
  }
  return FALSE;
}

EFI_STATUS
EFIAPI
BtTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT8       Event[300];
  INTN        Got;
  BOOLEAN     Up;
  VOID        *Hcd;
  UINTN       HcdSize;
  UINTN       Offset;
  UINTN       Records;
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&mGpio);
  if (EFI_ERROR (Status)) {
    Print (L"no GPIO protocol\n");
    return Status;
  }
  LOG ("BtTest: start\n");
  mGpio->Config (mGpio, PIN_BT_EN, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  mGpio->Config (mGpio, PIN_BT_WAKE, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  mGpio->Config (mGpio, PIN_HOST_WAKE, EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_NONE);
  UartClock ();
  DumpPins ("start");

  //
  // W: Wi-Fi left running (WLAN_EN untouched, as in Windows with
  // exynoswlan up); A: the configuration that answered with Wi-Fi off.
  //
  mGpio->Config (mGpio, PIN_WLAN_EN, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  //
  // P: HOST_WAKE (GPX2(1)) with the pull-down Windows leaves on it (its
  // only difference from here); A: no pull, as before.
  //
  mGpio->Config (mGpio, PIN_HOST_WAKE, EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_DOWN);
  Up = TryConfig ("P HOST_WAKE pull-down", TRUE, 0, TRUE, 115200, TRUE, FALSE);
  mGpio->Config (mGpio, PIN_HOST_WAKE, EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_NONE);
  Up = TryConfig ("A", TRUE, 0, TRUE, 115200, FALSE, FALSE) || Up;
  if (!Up) {
    LOG ("BtTest: no answer in any configuration\n");
    mGpio->Set (mGpio, PIN_BT_EN, 0);
    return EFI_NOT_FOUND;
  }

  Command (0x0C14, NULL, 0, Event, 500, FALSE);
  if (Event[0] == 4) {
    LOG ("BtTest: local name \"%a\"\n", (CHAR8 *)Event + 7);
  }
  Command (0x1001, NULL, 0, Event, 500, FALSE);
  Command (0x1009, NULL, 0, Event, 500, FALSE);

  if (EFI_ERROR (LoadFile (ImageHandle, L"\\bcm4350_V0301.0591.hcd", &Hcd, &HcdSize))) {
    LOG ("BtTest: no patch file on the boot volume\n");
    return EFI_SUCCESS;
  }
  Got = Command (0xFC2E, NULL, 0, Event, 500, FALSE);
  MicroSecondDelay (50 * 1000);
  for (Offset = 0, Records = 0; Offset + 3 <= HcdSize; Offset += 3 + ((UINT8 *)Hcd)[Offset + 2], Records++) {
    UINT8  *R = (UINT8 *)Hcd + Offset;

    Got = Command (R[0] | (R[1] << 8), R + 3, R[2], Event, 500, TRUE);
    if ((Got < 7) || (Event[1] != 0x0E) || (Event[6] != 0)) {
      LOG ("BtTest: patch record %u failed\n", Records);
      Hex ("answer", Event, (UINTN)Got);
      break;
    }
  }
  LOG ("BtTest: patch: %u records of %u bytes\n", Records, HcdSize);
  MicroSecondDelay (250 * 1000);
  UartBaud (115200);
  Drain ();
  Command (0x0C03, NULL, 0, Event, 1000, FALSE);
  Command (0x1001, NULL, 0, Event, 500, FALSE);
  Command (0x0C14, NULL, 0, Event, 500, FALSE);
  if (Event[0] == 4) {
    LOG ("BtTest: local name after the patch \"%a\"\n", (CHAR8 *)Event + 7);
  }
  LOG ("BtTest: done\n");
  return EFI_SUCCESS;
}
