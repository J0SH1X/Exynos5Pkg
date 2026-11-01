/** @file
  Raise the Cortex-A15 cluster clock from the bootloader's 800 MHz to
  CPU_TARGET_MHZ (1.0 GHz for now; 1.8 GHz is tested too) early in DXE,
  for UEFI and everything booted from it (the OS keeps the setting until
  a cpufreq driver of its own takes over).

  Values from the v1awifi kernel (exynos5420-cpufreq.c, asv-exynos5420.h):
    - 1800 MHz: APLL M/P/S 225/3/0, CLK_DIV_CPU0 { CPUD 4, ATB 7,
      PCLK_DBG 7, APLL 3, ARM2 0 }; 1000 MHz: 250/3/1, { 2, 6, 6, 3, 0 };
      CLK_DIV_CPU1 { COPY 7, HPM 7 } for both.
    - VDD_ARM: the slowest ASV group's (group 0) voltage for the level; we
      do not read the chip's ASV group, so take that plus 25 mV of margin.
    - EMA: the "WAS" assist (ARM_EMA_CTRL bit 12) is only for VDD_ARM at
      or below 950 mV - cleared before going above it.

  The sequence follows exynos5420_set_frequency_CA15(): voltage first, then
  the dividers, then the APLL relock with the core parked on MX_MSPLL_CPU
  (the bootloader routes SPLL, 400 MHz, there), then back to the APLL.

  VDD_ARM is MAX77802 BUCK2 (hs-i2c3 = bus 7, address 0x09).  The kernel
  disables GPIO-DVS and programs all eight DVS registers with the same
  value, so we do the same (whichever register the DVS pins select gets the
  new level).  If the PMIC cannot be programmed and read back, the clock
  stays where it is.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosI2c.h>

//
// CMU_CPU (regs-clock.h: S5P_VA_CMU = 0x10010000 on 5420).
//
#define APLL_LOCK          0x10010000
#define APLL_CON0          0x10010100
#define CLK_SRC_CPU        0x10010200
#define CLK_MUX_STAT_CPU   0x10010400
#define CLK_DIV_CPU0       0x10010500
#define CLK_DIV_CPU1       0x10010504
#define CLK_DIV_STAT_CPU0  0x10010600
#define CLK_DIV_STAT_CPU1  0x10010604
#define ARM_EMA_CTRL       0x10011008
#define ARM_WAS_ENABLE     (1u << 12)

//
// CMU_TOP: MX_MSPLL_CPU source select and the PLLs it can pick.
//
#define CLK_SRC_TOP7       0x1002021C
#define MX_MSPLL_CPU(s)    (((s) >> 12) & 0x3)   // 0 CPLL, 1 DPLL, 2 MPLL, 3 SPLL
STATIC CONST UINT32  mMsPllCon0[4] = { 0x10020120, 0x10020128, 0x10020180, 0x10020160 };
STATIC CONST CHAR8   *mMsPllName[4] = { "CPLL", "DPLL", "MPLL", "SPLL" };

#define PLL_PMS_MASK       ((0x3FFu << 16) | (0x3Fu << 8) | 0x7u)
#define PLL_M(c)           (((c) >> 16) & 0x3FF)
#define PLL_P(c)           (((c) >> 8) & 0x3F)
#define PLL_S(c)           ((c) & 0x7)
#define PLL_LOCKED         (1u << 29)
#define XTAL_HZ            24000000u

#define MUX_CPU_SEL        (1u << 16)    // mout_cpu: 0 MOUT_APLL, 1 MX_MSPLL_CPU
#define MUX_HPM_SEL        (1u << 20)    // mout_hpm_cpu: same choice
#define MUX_CPU_STAT(s)    (((s) >> 16) & 0x7)
#define MUX_STAT_APLL      1
#define MUX_STAT_MSPLL     2

//
// Operating points (kernel L6 / L14).  VDD_ARM = ASV group 0 + 25 mV.
//
#define CPU_DIV0(Cpud, Atb, PclkDbg, Apll, Arm2)  \
  (((Cpud) << 4) | ((Atb) << 16) | ((PclkDbg) << 20) | ((Apll) << 24) | ((Arm2) << 28))

#define CPU_TARGET_MHZ     1000u

#if CPU_TARGET_MHZ == 1800
#define TARGET_APLL_PMS    ((225u << 16) | (3u << 8) | 0u)
#define TARGET_DIV_CPU0    CPU_DIV0 (4u, 7u, 7u, 3u, 0u)
#define TARGET_VDD_ARM_UV  1250000u      // group 0: 1225000
#elif CPU_TARGET_MHZ == 1000
#define TARGET_APLL_PMS    ((250u << 16) | (3u << 8) | 1u)
#define TARGET_DIV_CPU0    CPU_DIV0 (2u, 6u, 6u, 3u, 0u)
#define TARGET_VDD_ARM_UV  1012500u      // group 0:  987500
#else
#error "no operating point for CPU_TARGET_MHZ"
#endif

#define TARGET_MHZ         CPU_TARGET_MHZ
#define DIV_CPU0_MASK      CPU_DIV0 (0x7u, 0x7u, 0x7u, 0x7u, 0x7u)
#define DIV_CPU1_MASK      ((0x7u << 0) | (0x7u << 4))
#define TARGET_DIV_CPU1    ((7u << 0) | (7u << 4))
#define EMA_BASE_UV        950000u

//
// MAX77802 BUCK2 (vdd_arm): DVS1..8 at 0x1D..0x24, 600 mV + 6.25 mV steps.
// Ramp: BUCK2CTRL1[7:6], board file selects 25 mV/us.
//
#define PMIC_I2C_BUS       7
#define PMIC_I2C_ADDR      0x09
#define PMIC_SDA_GPIO      EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB2, 2)
#define PMIC_SCL_GPIO      EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB2, 3)
#define PMIC_BUCK2DVS1     0x1D
#define PMIC_BUCK2_DVS     8
#define BUCK2_CODE(uV)     ((UINT8)(((uV) - 600000u) / 6250u))
#define BUCK2_UV(code)     (600000u + (UINT32)(code) * 6250u)

STATIC
UINT32
PllHz (
  IN UINT32  Con
  )
{
  if (PLL_P (Con) == 0) {
    return 0;
  }
  return (UINT32)(((UINT64)XTAL_HZ * PLL_M (Con)) / ((UINT64)PLL_P (Con) << PLL_S (Con)));
}

STATIC
UINT32
ArmClockHz (
  VOID
  )
{
  UINT32  Mux;
  UINT32  Div;
  UINT32  Hz;

  Mux = MUX_CPU_STAT (MmioRead32 (CLK_MUX_STAT_CPU));
  if (Mux == MUX_STAT_APLL) {
    Hz = PllHz (MmioRead32 (APLL_CON0));
  } else if (Mux == MUX_STAT_MSPLL) {
    Hz = PllHz (MmioRead32 (mMsPllCon0[MX_MSPLL_CPU (MmioRead32 (CLK_SRC_TOP7))]));
  } else {
    return 0;
  }
  Div = MmioRead32 (CLK_DIV_CPU0);
  return Hz / ((Div & 0x7) + 1) / (((Div >> 28) & 0x7) + 1);
}

STATIC
EFI_STATUS
ReadVddArm (
  IN  EXYNOS_I2C_PROTOCOL  *I2c,
  OUT UINT8                Level[PMIC_BUCK2_DVS]
  )
{
  if (I2c->Read (I2c, PMIC_I2C_BUS, PMIC_I2C_ADDR, PMIC_BUCK2DVS1, Level,
                 PMIC_BUCK2_DVS) != PMIC_BUCK2_DVS) {
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SetVddArm (
  IN EXYNOS_I2C_PROTOCOL  *I2c,
  IN UINT32               MicroVolts
  )
{
  UINT8       Buf[2];
  UINT8       Level[PMIC_BUCK2_DVS];
  UINTN       Index;
  EFI_STATUS  Status;

  for (Index = 0; Index < PMIC_BUCK2_DVS; Index++) {
    Buf[0] = (UINT8)(PMIC_BUCK2DVS1 + Index);
    Buf[1] = BUCK2_CODE (MicroVolts);
    if (I2c->Write (I2c, PMIC_I2C_BUS, PMIC_I2C_ADDR, Buf, 2) != 2) {
      return EFI_DEVICE_ERROR;
    }
  }

  //
  // 25 mV/us ramp: even 0.9 -> 1.25 V settles in ~15 us.  Give it 1 ms.
  //
  gBS->Stall (1000);

  Status = ReadVddArm (I2c, Level);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  for (Index = 0; Index < PMIC_BUCK2_DVS; Index++) {
    if (Level[Index] != BUCK2_CODE (MicroVolts)) {
      return EFI_DEVICE_ERROR;
    }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
WaitMuxStat (
  IN UINT32  Want
  )
{
  UINTN  Retry;

  for (Retry = 0; MUX_CPU_STAT (MmioRead32 (CLK_MUX_STAT_CPU)) != Want; Retry++) {
    if (Retry > 100000) {
      return EFI_TIMEOUT;
    }
  }
  return EFI_SUCCESS;
}

/**
  exynos5420_set_clkdiv_CA15(): CPU0 dividers, then CPU1, each waiting for
  its STAT bits.
**/
STATIC
VOID
SetCpuDividers (
  VOID
  )
{
  UINTN  Retry;

  MmioAndThenOr32 (CLK_DIV_CPU0, ~DIV_CPU0_MASK, TARGET_DIV_CPU0);
  for (Retry = 0; (Retry < 100000) && ((MmioRead32 (CLK_DIV_STAT_CPU0) & 0x11111111) != 0); Retry++) {
  }

  MmioAndThenOr32 (CLK_DIV_CPU1, ~DIV_CPU1_MASK, TARGET_DIV_CPU1);
  for (Retry = 0; (Retry < 100000) && ((MmioRead32 (CLK_DIV_STAT_CPU1) & 0x111) != 0); Retry++) {
  }
}

