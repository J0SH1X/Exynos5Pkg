/** @file
  PlatformBootManagerLib for Exynos5420Pkg.

  Registers the framebuffer GOP and the text consoles, adds the UEFI Shell
  from FVMAIN as a boot option, shows the platform splash and then a touch
  boot menu: Windows (default, after a countdown), the UEFI Shell, or a
  reboot into the Samsung bootloader's download or recovery mode.  Diagnostics go through DEBUG() only: ConOut does not
  exist yet at BeforeConsole time and Print() into it is what killed the
  Shell before.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootLogoLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FrameBufferConfigLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>
#include <Protocol/ExynosBattery.h>
#include <Protocol/ExynosGpio.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/KeypadDevice.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

//
// Title registered for the Shell boot option.  Also used to find the option
// again after RefreshAllBootOption() so we can boot it directly instead of
// trusting BootOrder.
//
#define SHELL_APP_TITLE  L"UEFI Shell"

//
// Boot menu.
//
#define MENU_TIMEOUT_S   5
#define MENU_POLL_MS     20

typedef enum {
  MenuWindows,
  MenuShell,
  MenuDownload,
  MenuRecovery,
  MenuCount
} MENU_ENTRY;

//
// Windows Boot Manager, in the order tried on every file system.
//
STATIC CONST CHAR16  *mWindowsLoaders[] = {
  L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
  L"\\EFI\\BOOT\\BOOTARM.EFI",
};

//
// Implemented in UefiBootManagerLib's BmConsole.c but not exported through
// UefiBootManagerLib.h.  HtcLeoPkg declares it the same way.
//
VOID
EFIAPI
EfiBootManagerConnectAllConsoles (
  VOID
  );

/**
  Locate all handles carrying ProtocolGuid, pass each to Process.
**/
STATIC
VOID
FilterAndProcess (
  IN EFI_GUID  *ProtocolGuid,
  IN VOID      (*Process)(EFI_HANDLE Handle)
  )
{
  EFI_STATUS   Status;
  EFI_HANDLE   *Handles;
  UINTN        NoHandles;
  UINTN        Idx;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol, ProtocolGuid, NULL, &NoHandles, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Idx = 0; Idx < NoHandles; Idx++) {
    Process (Handles[Idx]);
  }

  gBS->FreePool (Handles);
}

STATIC
VOID
EFIAPI
AddOutput (
  IN EFI_HANDLE  Handle
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  DevicePath = DevicePathFromHandle (Handle);
  if (DevicePath == NULL) {
    return;
  }

  EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, DevicePath, NULL);
}

STATIC
VOID
EFIAPI
AddInput (
  IN EFI_HANDLE  Handle
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  DevicePath = DevicePathFromHandle (Handle);
  if (DevicePath == NULL) {
    return;
  }

  EfiBootManagerUpdateConsoleVariable (ConIn, DevicePath, NULL);
}

/**
  Add a boot option pointing at a file inside our own firmware volume.

  @return The boot option number, or 0 if the file was not found.
**/
STATIC
UINTN
AddBootOptionForFile (
  IN CONST CHAR16  *Description,
  IN EFI_GUID      *FileGuid
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL         *DevicePath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH FileNode;
  EFI_BOOT_MANAGER_LOAD_OPTION     NewOption;
  EFI_BOOT_MANAGER_LOAD_OPTION     *Options;
  UINTN                            OptionCount;
  INT32                            OptionNumber;

  // The FV that contains this driver (BdsDxe) also contains the Shell.
  Status = gBS->HandleProtocol (
                  gImageHandle, &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
  DevicePath = AppendDevicePathNode (
                 DevicePathFromHandle (LoadedImage->DeviceHandle),
                 (EFI_DEVICE_PATH_PROTOCOL *)&FileNode
                 );
  if (DevicePath == NULL) {
    return 0;
  }

  //
  // Register the option once.  Neither OvmfPkg's nor HtcLeoPkg's
  // PlatformBootManagerLib probes the file with LoadImage() first, and doing
  // so here just made the option silently disappear: the FV definitely holds
  // the Shell (its FILE_GUID is in FVMAIN.Fv), so a failing probe means the
  // probe is wrong, not the file.  What HtcLeoPkg does do is look the option
  // up first (PlatformRegisterBootOption) so a reboot does not stack a second
  // "UEFI Shell" entry onto BootOrder; same idea, inlined here.
  //
  Status = EfiBootManagerInitializeLoadOption (
             &NewOption,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE,
             (CHAR16 *)Description,
             DevicePath,
             NULL,
             0
             );
  FreePool (DevicePath);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
  OptionNumber = EfiBootManagerFindLoadOption (&NewOption, Options, OptionCount);
  if (OptionNumber >= 0) {
    DEBUG ((DEBUG_INFO, "%a: '%s' already registered as option %d\n",
            __FUNCTION__, Description, (INT32)OptionNumber));
    EfiBootManagerFreeLoadOptions (Options, OptionCount);
    EfiBootManagerFreeLoadOption (&NewOption);
    return (UINTN)OptionNumber;
  }

  Status = EfiBootManagerAddLoadOptionVariable (&NewOption, 0);
  DEBUG ((DEBUG_INFO, "%a: added %s: %r\n", __FUNCTION__, Description, Status));
  OptionNumber = (INT32)NewOption.OptionNumber;

  EfiBootManagerFreeLoadOptions (Options, OptionCount);
  EfiBootManagerFreeLoadOption (&NewOption);
  return EFI_ERROR (Status) ? 0 : (UINTN)OptionNumber;
}

/**
  Callback invoked each second while BDS counts down the boot timeout.
**/
VOID
EFIAPI
PlatformBootManagerWaitCallback (
  IN UINT16  TimeoutRemain
  )
{
  DEBUG ((DEBUG_INFO, "Boot in %u second(s)...\n", TimeoutRemain));
}

/**
  Called when BDS could not find anything to boot.
**/
VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "%a: no boot option succeeded\n", __FUNCTION__));
  //
  // Nothing else to do on this platform (no recovery media yet): spin so the
  // message stays on the screen instead of rebooting into a loop.
  //
  for (;;) {
  }
}

