/** @file
  ResetSystemLib for Samsung Exynos5420 (v1awifi), for ResetSystemRuntimeDxe:
  the OS restarts and powers off through it.

  Reset: the PMU software reset register (SWRESET, PMU + 0x0400), what the
  kernel's exynos5_restart() uses.  Any reset flavour (cold/warm) becomes
  the same SoC-wide software reset.  Like sec_reboot(), the Samsung
  bootloader's reboot mode goes into PMU INFORM3 first:
  ResetPlatformSpecific() with ResetData L"download" (or L"recovery")
  reboots straight into that mode.

  Shutdown: like sec_power_off(), drive PS_HOLD (PMU + 0x330C bit 8) low so
  the PMIC cuts power.  With a charger attached the PMIC keeps the board
  up; the kernel then reboots into the bootloader's charging screen
  (INFORM2 = 0), and so do we when PS_HOLD did not take.

  Runtime: this code runs after ExitBootServices / SetVirtualAddressMap
  (Windows' restart and shut down).  The PMU window is added to the GCD as
  runtime MMIO so the OS maps it, its address is converted on the virtual
  address change, and the runtime paths use neither timers nor DEBUG (both
  would touch hardware at physical addresses the OS has not mapped, which
  is what used to bugcheck every Windows restart with
  KMODE_EXCEPTION_NOT_HANDLED).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>

#define EXYNOS_PMU_SIZE         SIZE_64KB
#define EXYNOS_PMU_SWRESET      0x0400
#define EXYNOS_PMU_INFORM2      0x0808
#define EXYNOS_PMU_INFORM3      0x080C
#define EXYNOS_PS_HOLD_CONTROL  0x330C
#define PS_HOLD_DATA_HIGH       (1u << 8)

//
// sec-reboot.c: the INFORM2 magic skips the LP charging screen, INFORM3
// carries the reboot mode for the bootloader.
//
#define INFORM2_NO_LPM          0x12345678
#define INFORM2_LPM             0x0
#define REBOOT_MODE_PREFIX      0x12345670
#define REBOOT_MODE_NONE        0
#define REBOOT_MODE_DOWNLOAD    1
#define REBOOT_MODE_RECOVERY    4

//
// About a second of spinning at 1-2 GHz, without a timer.
//
#define POWER_OFF_SPINS         200000000u

STATIC UINTN      mPmuBase;
STATIC EFI_EVENT  mVirtualAddressChangeEvent;

STATIC
VOID
SwReset (
  IN UINT32  Inform2,
  IN UINT32  Mode
  )
{
  MmioWrite32 (mPmuBase + EXYNOS_PMU_INFORM2, Inform2);
  MmioWrite32 (mPmuBase + EXYNOS_PMU_INFORM3, REBOOT_MODE_PREFIX | Mode);
  MmioWrite32 (mPmuBase + EXYNOS_PMU_SWRESET, 0x1);

  //
  // Should never get here.  Spin so no code runs on a half-reset SoC.
  //
  CpuDeadLoop ();
}

VOID
EFIAPI
ResetCold (
  VOID
  )
{
  SwReset (INFORM2_NO_LPM, REBOOT_MODE_NONE);
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  SwReset (INFORM2_NO_LPM, REBOOT_MODE_NONE);
}

VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  volatile UINT32  Spin;

  MmioAnd32 (mPmuBase + EXYNOS_PS_HOLD_CONTROL, ~PS_HOLD_DATA_HIGH);

  //
  // Still here after a second: a charger holds the board up.  Reboot into
  // the bootloader's charging screen, like the kernel.
  //
  for (Spin = 0; Spin < POWER_OFF_SPINS; Spin++) {
  }
  SwReset (INFORM2_LPM, REBOOT_MODE_NONE);
}

VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData
  )
{
  CONST CHAR16  *Mode;
  UINT32        RebootMode;

  //
  // ResetData starts with a NUL-terminated string (UEFI ResetSystem).
  //
  Mode       = (CONST CHAR16 *)ResetData;
  RebootMode = REBOOT_MODE_NONE;
  if ((Mode != NULL) && (DataSize >= sizeof (L"download"))) {
    if (StrnCmp (Mode, L"download", 8) == 0) {
      RebootMode = REBOOT_MODE_DOWNLOAD;
    } else if (StrnCmp (Mode, L"recovery", 8) == 0) {
      RebootMode = REBOOT_MODE_RECOVERY;
    }
  }

  SwReset (INFORM2_NO_LPM, RebootMode);
}

STATIC
VOID
EFIAPI
VirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EfiConvertPointer (0, (VOID **)&mPmuBase);
}

/**
  Make the PMU window runtime MMIO (so the OS maps it for us) and follow it
  to its virtual address.
**/
EFI_STATUS
EFIAPI
ExynosResetLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  EFI_PHYSICAL_ADDRESS             Base;
  EFI_STATUS                       Status;

  Base     = PcdGet32 (PcdPmuBaseAddress);
  mPmuBase = (UINTN)Base;

  Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);
  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, EXYNOS_PMU_SIZE,
                                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "ExynosResetLib: adding PMU MMIO failed - %r\n", Status));
      return Status;
    }
  } else {
    Status = gDS->SetMemorySpaceCapabilities (Base, EXYNOS_PMU_SIZE,
                                              Desc.Capabilities | EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "ExynosResetLib: PMU MMIO capabilities - %r\n", Status));
    }
  }

  Status = gDS->SetMemorySpaceAttributes (Base, EXYNOS_PMU_SIZE, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ExynosResetLib: PMU MMIO runtime attribute - %r\n", Status));
    return Status;
  }

  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, VirtualAddressChange, NULL,
                             &gEfiEventVirtualAddressChangeGuid, &mVirtualAddressChangeEvent);
}
