/** @file
  UEFI environment for the shared MMC code (MmcHost.h).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __MMC_ENV_H__
#define __MMC_ENV_H__

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

//
// UEFI runs identity mapped with DRAM below 4 GiB.
//
#define DmaAllocatePages(Pages)  AllocatePages (Pages)
#define DMA_ADDRESS(Va)          ((UINT32)(UINTN)(Va))

#endif // __MMC_ENV_H__