/**
  Platform boot manager before the console is initialized.
**/
VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  //
  // End of the platform's DXE phase.  Until this is signalled the security
  // handler defers every image that does not come from a firmware volume
  // ("3rd party image ... deferred to load before EndOfDxe"), so nothing on a
  // FAT volume, LinuxBoot.efi included, could be started from the Shell.
  //
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  //
  // Register the consoles here, not in AfterConsole.  BdsEntry() calls
  // EfiBootManagerConnectAllDefaultConsoles() between BeforeConsole and
  // AfterConsole, and that is what reads the ConOut variable and fills in
  // gST->ConOut.  Registering later leaves gST->ConOut NULL, which kills
  // Print() and leaves the Shell with nothing to draw on -- all output seen
  // before this point is DEBUG going straight to the framebuffer through
  // SerialPortLib, which never uses gST->ConOut.
  //
  // The GOP handle is what gets registered because SimpleFbDxe installs its
  // device path alongside the protocol; connecting that path is what makes
  // GraphicsConsoleDxe bind and install SimpleTextOut in the first place.
  //
  FilterAndProcess (&gEfiGraphicsOutputProtocolGuid, AddOutput);
  FilterAndProcess (&gEfiSimpleTextInProtocolGuid, AddInput);

  //
  // The keypad handle (GenericKeypadDeviceDxe's KEYPAD_DEVICE_PROTOCOL
  // instance) carries a device path, so adding it to ConIn makes
  // ConnectAllDefaultConsoles() connect it -- and KeypadDxe's driver
  // binding then produces SimpleTextIn on it.  Same wiring as HtcLeoPkg.
  //
  FilterAndProcess (&gEFIDroidKeypadDeviceProtocolGuid, AddInput);

  // The Shell has to exist before BDS sorts BootOrder.
  AddBootOptionForFile (SHELL_APP_TITLE, &gUefiShellFileGuid);
}

/**
  Boot the option registered under Description, directly.

  This deliberately does not consult BootOrder.  RefreshAllBootOption()
  discovers every bootable thing on every filesystem, including
  EFI\BOOT\BOOTARM.EFI on any FAT volume (an SD card, a USB stick), and
  those get sorted ahead of the Shell.  Picking the option by description
  and calling EfiBootManagerBoot() on it sidesteps that ordering entirely.

  @return TRUE if the option was found and EfiBootManagerBoot() returned.
**/
STATIC
BOOLEAN
BootOptionByDescription (
  IN CONST CHAR16  *Description
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                         Count;
  UINTN                         Index;
  BOOLEAN                       Found;

  Options = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  if (Options == NULL) {
    return FALSE;
  }

  Found = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (StrCmp (Options[Index].Description, Description) == 0) {
      EfiBootManagerBoot (&Options[Index]);
      //
      // If we get here the image loaded, started and gave control back,
      // which is a different bug from never being found.
      //
      Found = TRUE;
      break;
    }
  }

  EfiBootManagerFreeLoadOptions (Options, Count);
  return Found;
}

