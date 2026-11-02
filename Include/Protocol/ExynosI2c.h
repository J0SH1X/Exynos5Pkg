/** @file
  Exynos I2C protocol.

  Bus numbers follow Linux on Exynos5420 (v1awifi):
    0..3   s3c2410-style I2C at 0x12C60000 + Bus * 0x10000
    4..10  high-speed I2C (exynos5-hs-i2c) channels 0..6
           (hs-i2c3 = bus 7: MAX77802 PMIC, hs-i2c4 = bus 8: Wacom)
    17     i2c-gpio: MAX77888 charger 0x66 / MUIC 0x25
    18     i2c-gpio: MAX17050 fuel gauge 0x36
    19     i2c-gpio: LP8556 backlight 0x2C (GPA2(4)/(5); the kernel's bus 24)
  All controllers are clocked by ACLK_66_PERIC (~66 MHz).

  Modelled on HtcLeoPkg's HtcLeoI2C protocol (Linux-style i2c_msg transfers),
  with the bus number passed per call so one protocol instance covers all
  eight controllers.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_I2C_PROTOCOL_H__
#define __EXYNOS_I2C_PROTOCOL_H__

#define EXYNOS_I2C_PROTOCOL_GUID                                               \
  {                                                                            \
    0x9d3f4a71, 0x5c20, 0x4b8e,                                                \
    {                                                                          \
      0xa1, 0x77, 0x6e, 0x24, 0x41, 0x10, 0x44, 0x02                           \
    }                                                                          \
  }

//
// Buses 0..3: s3c2410-style controllers; 4..10: hs-i2c channels 0..6;
// 17, 18: bit-banged on GPIOs, numbered like the kernel's i2c-gpio buses
// (17: MAX77888 charger/MUIC, 18: MAX17050 fuel gauge), 19 the LP8556
// backlight's (the kernel's bus 24).  11..16 do not exist.
//
#define EXYNOS_I2C_FIRST_HS_BUS    4
#define EXYNOS_I2C_LAST_HS_BUS     10
#define EXYNOS_I2C_FIRST_GPIO_BUS  17
#define EXYNOS_I2C_MAX_BUS         20

//
// Linux-style transfer flags.
//
#define I2C_M_RD  0x0001

#pragma pack(1)
typedef struct {
  UINT16  Addr;   // 7-bit slave address
  UINT16  Flags;  // I2C_M_RD for reads
  UINT16  Len;    // payload length in bytes
  UINT8   *Buf;
} EXYNOS_I2C_MSG;
#pragma pack()

typedef struct _EXYNOS_I2C_PROTOCOL EXYNOS_I2C_PROTOCOL;

/**
  Write Count bytes to a slave.

  @param[in] Bus   Controller number, 0..EXYNOS_I2C_MAX_BUS-1.
  @param[in] Chip  7-bit slave address.
  @param[in] Buf   Payload.
  @param[in] Count Payload length in bytes.

  @return Number of bytes written, or -1 on error/NACK/timeout.
**/
typedef
INTN
(EFIAPI *EXYNOS_I2C_WRITE)(
  IN EXYNOS_I2C_PROTOCOL  *This,
  IN UINT32               Bus,
  IN UINT16               Chip,
  IN UINT8                *Buf,
  IN UINTN                Count
  );

/**
  Write Reg, then read Count bytes from a slave (repeated start).

  @param[in] Bus   Controller number, 0..EXYNOS_I2C_MAX_BUS-1.
  @param[in] Chip  7-bit slave address.
  @param[in] Reg   Register offset sent before the read.
  @param[in] Buf   Destination buffer.
  @param[in] Count Number of bytes to read.

  @return Number of bytes read, or -1 on error/NACK/timeout.
**/
typedef
INTN
(EFIAPI *EXYNOS_I2C_READ)(
  IN EXYNOS_I2C_PROTOCOL  *This,
  IN UINT32               Bus,
  IN UINT16               Chip,
  IN UINT8                Reg,
  IN UINT8                *Buf,
  IN UINTN                Count
  );

/**
  Execute a sequence of messages (Linux i2c_transfer semantics).

  @param[in] Bus  Controller number, 0..EXYNOS_I2C_MAX_BUS-1.
  @param[in] Msgs Message array.
  @param[in] Num  Number of messages.

  @return Number of messages transferred, or -1 on error.
**/
typedef
INTN
(EFIAPI *EXYNOS_I2C_XFER)(
  IN EXYNOS_I2C_PROTOCOL  *This,
  IN UINT32               Bus,
  IN EXYNOS_I2C_MSG       *Msgs,
  IN INTN                 Num
  );

struct _EXYNOS_I2C_PROTOCOL {
  EXYNOS_I2C_WRITE  Write;
  EXYNOS_I2C_READ   Read;
  EXYNOS_I2C_XFER   Xfer;
};

extern EFI_GUID gExynosI2cProtocolGuid;

#endif
