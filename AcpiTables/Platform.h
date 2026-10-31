/** @file
  Samsung Exynos5420 (v1awifi, SM-P900) defines shared by the ACPI tables.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_ACPI_PLATFORM_H__
#define __EXYNOS_ACPI_PLATFORM_H__

#include <IndustryStandard/Acpi.h>

#define EFI_ACPI_OEM_ID             {'S','A','M','S','N','G'}
#define EFI_ACPI_OEM_TABLE_ID       SIGNATURE_64 ('E','X','Y','5','4','2','0',' ')
#define EFI_ACPI_OEM_REVISION       0x00000001
#define EFI_ACPI_CREATOR_ID         SIGNATURE_32 ('E','D','K','2')
#define EFI_ACPI_CREATOR_REVISION   0x00000001

#define EXYNOS_ACPI_HEADER(Signature, Type, Revision)  \
  {                                                    \
    Signature,                                         \
    sizeof (Type),                                     \
    Revision,                                          \
    0,                     /* checksum: AcpiTableDxe */ \
    EFI_ACPI_OEM_ID,                                   \
    EFI_ACPI_OEM_TABLE_ID,                             \
    EFI_ACPI_OEM_REVISION,                             \
    EFI_ACPI_CREATOR_ID,                               \
    EFI_ACPI_CREATOR_REVISION                          \
  }

//
// GIC-400 (same PCDs ArmGicDxe uses).
//
#define EXYNOS_GICD_BASE  FixedPcdGet32 (PcdGicDistributorBase)
#define EXYNOS_GICC_BASE  FixedPcdGet32 (PcdGicInterruptInterfaceBase)

//
// Multi-Core Timer: global comparator 0 reaches the GIC through interrupt
// combiner group 23 bit 3 = SPI 23 = GSI 55 (MctTimerDxe enables the
// combiner bit and leaves it enabled for the OS).
//
#define EXYNOS_MCT_BASE      FixedPcdGet32 (PcdMctBaseAddress)
#define EXYNOS_MCT_SIZE      0x1000
#define EXYNOS_MCT_FREQUENCY FixedPcdGet32 (PcdMctFrequencyHz)
#define EXYNOS_MCT_G0_GSI    55

#endif