/**
  Boot an image from a device path once (no Boot#### variable), with
  OptionalData handed to it as its load options.

  @return Only if the image could not be started or returned.
**/
STATIC
VOID
BootDevicePath (
  IN CONST CHAR16              *Description,
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN CONST CHAR16              *OptionalData   OPTIONAL
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;
  EFI_STATUS                    Status;

  Status = EfiBootManagerInitializeLoadOption (
             &Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE,
             (CHAR16 *)Description,
             DevicePath,
             (UINT8 *)OptionalData,
             (OptionalData == NULL) ? 0 : (UINT32)StrSize (OptionalData)
             );
  if (EFI_ERROR (Status)) {
    return;
  }
  EfiBootManagerBoot (&Option);
  DEBUG ((DEBUG_ERROR, "%a: %s returned: %r\n", __FUNCTION__, Description, Option.Status));
  EfiBootManagerFreeLoadOption (&Option);
}

/**
  Windows Boot Manager from the first file system that has it.
**/
STATIC
VOID
BootWindows (
  VOID
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  UINTN                            Loader;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_DEVICE_PATH_PROTOCOL         *DevicePath;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                          NULL, &Count, &Handles))) {
    return;
  }

  for (Loader = 0; Loader < ARRAY_SIZE (mWindowsLoaders); Loader++) {
    for (Index = 0; Index < Count; Index++) {
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
          EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
        continue;
      }
      if (EFI_ERROR (Root->Open (Root, &File, (CHAR16 *)mWindowsLoaders[Loader], EFI_FILE_MODE_READ, 0))) {
        Root->Close (Root);
        continue;
      }
      File->Close (File);
      Root->Close (Root);

      DevicePath = FileDevicePath (Handles[Index], mWindowsLoaders[Loader]);
      if (DevicePath != NULL) {
        DEBUG ((DEBUG_INFO, "%a: %s on file system %u\n", __FUNCTION__, mWindowsLoaders[Loader], Index));
        BootDevicePath (L"Windows Boot Manager", DevicePath, NULL);
        FreePool (DevicePath);
      }
      FreePool (Handles);
      return;
    }
  }
  FreePool (Handles);
  DEBUG ((DEBUG_ERROR, "%a: no Windows Boot Manager on any file system\n", __FUNCTION__));
}

