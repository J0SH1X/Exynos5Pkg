/** @file
  Bring cores 1-3 of the boot (Cortex-A15) cluster out of reset, park them
  in ACPI parking-protocol mailboxes and publish the MADT that lists them,
  so Windows can start them.

  Exynos5420 is big.LITTLE (4x A15 + 4x A7); like Samsung's kernel (IKS,
  NR_CPUS = 4) only one cluster is used: the one we booted on.

  Secondary bring-up (platsmp.c, CONFIG_ARM_TRUSTZONE):
    - entry address -> SYSRAM_NS + 0x1C (0x0207301C), dsb
    - PMU ARM_CORE_CONFIGURATION(n) = 0x10042000 + n * 0x80: LOCAL_PWR_EN
      (3), wait for STATUS (+4) to follow
    - sev; the secure firmware hands the core to the entry address.
  Cores of the other cluster (pmu index n + 4) are not touched.

  The MADT used to be a static table (AcpiTables/Madt.aslc).  It is built
  here instead because the mailbox addresses are only known at run time,
  and only cores that actually reached the parking loop are listed - if
  none do, Windows still gets a valid single-CPU MADT.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <IndustryStandard/Acpi.h>
#include <Library/AcpiLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/AcpiTable.h>

#include "../../AcpiTables/Platform.h"

#define MAX_CORES           4

#define SYSRAM_NS           0x02073000
#define SYSRAM_BOOT_VECTOR  (SYSRAM_NS + 0x1C)
#define SYSRAM_CPU_STATE(n) (SYSRAM_NS + 0x28 + (n) * 4)

#define PMU_CORE_CONFIG(n)  (0x10042000 + (n) * 0x80)
#define PMU_CORE_STATUS(n)  (PMU_CORE_CONFIG (n) + 4)
#define CORE_LOCAL_PWR_EN   0x3

//
// Mailbox layout (Park.S).
//
#define MB_SIZE             SIZE_4KB
#define MB_PROCESSOR_ID     0x000
#define MB_JUMP_ADDRESS     0x008
#define FW_STATE            0x800
#define FW_GIC_ID           0x804
#define FW_ACTLR            0x808
#define FW_CPSR             0x80C
#define FW_MPIDR            0x810
#define FW_SCTLR            0x814
#define FW_STATE_PARKED     1

extern UINT8  MpParkStart[];
extern UINT8  MpParkLiterals[];
extern UINT8  MpParkEnd[];

#pragma pack (1)
typedef struct {
  EFI_ACPI_6_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER  Header;
  EFI_ACPI_6_0_GIC_STRUCTURE                           Gicc[MAX_CORES];
  EFI_ACPI_6_0_GIC_DISTRIBUTOR_STRUCTURE               Gicd;
} EXYNOS_MADT;
#pragma pack ()

STATIC
UINT32
MbRead (
  IN UINTN  Mailbox,
  IN UINTN  Offset
  )
{
  //
  // The parked core runs with caches off: drop our (cached) copy first.
  //
  InvalidateDataCacheRange ((VOID *)(Mailbox + Offset), sizeof (UINT32));
  return MmioRead32 (Mailbox + Offset);
}

STATIC
BOOLEAN
StartCore (
  IN UINTN  Core,
  IN UINTN  PmuIndex,
  IN UINTN  Mailbox
  )
{
  UINTN  Retry;

  DEBUG ((DEBUG_INFO, "MpPark: core %u: pmu %u status 0x%x, cpu state 0x%x\n",
          Core, PmuIndex, MmioRead32 (PMU_CORE_STATUS (PmuIndex)), MmioRead32 (SYSRAM_CPU_STATE (Core))));

  if ((MmioRead32 (PMU_CORE_STATUS (PmuIndex)) & CORE_LOCAL_PWR_EN) == 0) {
    MmioWrite32 (PMU_CORE_CONFIG (PmuIndex), CORE_LOCAL_PWR_EN);
    for (Retry = 0; (MmioRead32 (PMU_CORE_STATUS (PmuIndex)) & CORE_LOCAL_PWR_EN) != CORE_LOCAL_PWR_EN; Retry++) {
      if (Retry > 100) {
        DEBUG ((DEBUG_ERROR, "MpPark: core %u power-up timed out (status 0x%x)\n",
                Core, MmioRead32 (PMU_CORE_STATUS (PmuIndex))));
        return FALSE;
      }
      gBS->Stall (100);
    }
  }

  //
  // Wait (up to 100 ms) for the core to reach the parking loop, poking the
  // secure firmware's WFE with an event now and then.
  //
  for (Retry = 0; Retry < 1000; Retry++) {
    ArmDataSynchronizationBarrier ();
    __asm__ volatile ("sev" ::: "memory");
    if (MbRead (Mailbox, FW_STATE) == FW_STATE_PARKED) {
      DEBUG ((DEBUG_INFO, "MpPark: core %u parked after %u us: MPIDR 0x%08x GIC id %u CPSR 0x%x "
              "SCTLR 0x%08x ACTLR 0x%08x\n",
              Core, Retry * 100, MbRead (Mailbox, FW_MPIDR), MbRead (Mailbox, FW_GIC_ID),
              MbRead (Mailbox, FW_CPSR), MbRead (Mailbox, FW_SCTLR), MbRead (Mailbox, FW_ACTLR)));
      return TRUE;
    }
    gBS->Stall (100);
  }

  DEBUG ((DEBUG_ERROR, "MpPark: core %u did not reach the parking loop (pmu status 0x%x, cpu state 0x%x)\n",
          Core, MmioRead32 (PMU_CORE_STATUS (PmuIndex)), MmioRead32 (SYSRAM_CPU_STATE (Core))));
  return FALSE;
}

STATIC
VOID
SetGicc (
  OUT EFI_ACPI_6_0_GIC_STRUCTURE  *Gicc,
  IN  UINT32                      GicId,
  IN  UINT64                      BootMpidr,
  IN  UINTN                       Core,
  IN  UINTN                       MailboxBase
  )
{
  EFI_ACPI_6_0_GIC_STRUCTURE  Init = EFI_ACPI_6_0_GICC_STRUCTURE_INIT (
                                       GicId,
                                       GicId,
                                       (BootMpidr & 0xFF00) | Core,
                                       EFI_ACPI_6_0_GIC_ENABLED,
                                       0,          // PMU interrupt: none (combiner)
                                       EXYNOS_GICC_BASE,
                                       0, 0, 0, 0,
                                       0
                                       );

  CopyMem (Gicc, &Init, sizeof (Init));
  if (MailboxBase != 0) {
    Gicc->ParkingProtocolVersion = 1;
    Gicc->ParkedAddress          = MailboxBase + Core * MB_SIZE;
  }
}

STATIC
EFI_STATUS
InstallMadt (
  IN UINT64   Mpidr,
  IN UINT32   BootGicId,
  IN UINTN    MailboxBase,
  IN BOOLEAN  *Parked
  )
{
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTable;
  EXYNOS_MADT              Madt;
  UINTN                    Core;
  UINTN                    Count;
  UINTN                    TableKey;
  EFI_STATUS               Status;

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Madt, sizeof (Madt));
  Madt.Header.Header.Signature       = EFI_ACPI_6_0_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE;
  Madt.Header.Header.Revision        = EFI_ACPI_6_0_MULTIPLE_APIC_DESCRIPTION_TABLE_REVISION;
  CopyMem (Madt.Header.Header.OemId, (UINT8[6])EFI_ACPI_OEM_ID, sizeof (Madt.Header.Header.OemId));
  Madt.Header.Header.OemTableId      = EFI_ACPI_OEM_TABLE_ID;
  Madt.Header.Header.OemRevision     = EFI_ACPI_OEM_REVISION;
  Madt.Header.Header.CreatorId       = EFI_ACPI_CREATOR_ID;
  Madt.Header.Header.CreatorRevision = EFI_ACPI_CREATOR_REVISION;
  Madt.Header.LocalApicAddress       = EXYNOS_GICC_BASE;

  //
  // Boot CPU first (Windows takes the first GICC entry as the BSP), then
  // the parked cores.  Processor UID = GIC CPU interface number, which is
  // also what the OS writes into a mailbox's ProcessorId.
  //
  Count = 0;
  SetGicc (&Madt.Gicc[Count++], BootGicId, Mpidr, Mpidr & 0x3, MailboxBase);
  for (Core = 0; Core < MAX_CORES; Core++) {
    if (Parked[Core]) {
      SetGicc (&Madt.Gicc[Count++], MbRead (MailboxBase + Core * MB_SIZE, FW_GIC_ID), Mpidr, Core, MailboxBase);
    }
  }

  //
  // Close the gap: GICD right after the last GICC.
  //
  {
    EFI_ACPI_6_0_GIC_DISTRIBUTOR_STRUCTURE  Gicd = EFI_ACPI_6_0_GIC_DISTRIBUTOR_INIT (0, EXYNOS_GICD_BASE, 0, EFI_ACPI_6_0_GIC_V2);
    CopyMem (&Madt.Gicc[Count], &Gicd, sizeof (Gicd));
  }
  Madt.Header.Header.Length = (UINT32)(sizeof (Madt.Header) + Count * sizeof (EFI_ACPI_6_0_GIC_STRUCTURE) +
                                       sizeof (EFI_ACPI_6_0_GIC_DISTRIBUTOR_STRUCTURE));

  DEBUG ((DEBUG_INFO, "MpPark: MADT with %u CPU(s)\n", Count));
  return AcpiTable->InstallAcpiTable (AcpiTable, &Madt, Madt.Header.Header.Length, &TableKey);
}

EFI_STATUS
EFIAPI
MpParkDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_PHYSICAL_ADDRESS  Pages;
  UINTN                 MailboxBase;
  UINTN                 Code;
  UINT64                Mpidr;
  UINTN                 BootCore;
  UINTN                 PmuOffset;
  UINT32                BootGicId;
  UINT32                *Literals;
  BOOLEAN               Parked[MAX_CORES];
  UINTN                 Core;
  EFI_STATUS            Status;

  ZeroMem (Parked, sizeof (Parked));

  Mpidr     = ArmReadMpidr ();
  BootCore  = Mpidr & 0x3;
  PmuOffset = ((Mpidr >> 8) & 0xF) != 0 ? 4 : 0;
  BootGicId = (UINT32)(31 - __builtin_clz (MmioRead32 (EXYNOS_GICD_BASE + 0x800) & 0xFF));
  DEBUG ((DEBUG_INFO, "MpPark: boot CPU MPIDR 0x%08x (cluster %u core %u), GIC id %u, boot vector 0x%08x\n",
          (UINT32)Mpidr, (UINT32)((Mpidr >> 8) & 0xF), BootCore, BootGicId, MmioRead32 (SYSRAM_BOOT_VECTOR)));

  //
  // One page of code, then one mailbox per core (index = MPIDR.Aff0), all
  // reserved so the OS leaves them alone.
  //
  Pages  = MAX_UINT32;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType,
                               1 + MAX_CORES, &Pages);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MpPark: no memory for the mailboxes - %r\n", Status));
    return InstallMadt (Mpidr, BootGicId, 0, Parked);
  }

  Code        = (UINTN)Pages;
  MailboxBase = Code + SIZE_4KB;
  ZeroMem ((VOID *)Code, SIZE_4KB * (1 + MAX_CORES));
  CopyMem ((VOID *)Code, MpParkStart, (UINTN)(MpParkEnd - MpParkStart));
  Literals    = (UINT32 *)(Code + (UINTN)(MpParkLiterals - MpParkStart));
  Literals[0] = (UINT32)MailboxBase;
  Literals[1] = EXYNOS_GICC_BASE;
  Literals[2] = EXYNOS_GICD_BASE;

  //
  // The secondaries fetch and load with the MMU and caches off.
  //
  WriteBackInvalidateDataCacheRange ((VOID *)Code, SIZE_4KB * (1 + MAX_CORES));
  InvalidateInstructionCacheRange ((VOID *)Code, SIZE_4KB);

  MmioWrite32 (SYSRAM_BOOT_VECTOR, (UINT32)Code);
  ArmDataSynchronizationBarrier ();

  for (Core = 0; Core < MAX_CORES; Core++) {
    if (Core == BootCore) {
      continue;
    }
    Parked[Core] = StartCore (Core, Core + PmuOffset, MailboxBase + Core * MB_SIZE);
  }

  return InstallMadt (Mpidr, BootGicId, MailboxBase, Parked);
}
