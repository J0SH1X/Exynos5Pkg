/** @file
  Exynos5420 SPI driver: SPI2 (the WM5102 codec on v1awifi) behind the
  EXYNOS_SPI protocol.  Sets up what the controller needs at boot, which
  Windows (exynosspi) relies on afterwards:

    CMU   SCLK_SPI2 from XXTI (24 MHz), no divide, pre-divider 2: the
          controller halves it again, so SCK = 6 MHz (the codec allows far
          more; the kernel ran it at 10 MHz)
    pins  GPB1(1) SCK, GPB1(3) MISO, GPB1(4) MOSI: function 5, pull-up,
          drive strength 3 (s3c64xx_spi2_cfg_gpio()); GPB1(2) chip select
          as GPIO (SpiCore)

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosSpi.h>

#include "SpiCore.h"

#define SPI2_BASE              0x12D40000
#define GPB1_BANK              0x14010080     // GPIO B controller + 0x80

#define CMU_BASE               0x10010000
#define CLK_SRC_PERIC1         (CMU_BASE + 0x10254)
#define CLK_SRC_MASK_PERIC1    (CMU_BASE + 0x10354)
#define CLK_DIV_PERIC1         (CMU_BASE + 0x1055C)
#define CLK_DIV_PERIC4         (CMU_BASE + 0x10568)
#define CLK_GATE_IP_PERIC      (CMU_BASE + 0x10950)

#define GPIO_CON               0x0
#define GPIO_PUD               0x8
#define GPIO_DRV               0xC

STATIC EXYNOS_SPI  mSpi2 = { SPI2_BASE, GPB1_BANK, 2, 0 };

STATIC
EFI_STATUS
EFIAPI
SpiTransfer (
  IN  EXYNOS_SPI_PROTOCOL  *This,
  IN  UINT32               Bus,
  IN  CONST UINT8          *Tx     OPTIONAL,
  IN  UINTN                TxLen,
  OUT UINT8                *Rx     OPTIONAL,
  IN  UINTN                RxLen
  )
{
  if ((Bus != 2) || ((Tx == NULL) && (TxLen != 0)) || ((Rx == NULL) && (RxLen != 0))) {
    return EFI_INVALID_PARAMETER;
  }
  return (SpiCoreTransfer (&mSpi2, Tx, TxLen, Rx, RxLen) == 0) ? EFI_SUCCESS : EFI_TIMEOUT;
}

STATIC EXYNOS_SPI_PROTOCOL  mExynosSpi = { SpiTransfer };

STATIC
VOID
Spi2Platform (
  VOID
  )
{
  UINT32  Pin;

  //
  // SCLK_SPI2: source 0 (XXTI), ratio 0 (/1), pre-ratio 1 (/2), unmasked,
  // controller bus clock gated on.
  //
  MmioAndThenOr32 (CLK_SRC_PERIC1, ~(7u << 28), 0u << 28);
  MmioAndThenOr32 (CLK_DIV_PERIC1, ~(0xFu << 28), 0u << 28);
  MmioAndThenOr32 (CLK_DIV_PERIC4, ~(0xFFu << 24), 1u << 24);
  MmioOr32 (CLK_SRC_MASK_PERIC1, 1u << 28);
  MmioOr32 (CLK_GATE_IP_PERIC, 1u << 18);

  for (Pin = 1; Pin <= 4; Pin++) {
    if (Pin == 2) {
      continue;                         // chip select: SpiCoreInit()
    }
    MmioAndThenOr32 (GPB1_BANK + GPIO_CON, ~(0xFu << (Pin * 4)), 5u << (Pin * 4));
    MmioAndThenOr32 (GPB1_BANK + GPIO_PUD, ~(3u << (Pin * 2)), 3u << (Pin * 2));
    MmioAndThenOr32 (GPB1_BANK + GPIO_DRV, ~(3u << (Pin * 2)), 3u << (Pin * 2));
  }
}

EFI_STATUS
EFIAPI
SpiDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  Spi2Platform ();
  SpiCoreInit (&mSpi2);
  DEBUG ((DEBUG_INFO, "SpiDxe: SPI2 ready (CH_CFG 0x%08x MODE_CFG 0x%08x)\n",
          MmioRead32 (SPI2_BASE + 0x00), MmioRead32 (SPI2_BASE + 0x08)));
  return gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gExynosSpiProtocolGuid, &mExynosSpi, NULL);
}