/**
  The UEFI Shell from FVMAIN; with NoStartup, without running startup.nsh.
**/
STATIC
VOID
BootShell (
  IN BOOLEAN  NoStartup
  )
{
  EFI_LOADED_IMAGE_PROTOCOL          *LoadedImage;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_DEVICE_PATH_PROTOCOL           *DevicePath;

  if (!NoStartup) {
    BootOptionByDescription (SHELL_APP_TITLE);
    return;
  }
  if (EFI_ERROR (gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage))) {
    return;
  }
  EfiInitializeFwVolDevicepathNode (&FileNode, &gUefiShellFileGuid);
  DevicePath = AppendDevicePathNode (DevicePathFromHandle (LoadedImage->DeviceHandle),
                                     (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  if (DevicePath == NULL) {
    return;
  }
  //
  // The Shell takes its options from the load options and skips argv[0].
  //
  BootDevicePath (SHELL_APP_TITLE, DevicePath, L"Shell -nostartup");
  FreePool (DevicePath);
}

//
// The menu is laid out on the text console's character grid: each button
// is rows of spaces in its background colour with the label centred, so
// label and button always match.  Only the selection frame is drawn with
// GOP, around the same cells.
//
typedef struct {
  UINTN  Column;
  UINTN  Row;
  UINTN  Columns;
  UINTN  Rows;
} CELL_RECT;

typedef struct {
  UINTN      ScreenWidth;      // pixels
  UINTN      ScreenHeight;
  UINTN      Columns;          // text grid
  UINTN      Rows;
  CELL_RECT  Button[MenuCount];
} MENU_LAYOUT;

STATIC CONST UINTN  mButtonAttribute[MenuCount] = {
  EFI_WHITE | EFI_BACKGROUND_BLUE,
  EFI_WHITE | EFI_BACKGROUND_GREEN,
  EFI_WHITE | EFI_BACKGROUND_MAGENTA,
  EFI_WHITE | EFI_BACKGROUND_RED,
};
STATIC CONST CHAR16  *mButtonLabel[MenuCount] = {
  L"Windows", L"UEFI Shell", L"Reboot to download mode", L"Reboot to recovery"
};
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mBlack     = { 0x00, 0x00, 0x00, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mHighlight = { 0xFF, 0xFF, 0xFF, 0 };

#define FRAME_PIXELS  6

/**
  Reboot into the Samsung bootloader's download or recovery mode
  (ExynosResetLib: PMU INFORM3 reboot mode for a "download" / "recovery"
  platform-specific reset).
**/
STATIC
VOID
RebootTo (
  IN CONST CHAR16  *Mode
  )
{
  gRT->ResetSystem (EfiResetPlatformSpecific, EFI_SUCCESS, StrSize (Mode), (VOID *)Mode);
}

STATIC
VOID
TextAt (
  IN UINTN         Column,
  IN UINTN         Row,
  IN UINTN         Attribute,
  IN CONST CHAR16  *Text
  )
{
  gST->ConOut->SetAttribute (gST->ConOut, Attribute);
  gST->ConOut->SetCursorPosition (gST->ConOut, Column, Row);
  gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Text);
}

/**
  One line centred in the grid, cleared to the full width first.
**/
STATIC
VOID
CentredLine (
  IN MENU_LAYOUT   *L,
  IN UINTN         Row,
  IN UINTN         Attribute,
  IN CONST CHAR16  *Text
  )
{
  CHAR16  Line[256];
  UINTN   Length;
  UINTN   Start;
  UINTN   Index;
  UINTN   Width;

  Width = MIN (L->Columns, ARRAY_SIZE (Line) - 1);
  for (Index = 0; Index < Width; Index++) {
    Line[Index] = L' ';
  }
  Line[Width] = L'\0';
  Length = MIN (StrLen (Text), Width);
  Start  = (Width - Length) / 2;
  CopyMem (&Line[Start], Text, Length * sizeof (CHAR16));

  //
  // The last cell of the last row would scroll the console: stop short.
  //
  if (Row + 1 >= L->Rows) {
    Line[Width - 1] = L'\0';
  }
  TextAt (0, Row, Attribute, Line);
}

STATIC
VOID
DrawButton (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop,
  IN MENU_LAYOUT                   *L,
  IN UINTN                         Index,
  IN BOOLEAN                       Selected
  )
{
  CELL_RECT                      *B;
  CHAR16                         Row[256];
  UINTN                          Cell;
  UINTN                          Line;
  UINTN                          Length;
  UINTN                          CellWidth;
  UINTN                          CellHeight;
  UINTN                          X;
  UINTN                          Y;
  UINTN                          W;
  UINTN                          H;

  B          = &L->Button[Index];
  CellWidth  = L->ScreenWidth / L->Columns;
  CellHeight = L->ScreenHeight / L->Rows;

  //
  // Frame: white around the selected button, black otherwise.
  //
  X = B->Column * CellWidth;
  Y = B->Row * CellHeight;
  W = B->Columns * CellWidth;
  H = B->Rows * CellHeight;
  if ((X >= FRAME_PIXELS) && (Y >= FRAME_PIXELS)) {
    Gop->Blt (Gop, Selected ? &mHighlight : &mBlack, EfiBltVideoFill, 0, 0,
              X - FRAME_PIXELS, Y - FRAME_PIXELS, W + 2 * FRAME_PIXELS, FRAME_PIXELS, 0);
    Gop->Blt (Gop, Selected ? &mHighlight : &mBlack, EfiBltVideoFill, 0, 0,
              X - FRAME_PIXELS, Y + H, W + 2 * FRAME_PIXELS, FRAME_PIXELS, 0);
    Gop->Blt (Gop, Selected ? &mHighlight : &mBlack, EfiBltVideoFill, 0, 0,
              X - FRAME_PIXELS, Y, FRAME_PIXELS, H, 0);
    Gop->Blt (Gop, Selected ? &mHighlight : &mBlack, EfiBltVideoFill, 0, 0,
              X + W, Y, FRAME_PIXELS, H, 0);
  }

  //
  // Body: rows of spaces, the label on the middle one.
  //
  Length = MIN (B->Columns, ARRAY_SIZE (Row) - 1);
  for (Line = 0; Line < B->Rows; Line++) {
    for (Cell = 0; Cell < Length; Cell++) {
      Row[Cell] = L' ';
    }
    Row[Length] = L'\0';
    if (Line == B->Rows / 2) {
      UINTN  LabelLength = MIN (StrLen (mButtonLabel[Index]), Length);

      CopyMem (&Row[(Length - LabelLength) / 2], mButtonLabel[Index], LabelLength * sizeof (CHAR16));
    }
    TextAt (B->Column, B->Row + Line, mButtonAttribute[Index], Row);
  }
}

STATIC
VOID
DrawMenu (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop,
  IN MENU_LAYOUT                   *L,
  IN MENU_ENTRY                    Selected,
  IN UINTN                         SecondsLeft
  )
{
  UINTN   Index;
  CHAR16  Line[96];

  for (Index = 0; Index < MenuCount; Index++) {
    DrawButton (Gop, L, Index, Index == (UINTN)Selected);
  }

  if (SecondsLeft > 0) {
    UnicodeSPrint (Line, sizeof (Line), L"%s in %u s - touch a button to choose", mButtonLabel[Selected], SecondsLeft);
  } else {
    UnicodeSPrint (Line, sizeof (Line), L"Touch a button to choose");
  }
  CentredLine (L, L->Button[MenuDownload].Row + L->Button[MenuDownload].Rows + 3,
               EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
  CentredLine (L, L->Button[MenuDownload].Row + L->Button[MenuDownload].Rows + 5,
               EFI_DARKGRAY | EFI_BACKGROUND_BLACK, L"volume up / down: select      power / home: start");
}

/**
  Four touch buttons on a 2 x 2 grid: Windows (default), UEFI Shell,
  reboot to download mode, reboot to recovery.  The countdown stops at the
  first touch or key.  Without GOP or console the default is taken.
**/
STATIC
MENU_ENTRY
BootMenu (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop;
  EFI_ABSOLUTE_POINTER_PROTOCOL   *Pointer;
  EFI_ABSOLUTE_POINTER_STATE      State;
  EFI_INPUT_KEY                   Key;
  MENU_LAYOUT                     L;
  MENU_ENTRY                      Selected;
  UINTN                           Index;
  UINTN                           Ticks;
  UINTN                           SecondsLeft;
  BOOLEAN                         Counting;
  UINTN                           Column;
  UINTN                           Row;
  UINTN                           ButtonColumns;
  UINTN                           ButtonRows;
  UINTN                           GapColumns;
  UINTN                           GapRows;
  UINTN                           Top;

  if ((gST->ConOut == NULL) || (gST->ConIn == NULL)) {
    return MenuWindows;
  }
  if (EFI_ERROR (gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop)) &&
      EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop))) {
    return MenuWindows;
  }
  if (EFI_ERROR (gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &L.Columns, &L.Rows)) ||
      (L.Columns < 40) || (L.Rows < 20)) {
    return MenuWindows;
  }
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&Pointer))) {
    Pointer = NULL;
  }
  L.ScreenWidth  = Gop->Mode->Info->HorizontalResolution;
  L.ScreenHeight = Gop->Mode->Info->VerticalResolution;

  //
  // 2 x 2 buttons, each a third of the width and a fifth of the height,
  // centred, with gaps of a twentieth.
  //
  ButtonColumns = L.Columns / 3;
  ButtonRows    = L.Rows / 5;
  GapColumns    = L.Columns / 20;
  GapRows       = L.Rows / 20 + 1;
  Top           = L.Rows / 6;
  for (Index = 0; Index < MenuCount; Index++) {
    L.Button[Index].Columns = ButtonColumns;
    L.Button[Index].Rows    = ButtonRows;
    L.Button[Index].Column  = (Index % 2 == 0) ? (L.Columns / 2 - GapColumns / 2 - ButtonColumns)
                                               : (L.Columns / 2 + (GapColumns + 1) / 2);
    L.Button[Index].Row     = Top + (Index / 2) * (ButtonRows + GapRows);
  }

  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
  gST->ConOut->ClearScreen (gST->ConOut);
  Gop->Blt (Gop, &mBlack, EfiBltVideoFill, 0, 0, 0, 0, L.ScreenWidth, L.ScreenHeight, 0);
  CentredLine (&L, Top - 3, EFI_WHITE | EFI_BACKGROUND_BLACK, L"Galaxy Note Pro 12.2 (SM-P900) - boot menu");

  Selected    = MenuWindows;
  Counting    = TRUE;
  SecondsLeft = MENU_TIMEOUT_S;
  DrawMenu (Gop, &L, Selected, SecondsLeft);

  for (Ticks = 0; ; Ticks++) {
    if (Counting && (Ticks > 0) && ((Ticks % (1000 / MENU_POLL_MS)) == 0)) {
      if (--SecondsLeft == 0) {
        break;
      }
      DrawMenu (Gop, &L, Selected, SecondsLeft);
    }

    //
    // Touch / pen: a tap on a button starts it.  Coordinates go to cells,
    // the grid the buttons are drawn on.
    //
    if ((Pointer != NULL) && !EFI_ERROR (Pointer->GetState (Pointer, &State)) &&
        ((State.ActiveButtons & EFI_ABSP_TouchActive) != 0)) {
      Column = (UINTN)(State.CurrentX * L.Columns / (Pointer->Mode->AbsoluteMaxX + 1));
      Row    = (UINTN)(State.CurrentY * L.Rows / (Pointer->Mode->AbsoluteMaxY + 1));
      for (Index = 0; Index < MenuCount; Index++) {
        if ((Column >= L.Button[Index].Column) && (Column < L.Button[Index].Column + L.Button[Index].Columns) &&
            (Row >= L.Button[Index].Row) && (Row < L.Button[Index].Row + L.Button[Index].Rows)) {
          Selected = (MENU_ENTRY)Index;
          DrawMenu (Gop, &L, Selected, 0);
          goto Done;
        }
      }
      if (Counting) {
        Counting = FALSE;
        DrawMenu (Gop, &L, Selected, 0);
      }
    }

    //
    // Keys: volume up / down move (SCAN_UP / SCAN_DOWN), power / home start.
    //
    if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      Counting = FALSE;
      if (Key.ScanCode == SCAN_UP) {
        Selected = (MENU_ENTRY)((Selected + MenuCount - 1) % MenuCount);
      } else if (Key.ScanCode == SCAN_DOWN) {
        Selected = (MENU_ENTRY)((Selected + 1) % MenuCount);
      } else {
        goto Done;
      }
      DrawMenu (Gop, &L, Selected, 0);
    }

    gBS->Stall (MENU_POLL_MS * 1000);
  }