/**
  exynos5420_set_apll_CA15(): run the core (and HPM) from MX_MSPLL_CPU while
  the APLL relocks to new M/P/S, then switch back.
**/
STATIC
EFI_STATUS
SetApll (
  IN UINT32  Pms
  )
{
  UINTN       Retry;
  EFI_STATUS  Status;

  MmioOr32 (CLK_SRC_CPU, MUX_CPU_SEL | MUX_HPM_SEL);
  Status = WaitMuxStat (MUX_STAT_MSPLL);
  if (EFI_ERROR (Status)) {
    MmioAnd32 (CLK_SRC_CPU, ~(MUX_CPU_SEL | MUX_HPM_SEL));
    return Status;
  }

  MmioWrite32 (APLL_LOCK, PLL_P (Pms) * 250);
  MmioAndThenOr32 (APLL_CON0, ~PLL_PMS_MASK, Pms);
  for (Retry = 0; (MmioRead32 (APLL_CON0) & PLL_LOCKED) == 0; Retry++) {
    if (Retry > 1000) {
      // Stay on MX_MSPLL_CPU rather than switch to an unlocked PLL.
      return EFI_TIMEOUT;
    }
    gBS->Stall (10);
  }

  MmioAnd32 (CLK_SRC_CPU, ~(MUX_CPU_SEL | MUX_HPM_SEL));
  return WaitMuxStat (MUX_STAT_APLL);
}

