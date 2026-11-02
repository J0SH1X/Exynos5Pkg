/** @file
  Exynos SPI protocol (s3c64xx-style controllers, Exynos5420 v1awifi).

    Bus 2  SPI2 at 0x12D40000, GPB1(1) SCK, GPB1(3) MISO, GPB1(4) MOSI
           (function 5), chip select GPB1(2) as a GPIO: the WM5102 codec

  Only bus 2 is wired up on this board; the bus number is passed per call
  so more controllers can join without a protocol change.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_SPI_PROTOCOL_H__
#define __EXYNOS_SPI_PROTOCOL_H__

#define EXYNOS_SPI_PROTOCOL_GUID                                               \
  {                                                                            \
    0x9d3f4a71, 0x5c20, 0x4b8e,                                                \
    {                                                                          \
      0xa1, 0x77, 0x6e, 0x24, 0x41, 0x10, 0x44, 0x05                           \
    }                                                                          \
  }

typedef struct _EXYNOS_SPI_PROTOCOL EXYNOS_SPI_PROTOCOL;

/**
  One chip-select cycle (mode 0, MSB first): TxLen bytes out (what comes in
  meanwhile is dropped), then RxLen bytes in (zeros go out).

  @param[in]  Bus    Controller number (2).
  @param[in]  Tx     Bytes to send, may be NULL when TxLen is 0.
  @param[in]  TxLen  Number of bytes to send.
  @param[out] Rx     Receives RxLen bytes, may be NULL when RxLen is 0.
  @param[in]  RxLen  Number of bytes to receive.

  @retval EFI_SUCCESS            Done.
  @retval EFI_INVALID_PARAMETER  No such bus.
  @retval EFI_TIMEOUT            The controller stopped moving bytes.
**/
typedef
EFI_STATUS
(EFIAPI *EXYNOS_SPI_TRANSFER)(
  IN  EXYNOS_SPI_PROTOCOL  *This,
  IN  UINT32               Bus,
  IN  CONST UINT8          *Tx     OPTIONAL,
  IN  UINTN                TxLen,
  OUT UINT8                *Rx     OPTIONAL,
  IN  UINTN                RxLen
  );

struct _EXYNOS_SPI_PROTOCOL {
  EXYNOS_SPI_TRANSFER  Transfer;
};

extern EFI_GUID  gExynosSpiProtocolGuid;

#endif // __EXYNOS_SPI_PROTOCOL_H__