Done:
  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, TRUE);
  DEBUG ((DEBUG_INFO, "BootMenu: %s\n", mButtonLabel[Selected]));
  return Selected;
}

//
// ---------------------------------------------------------------------------
// Low battery: charge mode.
//
// This firmware replaces the boot image that used to show Samsung's
// off-line charging screen, so it has to do that job itself: with the
// battery nearly empty, do not start Windows (it would drain the pack
// faster than a weak charger fills it, and a critical-battery shutdown with
// a charger plugged in comes straight back here).  Wait with the CPU
// asleep and the backlight off while BatteryDxe keeps the charger
// configured, until there is enough charge to boot.
//
// Thresholds (0.01 % / mV from the MAX17050): enter below 8 % or 3.30 V,
// leave at 15 % or 3.85 V (the voltage is the way out if the gauge lost
// its state in a deep discharge).  Without a cable the tablet powers off.
// Any key or a touch lights the screen for a few seconds.
// ---------------------------------------------------------------------------
//
#define CHARGE_ENTER_SOC    800
#define CHARGE_ENTER_MV     3300
#define CHARGE_EXIT_SOC     1500
#define CHARGE_EXIT_MV      3850
#define CHARGE_SCREEN_S     15
#define CHARGE_NOCABLE_S    10
#define BACKLIGHT_PIN       EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPG1, 1)    // LED_BL_RST

