/** @file
  Exynos5420 FIMC-IS core: see FimcIsCore.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "FimcIsCore.h"

//
// PMU (fimc-is-regs.h, 5420 values; pm_domains5.c)
//
#define PMU_LOW_POWER_OFF        0x0004
#define PMU_ISP_ARM_SYS_PWR      0x1090
#define PMU_CMU_SYSCLK_ISP_SYS   0x14C4
#define PMU_CMU_RESET_ISP_SYS    0x1584
#define PMU_ISP_ARM_CONFIG       0x2480
#define PMU_ISP_ARM_STATUS       0x2484
#define PMU_ISP_ARM_OPTION       0x2488
#define   ARM_OPTION_ENABLE        0x00018000u
#define   ARM_OPTION_STOP_STARTED  0x00010000u
#define PMU_ISP_CONFIG           0x4020
#define PMU_ISP_STATUS           0x4024
#define   PD_ON                    0x7

//
// CMU (regs-clock.h with CONFIG_SOC_EXYNOS5420; clock-exynos5420.c)
//
#define CMU_DIV_ISP0             0xC300   // aclk_div0 [2:0], aclk_div1 [6:4]
#define CMU_DIV_ISP1             0xC304   // aclk_mcuisp_div0 [2:0], _div1 [6:4]
#define CMU_DIV_ISP2             0xC308   // aclk_div2 [2:0]
#define CMU_GATE_IP_ISP0         0xC800
#define CMU_GATE_IP_ISP1         0xC804
#define CMU_SRC_TOP1             0x10204
#define CMU_SRC_TOP3             0x1020C  // aclk_400_isp user mux bit 0
#define CMU_SRC_TOP4             0x10210  // aclk_333_432_isp bit 4, _isp0 bit 12, aclk_266_isp bit 16
#define CMU_SRC_TOP10            0x10280
#define CMU_SRC_TOP11            0x10284
#define CMU_DIV_TOP0             0x10500
#define CMU_DIV_TOP1             0x10504
#define CMU_GATE_IP_GSCL0        0x10910

// clk_on: isp0_333_432, isp0_400, isp0_266 / isp1_333_432, isp1_266 / gscl_3aa
#define GATE_ISP0_ON             ((0x3FFFFu << 14) | 0x1Fu | (0x7u << 5) | (1u << 3) | (1u << 1))
#define GATE_ISP1_ON             ((0x3Fu << 17) | (0x3u << 12) | (0xFu << 8) | 0x7u)
#define GATE_GSCL0_3AA           ((1u << 19) | (1u << 9) | (1u << 4))

//
// SYSREG (tdnr_s3d_pixel_async_sw_reset)
//
#define SYSREG_GSCBLK_CFG1       0x224    // bit 25: S3D pixel async reset (active low)
#define SYSREG_ISPBLK_CFG        0x22C    // bit 5: 3DNR pixel async reset

//
// MCUCTL and the shared registers (fimc-is-regs.h, fimc-is-cmd.h)
//
#define MCUCTL                   0x180000
#define MCUCTLR                  (MCUCTL + 0x00)
#define BBOAR                    (MCUCTL + 0x04)
#define INTGR0                   (MCUCTL + 0x08)
#define INTMSR0                  (MCUCTL + 0x18)
#define INTCR1                   (MCUCTL + 0x20)
#define INTMR1                   (MCUCTL + 0x24)
#define INTMSR1                  (MCUCTL + 0x2C)
#define ISSR0                    (MCUCTL + 0x80)
#define ISSR6                    (ISSR0 + 6 * 4)
#define COM_HICMD                (ISSR0 + 0x00)
#define COM_HIC_SENSORID         (ISSR0 + 0x04)
#define COM_HIC_PARAM1           (ISSR0 + 0x08)
#define COM_IHCMD_IFLAG          (ISSR0 + 0x24)
#define COM_IHCMD                (ISSR0 + 0x28)
#define COM_IHC_SENSORID         (ISSR0 + 0x2C)
#define COM_IHC_PARAM1           (ISSR0 + 0x30)
#define INTR_GENERAL             0

STATIC
VOID
SetField (
  UINTN   Reg,
  UINT32  Shift,
  UINT32  Bits,
  UINT32  Value
  )
{
  UINT32  Mask = ((1u << Bits) - 1) << Shift;

  MmioWrite32 (Reg, (MmioRead32 (Reg) & ~Mask) | ((Value << Shift) & Mask));
}

STATIC
BOOLEAN
WaitBits (
  UINTN   Reg,
  UINT32  Mask,
  UINT32  Value,
  UINTN   TimeoutUs
  )
{
  while ((MmioRead32 (Reg) & Mask) != Value) {
    if (TimeoutUs-- == 0) {
      return FALSE;
    }
    MicroSecondDelay (1);
  }
  return TRUE;
}

VOID
FimcIsSnapshot (
  IN  FIMC_IS       *Is,
  OUT FIMC_IS_REGS  *Regs
  )
{
  Regs->PdConfig    = MmioRead32 (Is->Pmu + PMU_ISP_CONFIG);
  Regs->PdStatus    = MmioRead32 (Is->Pmu + PMU_ISP_STATUS);
  Regs->LowPowerOff = MmioRead32 (Is->Pmu + PMU_LOW_POWER_OFF);
  Regs->ArmConfig   = MmioRead32 (Is->Pmu + PMU_ISP_ARM_CONFIG);
  Regs->ArmStatus   = MmioRead32 (Is->Pmu + PMU_ISP_ARM_STATUS);
  Regs->ArmOption   = MmioRead32 (Is->Pmu + PMU_ISP_ARM_OPTION);
  Regs->SrcTop1     = MmioRead32 (Is->Cmu + CMU_SRC_TOP1);
  Regs->SrcTop3     = MmioRead32 (Is->Cmu + CMU_SRC_TOP3);
  Regs->SrcTop4     = MmioRead32 (Is->Cmu + CMU_SRC_TOP4);
  Regs->SrcTop10    = MmioRead32 (Is->Cmu + CMU_SRC_TOP10);
  Regs->SrcTop11    = MmioRead32 (Is->Cmu + CMU_SRC_TOP11);
  Regs->DivTop0     = MmioRead32 (Is->Cmu + CMU_DIV_TOP0);
  Regs->DivTop1     = MmioRead32 (Is->Cmu + CMU_DIV_TOP1);
  Regs->GateGscl0   = MmioRead32 (Is->Cmu + CMU_GATE_IP_GSCL0);
  //
  // The ISP block's CMU and MCUCTL sit in the ISP domain: only readable
  // while it is on.
  //
  if ((Regs->PdStatus & PD_ON) == PD_ON) {
    Regs->DivIsp0  = MmioRead32 (Is->Cmu + CMU_DIV_ISP0);
    Regs->DivIsp1  = MmioRead32 (Is->Cmu + CMU_DIV_ISP1);
    Regs->DivIsp2  = MmioRead32 (Is->Cmu + CMU_DIV_ISP2);
    Regs->GateIsp0 = MmioRead32 (Is->Cmu + CMU_GATE_IP_ISP0);
    Regs->GateIsp1 = MmioRead32 (Is->Cmu + CMU_GATE_IP_ISP1);
    Regs->Mcuctlr  = MmioRead32 (Is->Is + MCUCTLR);
    Regs->Bboar    = MmioRead32 (Is->Is + BBOAR);
    Regs->Intmsr1  = MmioRead32 (Is->Is + INTMSR1);
    Regs->Issr6    = MmioRead32 (Is->Is + ISSR6);
  } else {
    Regs->DivIsp0 = Regs->DivIsp1 = Regs->DivIsp2 = Regs->GateIsp0 = Regs->GateIsp1 = ~0u;
    Regs->Mcuctlr = Regs->Bboar = Regs->Intmsr1 = Regs->Issr6 = ~0u;
  }
}

INTN
FimcIsPowerUp (
  IN OUT FIMC_IS  *Is,
  IN     BOOLEAN  SetTopMux
  )
{
  //
  // 1. pd-isp (exynos_pm_domain_power_control: CONFIGURATION = 7, wait
  //    STATUS & 7).
  //
  Is->Step = 1;
  if ((MmioRead32 (Is->Pmu + PMU_ISP_STATUS) & PD_ON) != PD_ON) {
    MmioWrite32 (Is->Pmu + PMU_ISP_CONFIG, PD_ON);
    if (!WaitBits (Is->Pmu + PMU_ISP_STATUS, PD_ON, PD_ON, 50000)) {
      return -1;
    }
  }
  Is->DomainOn = TRUE;

  //
  // 2. TOP user muxes (optional), ISP dividers to the stock ratios
  //    (exynos5420_fimc_is_cfg_clk: mcuisp 200 / 100 MHz from 400,
  //    aclk_div0 / 1 216 / 108 MHz from 432, aclk_div2 54 MHz).
  //
  Is->Step = 2;
  if (SetTopMux) {
    SetField (Is->Cmu + CMU_SRC_TOP3, 0, 1, 1);
    SetField (Is->Cmu + CMU_SRC_TOP4, 4, 1, 1);
    SetField (Is->Cmu + CMU_SRC_TOP4, 12, 1, 1);
    SetField (Is->Cmu + CMU_SRC_TOP4, 16, 1, 1);
  }
  SetField (Is->Cmu + CMU_DIV_ISP1, 0, 3, 1);
  SetField (Is->Cmu + CMU_DIV_ISP1, 4, 3, 3);
  SetField (Is->Cmu + CMU_DIV_ISP0, 0, 3, 1);
  SetField (Is->Cmu + CMU_DIV_ISP0, 4, 3, 3);
  SetField (Is->Cmu + CMU_DIV_ISP2, 0, 3, 1);

  //
  // 3. Gates (exynos5420_fimc_is_clk_on, minus the per-channel CSIS /
  //    FIMC-Lite gates, which belong to the sensor path).
  //
  Is->Step = 3;
  MmioOr32 (Is->Cmu + CMU_GATE_IP_ISP0, GATE_ISP0_ON);
  MmioOr32 (Is->Cmu + CMU_GATE_IP_ISP1, GATE_ISP1_ON);
  MmioOr32 (Is->Cmu + CMU_GATE_IP_GSCL0, GATE_GSCL0_3AA);
  return 0;
}

INTN
FimcIsLoadFirmware (
  IN OUT FIMC_IS     *Is,
  IN     CONST VOID  *Fw,
  IN     UINTN       Size
  )
{
  Is->Step = 4;
  if ((Size == 0) || (Size > FIMC_IS_SHARED_OFFSET)) {
    return -1;
  }
  ZeroMem (Is->MemVa, FIMC_IS_A5_MEM_SIZE);
  CopyMem (Is->MemVa, Fw, Size);
  EnvCacheClean (Is->MemVa, FIMC_IS_A5_MEM_SIZE);
  return 0;
}

INTN
FimcIsStartA5 (
  IN OUT FIMC_IS  *Is
  )
{
  UINTN   Index;
  UINT32  Value;

  //
  // 1. Pixel async bridges out of reset (the kernel clears the bits).
  //
  Is->Step = 5;
  MmioAnd32 (Is->Sysreg + SYSREG_GSCBLK_CFG1, ~(1u << 25));
  MmioAnd32 (Is->Sysreg + SYSREG_ISPBLK_CFG, ~(1u << 5));

  //
  // 2. A5 boot address.
  //
  Is->Step = 6;
  MmioWrite32 (Is->Is + BBOAR, Is->MemPa);

  //
  // 3. Secure configuration (CONFIG_ARM_TRUSTZONE path): ISP security
  //    and the bus masters' secure/non-secure attributes, then the
  //    readback the kernel checks.
  //
  Is->Step = 7;
  Is->SecureCheck = ~0u;
  if (Is->SecureWrite != NULL) {
    if (Is->SecureWrite (0x131E0004, 0x000000FF) != 0) {
      return -7;
    }
    for (Index = 0; Index < 3; Index++) {
      Is->SecureWrite (0x131F0080 + (UINT32)Index * 4, 0xFFFFFFFF);
    }
    for (Index = 0; Index < 18; Index++) {
      Is->SecureWrite (0x131F0400 + (UINT32)Index * 4, 0x10101010);
    }
    if ((Is->SecureRead != NULL) && (Is->SecureRead (0x131E0004, &Value) == 0)) {
      Is->SecureCheck = Value;
    }
  }

  //
  // 4. A5 power on, enable, wait for its status.
  //
  Is->Step = 8;
  MmioWrite32 (Is->Pmu + PMU_ISP_ARM_CONFIG, 1);
  MmioWrite32 (Is->Pmu + PMU_ISP_ARM_OPTION, ARM_OPTION_ENABLE);
  if (!WaitBits (Is->Pmu + PMU_ISP_ARM_STATUS, 1, 1, 1000)) {
    return -8;
  }
  Is->A5On = TRUE;
  return 0;
}

BOOLEAN
FimcIsTakeMessage (
  IN  FIMC_IS      *Is,
  OUT FIMC_IS_MSG  *Msg
  )
{
  UINTN  Index;

  if (((MmioRead32 (Is->Is + INTMSR1) & (1u << INTR_GENERAL)) == 0) &&
      (MmioRead32 (Is->Is + COM_IHCMD_IFLAG) == 0)) {
    return FALSE;
  }
  Msg->Command  = MmioRead32 (Is->Is + COM_IHCMD);
  Msg->SensorId = MmioRead32 (Is->Is + COM_IHC_SENSORID);
  for (Index = 0; Index < 4; Index++) {
    Msg->Param[Index] = MmioRead32 (Is->Is + COM_IHC_PARAM1 + Index * 4);
  }
  // fimc_is_clr_intr(INTR_GENERAL)
  MmioWrite32 (Is->Is + INTCR1, 1u << INTR_GENERAL);
  MmioWrite32 (Is->Is + COM_IHCMD_IFLAG, 0);
  return TRUE;
}

INTN
FimcIsWaitMessage (
  IN  FIMC_IS      *Is,
  IN  UINTN        TimeoutMs,
  OUT FIMC_IS_MSG  *Msg
  )
{
  UINTN  Us = TimeoutMs * 1000;

  while (!FimcIsTakeMessage (Is, Msg)) {
    if (Us < 100) {
      return -1;
    }
    MicroSecondDelay (100);
    Us -= 100;
  }
  return 0;
}

INTN
FimcIsSend (
  IN FIMC_IS  *Is,
  IN UINT32   Command,
  IN UINT32   SensorId,
  IN UINT32   Param1,
  IN UINT32   Param2,
  IN UINT32   Param3,
  IN UINT32   Param4
  )
{
  // waiting_is_ready(): TRY_RECV_AWARE_COUNT x 100 us
  if (!WaitBits (Is->Is + INTMSR0, 1, 0, 100000)) {
    return -1;
  }
  MmioWrite32 (Is->Is + COM_HICMD, Command);
  MmioWrite32 (Is->Is + COM_HIC_SENSORID, SensorId);
  MmioWrite32 (Is->Is + COM_HIC_PARAM1, Param1);
  MmioWrite32 (Is->Is + COM_HIC_PARAM1 + 4, Param2);
  MmioWrite32 (Is->Is + COM_HIC_PARAM1 + 8, Param3);
  MmioWrite32 (Is->Is + COM_HIC_PARAM1 + 12, Param4);
  MmioWrite32 (Is->Is + INTGR0, 1);
  return 0;
}

VOID
FimcIsPowerDown (
  IN OUT FIMC_IS  *Is
  )
{
  if (Is->A5On) {
    //
    // fimc_is_ischain_power(0): STOP_STARTED while the firmware runs (it
    // should be in WFI after HIC_POWER_DOWN), then everything off.
    //
    MmioWrite32 (Is->Pmu + PMU_ISP_ARM_OPTION, ARM_OPTION_STOP_STARTED);
    MicroSecondDelay (1000);
    MmioWrite32 (Is->Pmu + PMU_ISP_ARM_OPTION, 0);
    MmioWrite32 (Is->Pmu + PMU_ISP_ARM_CONFIG, 0);
    WaitBits (Is->Pmu + PMU_ISP_ARM_STATUS, 1, 0, 1000);
    Is->A5On = FALSE;
  }
  MmioWrite32 (Is->Pmu + PMU_CMU_RESET_ISP_SYS, 0);
  MmioWrite32 (Is->Pmu + PMU_CMU_SYSCLK_ISP_SYS, 0);
  MmioWrite32 (Is->Pmu + PMU_ISP_ARM_SYS_PWR, 0);
  if (Is->DomainOn) {
    MmioWrite32 (Is->Pmu + PMU_ISP_CONFIG, 0);
    WaitBits (Is->Pmu + PMU_ISP_STATUS, PD_ON, 0, 50000);
    Is->DomainOn = FALSE;
  }
}
