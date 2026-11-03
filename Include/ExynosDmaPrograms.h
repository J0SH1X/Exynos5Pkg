/** @file
  Memory for PL330 DMA channel programs: reserved by the firmware
  (Exynos5420PkgMem.c) and named in the CSRT's PL330 controller descriptor,
  so the halextpl330 HAL extension needs no HAL common buffers (it maps
  this range itself, uncached).  1 KiB per channel.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_DMA_PROGRAMS_H__
#define __EXYNOS_DMA_PROGRAMS_H__

#define PL330_PROGRAM_BASE  0x9E080000      // right after the OS log ring
#define PL330_PROGRAM_SIZE  0x00002000      // ADMA0 at +0, PDMA0 at +0x1000

#endif
