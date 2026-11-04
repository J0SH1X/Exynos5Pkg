/** @file
  v1awifi camera module power: see CamBoard.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "CamBoard.h"

#define GPE0               0x000            // in GpioLb
#define GPF0               0x040
#define GPF1               0x060
#define GPH0               0x100            // in GpioB
#define GPIO_CON           0x0
#define GPIO_DAT           0x4
#define GPIO_PUD           0x8

#define FUNC_INPUT         0x0
#define FUNC_OUTPUT        0x1
#define FUNC_2             0x2

#define SCLK_SRC_ISP       0x270            // sclk_isp_sensor0..2 mux [30:28]: 5 = IPLL
#define SCLK_DIV_ISP       0x580            // sensor0 [15:8]
#define GATE_TOP_SCLK_ISP  0x870            // sensor0 bit 4
#define SRC_IPLL           5

#define LDO_ON             (3u << 6)
#define LDO_CODE(mV)       ((UINT8)(((mV) - 800) / 50))

STATIC
VOID
PinFunc (
  UINTN   Bank,
  UINT32  Pin,
  UINT32  Func
  )
{
  MmioAndThenOr32 (Bank + GPIO_CON, ~(0xFu << (Pin * 4)), Func << (Pin * 4));
  MmioAnd32 (Bank + GPIO_PUD, ~(3u << (Pin * 2)));          // pull none
}

STATIC
VOID
PinOut (
  UINTN   Bank,
  UINT32  Pin,
  UINT32  Value
  )
{
  if (Value) {
    MmioOr32 (Bank + GPIO_DAT, 1u << Pin);
  } else {
    MmioAnd32 (Bank + GPIO_DAT, ~(1u << Pin));
  }
  PinFunc (Bank, Pin, FUNC_OUTPUT);
}

STATIC
INTN
Ldo (
  CAM_BOARD  *Board,
  UINT8      Reg,
  UINT8      Value
  )
{
  return Board->PmicWrite (Board->PmicContext, Reg, Value);
}

VOID
CamBoardSnapshot (
  IN  CAM_BOARD       *Board,
  OUT CAM_BOARD_REGS  *Regs
  )
{
  Regs->Gpe0Con        = MmioRead32 (Board->GpioLb + GPE0 + GPIO_CON);
  Regs->Gpe0Dat        = MmioRead32 (Board->GpioLb + GPE0 + GPIO_DAT);
  Regs->Gpf0Con        = MmioRead32 (Board->GpioLb + GPF0 + GPIO_CON);
  Regs->Gpf1Con        = MmioRead32 (Board->GpioLb + GPF1 + GPIO_CON);
  Regs->Gpf1Dat        = MmioRead32 (Board->GpioLb + GPF1 + GPIO_DAT);
  Regs->Gph0Con        = MmioRead32 (Board->GpioB + GPH0 + GPIO_CON);
  Regs->SclkSrcIsp     = MmioRead32 (Board->CmuTop + SCLK_SRC_ISP);
  Regs->SclkDivIsp     = MmioRead32 (Board->CmuTop + SCLK_DIV_ISP);
  Regs->GateTopSclkIsp = MmioRead32 (Board->CmuTop + GATE_TOP_SCLK_ISP);
  Regs->Ldo18 = Regs->Ldo24 = 0xFF;
  Board->PmicRead (Board->PmicContext, CAM_PMIC_LDO18, &Regs->Ldo18);
  Board->PmicRead (Board->PmicContext, CAM_PMIC_LDO24, &Regs->Ldo24);
}

INTN
CamRearPowerOn (
  IN OUT CAM_BOARD  *Board
  )
{
  UINT32  Div = (Board->MclkDiv != 0) ? Board->MclkDiv : 24;

  Board->Step = 1;
  Board->PmicRead (Board->PmicContext, CAM_PMIC_LDO18, &Board->Ldo18Before);
  Board->PmicRead (Board->PmicContext, CAM_PMIC_LDO24, &Board->Ldo24Before);

  //
  // Reset held (GPE0(5) low) while the rails come up.
  //
  PinOut (Board->GpioLb + GPE0, 5, 0);
  Board->On = TRUE;

  // 1. AVDD 2.8 V: GPE0(7) GPIO_CAM_IO_EN
  PinOut (Board->GpioLb + GPE0, 7, 1);
  MicroSecondDelay (1000);

  // 2. DVDD core: LDO24 1.05 V
  Board->Step = 2;
  if (Ldo (Board, CAM_PMIC_LDO24, LDO_ON | LDO_CODE (1050)) < 0) {
    return -2;
  }
  MicroSecondDelay (1000);

  // 3. AF 2.8 V: GPE0(6) GPIO_CAM_AF_EN
  PinOut (Board->GpioLb + GPE0, 6, 1);
  MicroSecondDelay (1000);

  // 4. host / IO 1.8 V: LDO18
  Board->Step = 4;
  if (Ldo (Board, CAM_PMIC_LDO18, LDO_ON | LDO_CODE (1800)) < 0) {
    return -4;
  }
  MicroSecondDelay (1000);

  //
  // 5. MCLK: sclk_isp_sensor0 from IPLL / Div (stock V1A: 18 MHz), gate on,
  //    GPH0(5) to CIS_CLK0 (function 2).
  //
  Board->Step = 5;
  MmioAndThenOr32 (Board->CmuTop + SCLK_SRC_ISP, ~(7u << 28), SRC_IPLL << 28);
  MmioAndThenOr32 (Board->CmuTop + SCLK_DIV_ISP, ~(0xFFu << 8), (Div - 1) << 8);
  MmioOr32 (Board->CmuTop + GATE_TOP_SCLK_ISP, 1u << 4);
  PinFunc (Board->GpioB + GPH0, 5, FUNC_2);
  MicroSecondDelay (1000);

  // 6. reset released
  Board->Step = 6;
  PinOut (Board->GpioLb + GPE0, 5, 1);
  MicroSecondDelay (100);

  //
  // 7. The ISP's pins: I2C0 (sensor) GPF0(0/1), I2C1 (AF) GPF0(2/3),
  //    SPI0 (module F-ROM) GPF1(0/2/3), chip select GPF1(1) as a GPIO, high.
  //
  Board->Step = 7;
  PinFunc (Board->GpioLb + GPF0, 0, FUNC_2);
  PinFunc (Board->GpioLb + GPF0, 1, FUNC_2);
  PinFunc (Board->GpioLb + GPF0, 2, FUNC_2);
  PinFunc (Board->GpioLb + GPF0, 3, FUNC_2);
  MmioOr32 (Board->GpioLb + GPF0 + 0xC, 0xFFu);                                // max drive: SDA / SCL of ISP I2C0 / I2C1
  PinFunc (Board->GpioLb + GPF1, 0, FUNC_2);
  PinOut (Board->GpioLb + GPF1, 1, 1);
  PinFunc (Board->GpioLb + GPF1, 2, FUNC_2);
  PinFunc (Board->GpioLb + GPF1, 3, FUNC_2);
  return 0;
}

VOID
CamRearPowerOff (
  IN OUT CAM_BOARD  *Board
  )
{
  if (!Board->On) {
    return;
  }
  PinOut (Board->GpioLb + GPE0, 5, 0);                       // reset
  PinFunc (Board->GpioLb + GPF0, 0, FUNC_INPUT);
  PinFunc (Board->GpioLb + GPF0, 1, FUNC_INPUT);
  PinFunc (Board->GpioLb + GPF0, 2, FUNC_INPUT);
  PinFunc (Board->GpioLb + GPF0, 3, FUNC_INPUT);
  PinFunc (Board->GpioLb + GPF1, 0, FUNC_INPUT);
  PinFunc (Board->GpioLb + GPF1, 1, FUNC_INPUT);
  PinFunc (Board->GpioLb + GPF1, 2, FUNC_INPUT);
  PinFunc (Board->GpioLb + GPF1, 3, FUNC_INPUT);
  PinFunc (Board->GpioB + GPH0, 5, FUNC_INPUT);              // MCLK
  MmioAnd32 (Board->CmuTop + GATE_TOP_SCLK_ISP, ~(1u << 4));
  Ldo (Board, CAM_PMIC_LDO18, Board->Ldo18Before);
  PinOut (Board->GpioLb + GPE0, 6, 0);                       // AF
  Ldo (Board, CAM_PMIC_LDO24, Board->Ldo24Before);
  PinOut (Board->GpioLb + GPE0, 7, 0);                       // AVDD
  Board->On = FALSE;
}

//
// Front camera (board-universal5420-media.c, FLITE_ID_B entries, in table order).
//
#define GPY7               0x000            // in GpioRt
#define SCLK_SENSOR2_SHIFT 24               // SCLK_DIV_ISP [31:24]
#define SCLK_SENSOR2_GATE  (1u << 12)

INTN
CamFrontPowerOn (
  IN OUT CAM_BOARD  *Board
  )
{
  UINT32  Div = (Board->MclkDiv != 0) ? Board->MclkDiv : 24;

  Board->Step = 1;
  Board->PmicRead (Board->PmicContext, CAM_PMIC_LDO19, &Board->Ldo19Before);
  PinOut (Board->GpioLb + GPE0, 4, 0);                       // reset held
  Board->On = TRUE;

  PinOut (Board->GpioRt + GPY7, 0, 1);                       // AVDD 2.8 V
  MicroSecondDelay (1000);
  if (Board->FrontExtraIo) {
    Board->PmicRead (Board->PmicContext, CAM_PMIC_LDO18, &Board->Ldo18Before);
    Ldo (Board, CAM_PMIC_LDO18, LDO_ON | LDO_CODE (1800));
    MicroSecondDelay (1000);
  }

  Board->Step = 2;
  if (Ldo (Board, CAM_PMIC_LDO19, LDO_ON | LDO_CODE (1800)) < 0) {  // DVDD 1.8 V
    return -2;
  }
  MicroSecondDelay (1000);

  PinOut (Board->GpioRt + GPY7, 2, 0);                       // VT_CAM_ID output low

  Board->Step = 5;
  MmioAndThenOr32 (Board->CmuTop + SCLK_SRC_ISP, ~(7u << 28), SRC_IPLL << 28);
  MmioAndThenOr32 (Board->CmuTop + SCLK_DIV_ISP, ~(0xFFu << SCLK_SENSOR2_SHIFT), (Div - 1) << SCLK_SENSOR2_SHIFT);
  MmioOr32 (Board->CmuTop + GATE_TOP_SCLK_ISP, SCLK_SENSOR2_GATE);
  PinFunc (Board->GpioB + GPH0, 7, FUNC_2);                  // MCLK
  MicroSecondDelay (1000);

  Board->Step = 6;
  PinOut (Board->GpioLb + GPE0, 4, 1);                       // reset released
  MicroSecondDelay (1000);

  Board->Step = 7;
  {
    UINT8  Ldo = 0xFF;

    PinFunc (Board->GpioLb + GPF0, 4, FUNC_INPUT);
    PinFunc (Board->GpioLb + GPF0, 5, FUNC_INPUT);
    MicroSecondDelay (1000);
    Board->I2cProbe = (MmioRead32 (Board->GpioLb + GPF0 + GPIO_DAT) >> 4) & 3;
    Board->PmicRead (Board->PmicContext, CAM_PMIC_LDO19, &Ldo);
    Board->FrontRails = (MmioRead32 (Board->GpioRt + GPY7 + GPIO_DAT) & 0xFF) | ((UINT32)Ldo << 8);
  }
  PinFunc (Board->GpioLb + GPF0, 4, FUNC_2);                 // ISP I2C2
  PinFunc (Board->GpioLb + GPF0, 5, FUNC_2);
  //
  // The kernel sets the maximum drive strength on SDA / SCL pins: without it the
  // front sensor's long flex gives "I2C_WaitForBusReady timeout" in the A5.
  //
  MmioOr32 (Board->GpioLb + GPF0 + 0xC, (3u << 8) | (3u << 10));          // S5P_GPIO_DRVSTR_LV4
  if (Board->I2cPullUp) {
    MmioAndThenOr32 (Board->GpioLb + GPF0 + GPIO_PUD, ~((3u << 8) | (3u << 10)), (3u << 8) | (3u << 10));
  }
  return 0;
}

VOID
CamFrontPowerOff (
  IN OUT CAM_BOARD  *Board
  )
{
  if (!Board->On) {
    return;
  }
  PinOut (Board->GpioLb + GPE0, 4, 0);
  PinFunc (Board->GpioLb + GPF0, 4, FUNC_INPUT);
  PinFunc (Board->GpioLb + GPF0, 5, FUNC_INPUT);
  PinFunc (Board->GpioB + GPH0, 7, FUNC_INPUT);
  MmioAnd32 (Board->CmuTop + GATE_TOP_SCLK_ISP, ~SCLK_SENSOR2_GATE);
  Ldo (Board, CAM_PMIC_LDO19, Board->Ldo19Before);
  if (Board->FrontExtraIo) {
    Ldo (Board, CAM_PMIC_LDO18, Board->Ldo18Before);
  }
  PinOut (Board->GpioRt + GPY7, 0, 0);
  Board->On = FALSE;
}
