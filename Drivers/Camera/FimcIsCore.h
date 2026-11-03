/** @file
  Exynos5420 FIMC-IS (camera ISP) core: power domain, clock gates, the
  Cortex-A5 inside the ISP, its firmware and its mailbox.  Shared by UEFI
  (Application/CameraTestApp) and, later, Windows, through <ExynosEnv.h>;
  the caller maps the blocks and supplies the secure-register writes.

  Sources (Samsung 3.4 kernel, lineageos_v1awifi_defconfig):
    drivers/media/video/exynos/fimc-is-mc2/
      fimc-is-device-ischain.c  fimc_is_ischain_power(), loadfirm,
                                tdnr_s3d_pixel_async_sw_reset()
      fimc-is-regs.h            MCUCTL / ISSR / PMU / SYSREG offsets
      fimc-is-cmd.h             struct is_common_reg (mailbox), IHC_*
      fimc-is-interface.c       INTR_GENERAL handling, IHC_GET_SENSOR_NUMBER
      fimc-is-core.h            FIMC_IS_A5_MEM_SIZE, shared region / cal offsets
    arch/arm/mach-exynos/
      setup-fimc-is.c           exynos5420_fimc_is_cfg_clk / clk_on
      clock-exynos5420.c        clock name -> CMU register / bit
      pm_domains5.c             pd-isp (ISP_CONFIGURATION 0x10044020)

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __FIMC_IS_CORE_H__
#define __FIMC_IS_CORE_H__

#include <ExynosEnv.h>

//
// Physical blocks (the caller maps them; identity in UEFI).
//
#define FIMC_IS_PHYS          0x13000000u   // MCUCTL at +0x180000
#define FIMC_IS_SIZE          0x001A0000u
#define FIMC_IS_PMU_PHYS      0x10040000u   // PMU, 0x5000 bytes used
#define FIMC_IS_CMU_PHYS      0x10010000u   // CMU (ISP block at +0xC000, TOP at +0x10000)
#define FIMC_IS_SYSREG_PHYS   0x10050000u

#define FIMC_IS_A5_MEM_SIZE   0x01400000u   // 20 MiB: firmware, heap, shared region
#define FIMC_IS_A5_MEM_ALIGN  0x04000000u   // FIMC_IS_FW_BASE_MASK = (1 << 26) - 1
#define FIMC_IS_SHARED_OFFSET 0x013C0000u   // FIMC_IS_SHARED_REGION_ADDR
#define FIMC_IS_CAL_OFFSET    0x013D0000u   // FIMC_IS_CAL_START_ADDR (module calibration)

//
// Mailbox commands from the A5 (fimc-is-cmd.h).
//
#define IHC_GET_SENSOR_NUMBER 0x1000

//
// Secure SFR access through the secure monitor (exynos_smc SMC_CMD_REG).
// Returns 0 on success.  NULL = skip the secure configuration.
//
typedef INTN (*FIMC_IS_SECURE_WRITE)(UINT32 PhysAddr, UINT32 Value);
typedef INTN (*FIMC_IS_SECURE_READ)(UINT32 PhysAddr, UINT32 *Value);

typedef struct {
  UINT32  Command;
  UINT32  SensorId;
  UINT32  Param[4];
} FIMC_IS_MSG;

typedef struct {
  // filled by the caller
  UINTN                   Is;          // FIMC_IS_PHYS
  UINTN                   Pmu;         // FIMC_IS_PMU_PHYS
  UINTN                   Cmu;         // FIMC_IS_CMU_PHYS
  UINTN                   Sysreg;      // FIMC_IS_SYSREG_PHYS
  VOID                    *MemVa;      // FIMC_IS_A5_MEM_SIZE, FIMC_IS_A5_MEM_ALIGN aligned
  UINT32                  MemPa;       // what the A5 sees (BBOAR); SysMMU off -> physical
  FIMC_IS_SECURE_WRITE    SecureWrite;
  FIMC_IS_SECURE_READ     SecureRead;
  // state
  UINT32                  Step;        // last step reached (for error reports)
  UINT32                  SecureCheck; // 0x131E0004 read back (0xFC expected), or ~0
  BOOLEAN                 DomainOn;
  BOOLEAN                 A5On;
} FIMC_IS;

//
// Register snapshot for logs: PMU ISP domain / A5, CMU TOP / ISP, MCUCTL.
//
typedef struct {
  UINT32  PdConfig, PdStatus, LowPowerOff;
  UINT32  ArmConfig, ArmStatus, ArmOption;
  UINT32  SrcTop1, SrcTop3, SrcTop4, SrcTop10, SrcTop11, DivTop0, DivTop1;
  UINT32  DivIsp0, DivIsp1, DivIsp2, GateIsp0, GateIsp1, GateGscl0;
  UINT32  Mcuctlr, Bboar, Intmsr1, Issr6;
} FIMC_IS_REGS;

VOID
FimcIsSnapshot (
  IN  FIMC_IS       *Is,
  OUT FIMC_IS_REGS  *Regs
  );

//
// ISP power domain on, clock gates on (isp0_*, isp1_*, gscl_3aa), the ISP
// block dividers to the stock ratios (MCUISP 1/2 and 1/4, aclk_div0/1/2
// 1/2, 1/4, 1/2).  TOP muxes are left as the bootloader set them unless
// SetTopMux (user muxes of aclk_400_isp / aclk_266_isp / aclk_333_432_isp
// and _isp0 to their switched source).  0 or < 0 (Is->Step says where).
//
INTN
FimcIsPowerUp (
  IN OUT FIMC_IS  *Is,
  IN     BOOLEAN  SetTopMux
  );

//
// Copies the firmware to the start of the A5 memory and cleans the cache.
// Returns 0, or < 0 when it does not fit below the shared region.
//
INTN
FimcIsLoadFirmware (
  IN OUT FIMC_IS     *Is,
  IN     CONST VOID  *Fw,
  IN     UINTN       Size
  );

//
// Pixel async bridge reset, BBOAR, secure configuration (if SecureWrite),
// A5 power-on and enable.  0 or < 0.
//
INTN
FimcIsStartA5 (
  IN OUT FIMC_IS  *Is
  );

//
// Polls the general mailbox (INTMSR1 bit 0 / ihcmd_iflag) for up to
// TimeoutMs; on a message fills Msg, acknowledges it and returns 0.
// Readers that have the interrupt (Windows) call FimcIsTakeMessage from
// the ISR instead.
//
INTN
FimcIsWaitMessage (
  IN  FIMC_IS      *Is,
  IN  UINTN        TimeoutMs,
  OUT FIMC_IS_MSG  *Msg
  );

BOOLEAN
FimcIsTakeMessage (
  IN  FIMC_IS      *Is,
  OUT FIMC_IS_MSG  *Msg
  );

//
// A5 off, the ISP domain's sys-power registers cleared, domain off.
//
VOID
FimcIsPowerDown (
  IN OUT FIMC_IS  *Is
  );

//
// Cache maintenance, supplied by the environment (as for MaliCore).
//
VOID
EnvCacheClean (
  IN VOID   *Va,
  IN UINTN  Size
  );

#endif // __FIMC_IS_CORE_H__
