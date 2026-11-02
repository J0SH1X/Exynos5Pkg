/** @file
  Switch for the PL330 system DMA (halextpl330 HAL extension): the CSRT's
  PL330 group and the DSDT's FixedDMA resources.  Both come and go
  together: a FixedDMA without a registered controller would keep its
  device from starting.  Usable from C (Csrt.aslc) and ASL (Dsdt.asl).

  1 needs the halextpl330 HAL extension installed (System32 plus its
  OSExtensionDatabase entry); with it, Windows capture (exynosaudio's
  microphone) works.  0 builds the image without system DMA.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_PLATFORM_DMA_H__
#define __EXYNOS_PLATFORM_DMA_H__

#define EXYNOS_PL330_DMA  0

#endif