EFI_STATUS
EFIAPI
CpuFreqDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EXYNOS_I2C_PROTOCOL   *I2c;
  EXYNOS_GPIO_PROTOCOL  *Gpio;
  UINT8                 Level[PMIC_BUCK2_DVS];
  UINT32                Con;
  UINT32                MsPll;
  EFI_STATUS            Status;

  Con   = MmioRead32 (APLL_CON0);
  MsPll = MX_MSPLL_CPU (MmioRead32 (CLK_SRC_TOP7));
  DEBUG ((DEBUG_INFO, "CpuFreq: ARMCLK %u MHz (APLL %u/%u/%u, DIV_CPU0 0x%08x DIV_CPU1 0x%08x, "
          "MX_MSPLL_CPU %a %u MHz)\n",
          ArmClockHz () / 1000000, PLL_M (Con), PLL_P (Con), PLL_S (Con),
          MmioRead32 (CLK_DIV_CPU0), MmioRead32 (CLK_DIV_CPU1),
          mMsPllName[MsPll], PllHz (MmioRead32 (mMsPllCon0[MsPll])) / 1000000));

  if ((Con & PLL_PMS_MASK) == TARGET_APLL_PMS) {
    return EFI_SUCCESS;
  }

  //
  // The relock parks the core on MX_MSPLL_CPU, at whatever that PLL runs:
  // only do it if that is neither stopped nor faster than the target.
  //
  if ((PllHz (MmioRead32 (mMsPllCon0[MsPll])) == 0) ||
      ((MmioRead32 (mMsPllCon0[MsPll]) & PLL_LOCKED) == 0) ||
      (PllHz (MmioRead32 (mMsPllCon0[MsPll])) > TARGET_MHZ * 1000000u)) {
    DEBUG ((DEBUG_ERROR, "CpuFreq: MX_MSPLL_CPU unusable for the relock, clock left alone\n"));
    return EFI_UNSUPPORTED;
  }

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&I2c);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&Gpio);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Gpio->Config (Gpio, PMIC_SDA_GPIO, 3, EXYNOS_GPIO_PULL_UP);
  Gpio->Config (Gpio, PMIC_SCL_GPIO, 3, EXYNOS_GPIO_PULL_UP);

  Status = ReadVddArm (I2c, Level);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CpuFreq: MAX77802 not answering on I2C%u, clock left alone\n", PMIC_I2C_BUS));
    return Status;
  }
  DEBUG ((DEBUG_INFO, "CpuFreq: VDD_ARM DVS1..8 %u %u %u %u %u %u %u %u mV, EMA 0x%x\n",
          BUCK2_UV (Level[0]) / 1000, BUCK2_UV (Level[1]) / 1000, BUCK2_UV (Level[2]) / 1000,
          BUCK2_UV (Level[3]) / 1000, BUCK2_UV (Level[4]) / 1000, BUCK2_UV (Level[5]) / 1000,
          BUCK2_UV (Level[6]) / 1000, BUCK2_UV (Level[7]) / 1000, MmioRead32 (ARM_EMA_CTRL)));

  Status = SetVddArm (I2c, TARGET_VDD_ARM_UV);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CpuFreq: VDD_ARM to %u mV failed - %r, clock left alone\n",
            TARGET_VDD_ARM_UV / 1000, Status));
    return Status;
  }

  //
  // exynos5420_set_ema_CA15(): no write assist above 950 mV.
  //
  if (TARGET_VDD_ARM_UV > EMA_BASE_UV) {
    MmioAnd32 (ARM_EMA_CTRL, ~ARM_WAS_ENABLE);
  }

  //
  // Raising the frequency: dividers first, then the APLL (just S when M and
  // P already match; a relock otherwise, as from the bootloader's 800).
  //
  SetCpuDividers ();
  if ((Con & (PLL_PMS_MASK & ~0x7u)) == (TARGET_APLL_PMS & ~0x7u)) {
    MmioAndThenOr32 (APLL_CON0, ~0x7u, PLL_S (TARGET_APLL_PMS));
  } else {
    Status = SetApll (TARGET_APLL_PMS);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "CpuFreq: APLL relock failed - %r (CON0 0x%08x, MUX_STAT 0x%x)\n",
              Status, MmioRead32 (APLL_CON0), MmioRead32 (CLK_MUX_STAT_CPU)));
      return Status;
    }
  }

  DEBUG ((DEBUG_INFO, "CpuFreq: ARMCLK %u MHz at %u mV (DIV_CPU0 0x%08x DIV_CPU1 0x%08x EMA 0x%x)\n",
          ArmClockHz () / 1000000, TARGET_VDD_ARM_UV / 1000, MmioRead32 (CLK_DIV_CPU0),
          MmioRead32 (CLK_DIV_CPU1), MmioRead32 (ARM_EMA_CTRL)));
  return EFI_SUCCESS;
}