STATIC EXYNOS_GPIO_PROTOCOL  *mBacklightGpio;

/**
  Backlight enable (the kernel's v1_backlight_set_power(): GPG1(1)).  The
  level goes out before the pin becomes an output so turning it on does not
  glitch it off.
**/
STATIC
VOID
Backlight (
  IN BOOLEAN  On
  )
{
  if (mBacklightGpio == NULL) {
    return;
  }
  mBacklightGpio->Set (mBacklightGpio, BACKLIGHT_PIN, On ? 1 : 0);
  mBacklightGpio->Config (mBacklightGpio, BACKLIGHT_PIN, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
}

STATIC
VOID
ChargeScreen (
  IN CONST EXYNOS_BATTERY_STATUS  *S,
  IN CONST CHAR16                 *Message
  )
{
  UINT32  Temp;

  Temp = (UINT32)((S->DeciCelsius < 0) ? -S->DeciCelsius : S->DeciCelsius);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
  gST->ConOut->SetCursorPosition (gST->ConOut, 0, 0);
  Print (L"\n\n   %-60s\n\n", Message);
  Print (L"   Battery  %3u.%02u %%   %4u mV   %5d mA   %s%u.%u C      \n",
         S->SocCentiPercent / 100, S->SocCentiPercent % 100, S->MilliVolts, S->MilliAmps,
         (S->DeciCelsius < 0) ? L"-" : L"", Temp / 10, Temp % 10);
  Print (L"   Charger  %-6a input limit %4u mA   charge current %4u mA      \n",
         S->Cable, S->InputLimitMa, S->ChargeCurrentMa);
  Print (L"\n   Windows starts at %u %%.   Any key lights this screen.      \n", CHARGE_EXIT_SOC / 100);
}

/**
  Returns when the boot should go on: the battery is fine, it recovered, or
  the user insisted.  Does not return when the tablet powers off.
**/
STATIC
VOID
ChargeModeIfNeeded (
  VOID
  )
{
  EXYNOS_BATTERY_PROTOCOL        *Battery;
  EFI_ABSOLUTE_POINTER_STATE     Touch;
  EFI_ABSOLUTE_POINTER_PROTOCOL  *Pointer;
  EXYNOS_BATTERY_STATUS          S;
  EFI_INPUT_KEY                  Key;
  EFI_EVENT                      Tick;
  UINTN                          NoCable;
  UINTN                          ScreenLeft;
  BOOLEAN                        Activity;

  if (EFI_ERROR (gBS->LocateProtocol (&gExynosBatteryProtocolGuid, NULL, (VOID **)&Battery)) ||
      EFI_ERROR (Battery->GetStatus (Battery, &S))) {
    return;                       // no battery information: boot as usual
  }
  if ((S.SocCentiPercent >= CHARGE_ENTER_SOC) && (S.MilliVolts >= CHARGE_ENTER_MV)) {
    return;
  }
  if ((gST->ConOut == NULL) || (gST->ConIn == NULL)) {
    return;
  }

  DEBUG ((DEBUG_INFO, "ChargeMode: %u.%02u %% %u mV, cable %a - not booting yet\n",
          S.SocCentiPercent / 100, S.SocCentiPercent % 100, S.MilliVolts, S.Cable));

  gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&mBacklightGpio);
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&Pointer))) {
    Pointer = NULL;
  }
  if (EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &Tick)) ||
      EFI_ERROR (gBS->SetTimer (Tick, TimerPeriodic, 10000000))) {
    return;
  }

  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
  gST->ConOut->ClearScreen (gST->ConOut);
  ScreenLeft = CHARGE_SCREEN_S;
  NoCable    = 0;

  for ( ; ; ) {
    //
    // Asleep until the next interrupt (the 10 ms tick, or a key / touch).
    //
    ArmCallWFI ();

    Activity = FALSE;
    while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      Activity = TRUE;
      if (NoCable > 0) {
        goto BootAnyway;          // "press a key to boot anyway"
      }
    }
    if ((Pointer != NULL) && !EFI_ERROR (Pointer->GetState (Pointer, &Touch))) {
      Activity |= (Touch.ActiveButtons & EFI_ABSP_TouchActive) != 0;
    }
    if (Activity && (ScreenLeft == 0)) {
      Backlight (TRUE);
      gST->ConOut->ClearScreen (gST->ConOut);
    }
    if (Activity) {
      ScreenLeft = CHARGE_SCREEN_S;
    }

    if (gBS->CheckEvent (Tick) != EFI_SUCCESS) {
      continue;                   // once a second from here on
    }
    if (EFI_ERROR (Battery->GetStatus (Battery, &S))) {
      continue;
    }

    if ((S.SocCentiPercent >= CHARGE_EXIT_SOC) || (S.MilliVolts >= CHARGE_EXIT_MV)) {
      DEBUG ((DEBUG_INFO, "ChargeMode: %u.%02u %% %u mV - enough, booting\n",
              S.SocCentiPercent / 100, S.SocCentiPercent % 100, S.MilliVolts));
      break;
    }

    if (!S.ExternalPower) {
      NoCable++;
      if (NoCable == 1) {
        Backlight (TRUE);
        gST->ConOut->ClearScreen (gST->ConOut);
      }
      ScreenLeft = CHARGE_SCREEN_S;
      if (NoCable > CHARGE_NOCABLE_S) {
        DEBUG ((DEBUG_INFO, "ChargeMode: no charger, battery too low - powering off\n"));
        gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        continue;
      }
      ChargeScreen (&S, L"Battery too low and no charger - powering off (any key: boot anyway)");
      continue;
    }
    if (NoCable > 0) {
      NoCable = 0;
      gST->ConOut->ClearScreen (gST->ConOut);
    }

    if (ScreenLeft > 0) {
      ChargeScreen (&S, L"Battery low - charging, not starting yet");
      if (--ScreenLeft == 0) {
        Backlight (FALSE);
      }
    }
  }

