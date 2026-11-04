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
// Mailbox commands (fimc-is-cmd.h, IS_COMMAND_VER 122).  Host -> A5:
//
#define HIC_PROCESS_START     5
#define HIC_PROCESS_STOP      6
#define HIC_STREAM_ON         7
#define HIC_STREAM_OFF        8
#define HIC_SET_PARAMETER     13
#define HIC_OPEN_SENSOR       18
#define HIC_POWER_DOWN        23
#define HIC_GET_SET_FILE_ADDR 24
#define HIC_LOAD_SET_FILE     25
// A5 -> host:
#define IHC_GET_SENSOR_NUMBER 0x1000
#define IHC_NOT_READY         0x1005
// replies (A5 -> host, param1 = the command):
#define ISR_DONE              0x2000
#define ISR_NDONE             0x2001

#define FIMC_IS_GROUP_SHIFT   16          // hic_sensorid = group << 16 | instance
#define FIMC_IS_GROUP_3A0     0
#define FIMC_IS_GROUP_3A1     1
#define FIMC_IS_GROUP_ISP     2

//
// Per-instance parameter region at the end of the A5 memory
// (offset_region = A5_MEM_SIZE - (instance + 1) * REGION_SIZE), struct
// is_region (fimc-is-param.h; offsets from compiling the kernel header
// for ARM): parameter 0x1D80 bytes, tune, frame headers, face markers,
// then u32 shared[500] at +0x2D34.  HIC_OPEN_SENSOR takes the A5 address
// of shared[] with struct sensor_open_extended in it; the A5 writes
// FIMC_IS_MAGIC to shared[499] when it accepted the open.
//
#define FIMC_IS_REGION_SIZE   0x00005000u
#define FIMC_IS_REGION_OFFSET(i) (FIMC_IS_A5_MEM_SIZE - ((i) + 1) * FIMC_IS_REGION_SIZE)
#define FIMC_IS_REGION_SHARED 0x2D34u
#define FIMC_IS_SHARED_COUNT  500
#define FIMC_IS_MAGIC         0x01020304u
#define FIMC_IS_SETFILE_MAX   0x00140000u // FIMC_IS_SETFILE_SIZE

//
// struct sensor_open_extended (include/media/exynos_fimc_is.h), all u32.
//
typedef struct {
  UINT32  ProductName;
  UINT32  PeriType;                 // SE_I2C 0, SE_SPI 1, SE_GPIO 2
  UINT32  Setting[3];               // i2c {channel, slave, speed} / gpio {first, second}
} FIMC_IS_SENSOR_PROTOCOL;

typedef struct {
  UINT32                   I2cChannel;
  FIMC_IS_SENSOR_PROTOCOL  Actuator;
  FIMC_IS_SENSOR_PROTOCOL  Flash;
  FIMC_IS_SENSOR_PROTOCOL  From;
  UINT32                   Mclk;
  UINT32                   MipiLanes;
  UINT32                   MipiSpeed;
  UINT32                   FastOpenSensor;
  UINT32                   SelfCalibrationMode;
  UINT32                   I2cSclk;
} FIMC_IS_OPEN_EXT;

//
// Rear camera module (fimc-is-device-sensor.c, CONFIG_MACH_UNIVERSAL5420):
// IMX134 = 36, actuator WV560 (6) on ISP I2C1, flash MAX77693 (3) GPIO 0/1,
// no F-ROM driver (100), ISP I2C at 108 MHz (I2C_L0).
//
#define FIMC_IS_SENSOR_IMX134 36
#define FIMC_IS_SENSOR_S5K6B2 9

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
  UINT32                  LogPos;      // FimcIsLogDump: firmware log read position
  BOOLEAN                 SensorI2cFail; // the firmware logged "Sensor I2C doesn't work"
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
// Host -> A5 message (fimc_is_set_cmd without the wait): waits for the A5
// to have taken the previous one (INTMSR0 bit 0 clear), fills hicmd /
// hic_sensorid / hic_param1..4 and raises INTGR0 bit 0.  0 or -1 (A5 busy).
// The reply (ISR_DONE / ISR_NDONE, param1 = Command) comes as a general
// message: FimcIsWaitMessage / FimcIsTakeMessage.
//
INTN
FimcIsSend (
  IN FIMC_IS  *Is,
  IN UINT32   Command,
  IN UINT32   SensorId,
  IN UINT32   Param1,
  IN UINT32   Param2,
  IN UINT32   Param3,
  IN UINT32   Param4
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

// used by FimcIsHost.c (A5 -> host data: region, firmware log)
VOID
EnvCacheInvalidate (
  IN VOID   *Va,
  IN UINTN  Size
  );

#endif // __FIMC_IS_CORE_H__
