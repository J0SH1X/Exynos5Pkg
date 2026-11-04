/** @file
  v1awifi camera module power (rear IMX134 module on CSI-A / ISP I2C0):
  rails, enables, MCLK, reset and the ISP-owned pins, in the order of the
  Samsung 3.4 kernel (board-universal5420-media.c gpio_universal5420, the
  V1A "#else" branches; setup-fimc-is.c exynos5420_fimc_is_sensor_clk_on).
  Shared by UEFI and Windows through <ExynosEnv.h>; the caller maps the
  GPIO / CMU windows and supplies the PMIC access (MAX77802, HS-I2C3 0x09).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __CAM_BOARD_H__
#define __CAM_BOARD_H__

#include <ExynosEnv.h>

#define CAM_GPIO_LB_PHYS     0x14000000u   // GPE0 +0x00, GPF0 +0x40, GPF1 +0x60
#define CAM_GPIO_B_PHYS      0x14010000u   // GPH0 +0x100
#define CAM_GPIO_RT_PHYS     0x13400000u   // GPY7 +0x000 (front camera AVDD enable, ID)
#define CAM_CMU_TOP_PHYS     0x10020000u   // SCLK_SRC_ISP +0x270, SCLK_DIV_ISP +0x580, GATE_TOP_SCLK_ISP +0x870

//
// MAX77802 LDOnCTRL1 = 0x60 + n - 1: bits 7:6 mode (3 = on), 5:0 code,
// P-channel LDOs 800 mV + 50 mV x code.
//
#define CAM_PMIC_LDO18       0x71          // cam_isp_sensor_io_1.8v
#define CAM_PMIC_LDO24       0x77          // cam_sensor_core (1.05..1.2 V, apply_uV -> 1.05 V)
#define CAM_PMIC_LDO19       0x72          // vt_cam_1.8v (front sensor DVDD, 1.8 V)

typedef INTN (*CAM_PMIC_READ)(VOID *Context, UINT8 Reg, UINT8 *Value);
typedef INTN (*CAM_PMIC_WRITE)(VOID *Context, UINT8 Reg, UINT8 Value);

typedef struct {
  // filled by the caller
  UINTN           GpioLb;      // CAM_GPIO_LB_PHYS, 0x1000
  UINTN           GpioB;       // CAM_GPIO_B_PHYS, 0x1000
  UINTN           GpioRt;      // CAM_GPIO_RT_PHYS, 0x1000 (front camera only)
  UINTN           CmuTop;      // CAM_CMU_TOP_PHYS, 0x1000
  CAM_PMIC_READ   PmicRead;
  CAM_PMIC_WRITE  PmicWrite;
  VOID            *PmicContext;
  UINT32          MclkDiv;     // SCLK_DIV_ISP sensor0 divider from IPLL (0 = 24: 432 / 24 = 18 MHz, stock V1A)
  // state
  UINT32          Step;
  UINT8           Ldo18Before;
  UINT8           Ldo24Before;
  UINT8           Ldo19Before;
  BOOLEAN         On;
  BOOLEAN         I2cDrive;     // max drive strength on the front I2C pins (kernel does this for SDA/SCL)
  BOOLEAN         I2cPullUp;    // internal pull-ups on them (experiment)
  BOOLEAN         FrontExtraIo; // also enable LDO18 (rear I/O rail) for the front module (experiment)
  UINT32          I2cProbe;     // GPF0 DAT bits 5:4 read with the I2C pins as inputs (1 = pulled high)
  UINT32          FrontRails;   // GPY7 con, dat, LDO19 for logs
} CAM_BOARD;

//
// Rear module on, ending with the reset released and the ISP I2C0/1 and
// SPI0 pins handed to the ISP.  0 or < 0 (Board->Step says where).
//
INTN
CamRearPowerOn (
  IN OUT CAM_BOARD  *Board
  );

//
// Front module (S5K6B2 on CSI-B, ISP I2C2): AVDD GPY7(0), LDO19, reset
// GPE0(4), ID GPY7(2), MCLK sclk_isp_sensor2 on GPH0(7), I2C GPF0(4/5).
//
INTN
CamFrontPowerOn (
  IN OUT CAM_BOARD  *Board
  );

VOID
CamFrontPowerOff (
  IN OUT CAM_BOARD  *Board
  );

VOID
CamRearPowerOff (
  IN OUT CAM_BOARD  *Board
  );

//
// GPE0 / GPF0 / GPF1 / GPH0 CON+DAT and the clock registers, for logs.
//
typedef struct {
  UINT32  Gpe0Con, Gpe0Dat, Gpf0Con, Gpf1Con, Gpf1Dat, Gph0Con;
  UINT32  SclkSrcIsp, SclkDivIsp, GateTopSclkIsp;
  UINT8   Ldo18, Ldo24;
} CAM_BOARD_REGS;

VOID
CamBoardSnapshot (
  IN  CAM_BOARD       *Board,
  OUT CAM_BOARD_REGS  *Regs
  );

#endif // __CAM_BOARD_H__
