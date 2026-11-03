/** @file
  GPU / display memory carve-out: 128 MiB of DRAM below the OS log ring,
  reported to the OS as reserved memory, so Windows never owns it and the
  display driver (exynoswddm) can map it as its WDDM memory segment.  UEFI
  writes a header at its start (magic, base, size) that the driver checks
  before mapping the range.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_GPU_CARVEOUT_H__
#define __EXYNOS_GPU_CARVEOUT_H__

#define GPU_CARVEOUT_BASE   0x96000000u
#define GPU_CARVEOUT_SIZE   0x08000000u      // ends at OS_LOG_BASE (0x9E000000)
#define GPU_CARVEOUT_MAGIC  0x43475845u      // 'EXGC'

typedef struct {
  UINT32  Magic;
  UINT32  Base;
  UINT32  Size;
  UINT32  Reserved;
} GPU_CARVEOUT_HEADER;

#endif