BootAnyway:
  gBS->SetTimer (Tick, TimerCancel, 0);
  gBS->CloseEvent (Tick);
  Backlight (TRUE);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, TRUE);
}

/**
  One-shot "boot the Shell next time": a file \bootshell.flag on any FAT
  volume (pushed over the USB debug channel) is deleted and the menu is
  skipped in favour of the Shell with startup.nsh.  Lets a script run on
  the tablet without anybody touching it.
**/
STATIC
BOOLEAN
TakeShellFlag (
  VOID
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  BOOLEAN                          Found;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    return FALSE;
  }
  Found = FALSE;
  for (Index = 0; (Index < Count) && !Found; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
        EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }
    if (!EFI_ERROR (Root->Open (Root, &File, L"\\bootshell.flag", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
      File->Delete (File);
      Found = TRUE;
    }
    Root->Close (Root);
  }
  FreePool (Handles);
  if (Found) {
    DEBUG ((DEBUG_INFO, "BootMenu: \\bootshell.flag found - Shell with startup.nsh\n"));
  }
  return Found;
}

/**
  Platform boot manager after the console is initialized.
**/
VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_STATUS  Status;

  //
  // Bind every driver to every handle.  Nothing connects the GOP handle to
  // GraphicsConsoleDxe otherwise, so no SimpleTextOut is ever installed and
  // gST->ConOut stays NULL -- which is what kills the Shell on its first
  // Print().  This is the step HtcLeoPkg does (PlatformBm.c:411) that we were
  // missing; consoles were registered in BeforeConsole but never bound.
  //
  EfiBootManagerConnectAll ();

  //
  // Consoles were registered in BeforeConsole and BdsEntry() connects the
  // default ones between the two callbacks; re-connecting repairs anything
  // ConnectAll() re-bound.  Leo calls these same two (PlatformBm.c:411).
  //
  EfiBootManagerConnectAllConsoles ();

  //
  // BmUpdateSystemTableConsole() only accepts a console whose device path
  // LocateDevicePath() can resolve.  If that fails, gST->ConOut stays NULL and
  // the Shell dies on its first Print().  Grab any SimpleTextOut directly so
  // the Shell always has something to draw on.
  //
  if (gST->ConOut == NULL) {
    EFI_STATUS                      S;
    EFI_HANDLE                      *Handles;
    UINTN                           NoHandles;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *TextOut;

    S = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextOutProtocolGuid,
                                 NULL, &NoHandles, &Handles);
    if (!EFI_ERROR (S) && (NoHandles > 0)) {
      S = gBS->HandleProtocol (Handles[0], &gEfiSimpleTextOutProtocolGuid,
                               (VOID **)&TextOut);
      if (!EFI_ERROR (S)) {
        gST->ConsoleOutHandle = Handles[0];
        gST->ConOut            = TextOut;
      }
      gBS->FreePool (Handles);
    }
  }

  //
  // Show the splash.  Must come after ConnectAll(): BootLogoEnableLogo()
  // looks the GOP up on gST->ConsoleOutHandle, and that handle only carries
  // GOP once ConSplitter has adopted our framebuffer's GOP.
  //
  Status = BootLogoEnableLogo ();


  // Re-enumerate boot options from all file systems / firmware volumes.
  EfiBootManagerRefreshAllBootOption ();

  //
  // Boot what the menu says.  Do not hand control back to BDS: whatever
  // else RefreshAllBootOption() found (BOOTARM.EFI on an SD card, for
  // instance) would be picked by BootOrder.  If Windows cannot be started,
  // fall back to the Shell with startup.nsh (the old default).
  //
  ChargeModeIfNeeded ();

  //
  // TEMPORARY (display bring-up): boot straight to the Shell and its
  // startup.nsh, no menu, so unattended test reboots end up in the test.
  // Set to 0 to get the boot menu (and Windows) back.
  //
#define TEMP_DEFAULT_SHELL  0
  if (TEMP_DEFAULT_SHELL || TakeShellFlag ()) {
    BootShell (FALSE);
    return;
  }

  switch (BootMenu ()) {
    case MenuShell:
      //
      // The Shell with its startup.nsh: with no keyboard that script is the
      // only way to run anything from the Shell.  Windows starts from its
      // own button, so startup.nsh no longer has to boot it.
      //
      BootShell (FALSE);
      break;
    case MenuDownload:
      RebootTo (L"download");
      break;
    case MenuRecovery:
      RebootTo (L"recovery");
      break;
    default:
      BootWindows ();
      BootShell (FALSE);
      break;
  }

  //
  // The platform watchdog would fire after ~30 s of BDS running with no
  // boot attempt; disable it explicitly so a slow DEBUG build cannot reset
  // the phone mid-boot.
  //
  Status = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  DEBUG ((DEBUG_INFO, "%a: watchdog disarm: %r\n", __FUNCTION__, Status));
}
