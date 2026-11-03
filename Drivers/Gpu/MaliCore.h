/** @file
  Exynos5420 ARM Mali-T628 MP6 (Midgard, T62x): power, clocks, reset,
  MMU (Mali LPAE page tables) and job submission.  Shared by UEFI
  (Application/GpuTestApp) and Windows (ExynosWinDrivers), built
  unchanged in both through <ExynosEnv.h>; the caller maps the register
  blocks and provides the GPU-visible memory.

  Sources:
    - mainline Panfrost (drivers/gpu/drm/panfrost: panfrost_regs.h,
      panfrost_gpu.c soft reset / quirks / power-on, panfrost_mmu.c AS
      setup, panfrost_job.c job slot submission, panfrost_issues.h T620),
      io-pgtable-arm.c (ARM_MALI_LPAE: 4 levels, 4 KiB granule, leaf
      entries of type "block" (1) at every level, HAP permissions, no
      access flag, MEMATTR 0x88 / 0x8D / 0x88);
    - Samsung's r15p0 mali_kbase for the SoC side (platform/exynos/
      gpu_exynos5420.c, mach-exynos/clock-exynos5420.c): G3D power domain
      (status & 7 == 7), clock path VPLL -> mout_aclk_g3d -> dout_aclk_g3d
      -> mout_sw_aclk_g3d -> mout_user_aclk_g3d -> gate "g3d" (GATE_IP_G3D
      bits 9, 8, 6, 1), OPP 177..533 MHz at 812.5..1037.5 mV (BUCK4).
  The job descriptor layout (Midgard 64-bit job header, WRITE_VALUE
  payload) follows Mesa's Panfrost genxml v4.xml (arch 4 = T62x).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __MALI_CORE_H__
#define __MALI_CORE_H__

#include <ExynosEnv.h>

//
// Physical addresses of the blocks the caller maps.
//
#define MALI_GPU_PHYS        0x11800000      // 0x5000
#define MALI_CMU_TOP_PHYS    0x10020000      // 0x1000
#define MALI_PMU_G3D_PHYS    0x10044000      // 0x1000 (G3D domain at +0x80)

#define MALI_GSI_JOB         (219 + 32)
#define MALI_GSI_MMU         (74 + 32)
#define MALI_GSI_GPU         (117 + 32)

//
// MAX77802 BUCK4 (vdd_g3d): the host programs it (I2C) before power-up.
// DVS1..8 at 0x38..0x3F, 600 mV + 6.25 mV steps; CTRL1 0x37 bits 5:4 mode.
//
#define MALI_PMIC_BUCK4_CTRL1  0x37
#define MALI_PMIC_BUCK4_DVS1   0x38
#define MALI_BUCK4_CODE(uV)    ((UINT8)(((uV) - 600000u) / 6250u))

//
// Host-provided cache maintenance for memory the GPU reads / writes (no
// I/O coherency on the 5420).
//
VOID
EnvCacheClean (
  IN VOID   *Va,
  IN UINTN  Size
  );

VOID
EnvCacheInvalidate (
  IN VOID   *Va,
  IN UINTN  Size
  );

typedef struct {
  //
  // Register blocks (virtual addresses), filled by the caller.
  //
  UINTN    Gpu;
  UINTN    Cmu;
  UINTN    Pmu;
  //
  // Discovered at power-up.
  //
  UINT32   GpuId;          // [31:16] product (0x0620 = T62x), [15:12] major, [11:4] minor, [3:0] status
  UINT32   L2Features;
  UINT32   CoreFeatures;
  UINT32   TilerFeatures;
  UINT32   MemFeatures;
  UINT32   MmuFeatures;    // [7:0] VA bits, [15:8] PA bits
  UINT32   AsPresent;
  UINT32   JsPresent;
  UINT32   ShaderPresent;
  UINT32   TilerPresent;
  UINT32   L2Present;
  UINT32   JsFeatures[3];  // job types per slot (bit n = type n): slot 0 fragment, slot 1 the rest
  UINT32   ClockHz;
  UINT32   Step;           // MALI_STEP_* reached / failed
} EXYNOS_MALI;

enum {
  MALI_STEP_DOMAIN = 1,
  MALI_STEP_CLOCK,
  MALI_STEP_RESET,
  MALI_STEP_L2,
  MALI_STEP_CORES,
  MALI_STEP_READY
};

//
// Page-table pool: physically contiguous, 4 KiB aligned, a constant
// virtual / physical offset (UEFI: identity; Windows: contiguous memory).
//
typedef struct {
  UINT8    *Va;
  UINT64   Pa;
  UINTN    Size;
  UINTN    Used;
  UINT64   *Root;          // level 0 table
} MALI_PT_POOL;

//
// Job header (Midgard, 64-bit descriptors), 32 bytes, 64-byte aligned.
//
#define MALI_JOB_TYPE_NULL         1
#define MALI_JOB_TYPE_WRITE_VALUE  2
#define MALI_JOB_TYPE_CACHE_FLUSH  3
#define MALI_JOB_TYPE_FRAGMENT     9

#define MALI_SLOT_FRAGMENT         0       // panfrost: fragment jobs on JS0
#define MALI_SLOT_OTHER            1       // vertex / tiler / compute / write value / null

//
// Midgard v4 (T62x) WRITE_VALUE types (Mesa genxml v4.xml): no immediate
// values on this architecture; Zero writes 64 bits of 0.
//
#define MALI_WRITE_VALUE_CYCLE_COUNTER  1
#define MALI_WRITE_VALUE_TIMESTAMP      2
#define MALI_WRITE_VALUE_ZERO           3

#pragma pack (1)
typedef struct {
  UINT32  ExceptionStatus;
  UINT32  FirstIncompleteTask;
  UINT64  FaultPointer;
  UINT32  Control;         // [0] 64-bit descriptor, [7:1] type, [8] barrier, [31:16] index
  UINT16  Dependency1;
  UINT16  Dependency2;
  UINT64  Next;
} MALI_JOB_HEADER;

typedef struct {
  MALI_JOB_HEADER  Header;
  UINT64           Address;
  UINT32           Type;
  UINT32           Reserved;
} MALI_WRITE_VALUE_JOB;
//
// Clear-only render pass (Midgard v4 single-target framebuffer, Mesa
// pan_emit_fbd for PAN_ARCH 4): a fragment job over the buffer's 16x16
// tiles with the tiler disabled; every pixel gets the raw 32-bit Color.
//
typedef struct {
  MALI_JOB_HEADER  Header;
  UINT32           BoundMin;       // [11:0] X tile, [27:16] Y tile
  UINT32           BoundMax;
  UINT64           Framebuffer;
} MALI_FRAGMENT_JOB;

typedef struct {
  UINT32  LocalStorage[8];         // 0
  UINT32  Parameters[40];          // 32
  UINT32  Tiler[10];               // 192
  UINT32  Padding1[6];             // 232
  UINT32  TilerWeights[8];         // 256
  UINT32  Padding2[8];             // 288
} MALI_FRAMEBUFFER;                // 320 bytes, 64-byte aligned
#pragma pack ()

#define MALI_POLYGON_LIST_BYTES    4096    // tiler header for a disabled tiler (512 used)

//
// Power and setup.
//
INTN
MaliPowerUp (
  IN OUT EXYNOS_MALI  *Mali,
  IN     UINT32       ClockMhz           // 177, 266, 350, 420, 480, 533
  );

VOID
MaliPowerDown (
  IN OUT EXYNOS_MALI  *Mali
  );

VOID
MaliLogState (
  IN EXYNOS_MALI  *Mali
  );

//
// MMU.
//
INTN
MaliPtInit (
  IN OUT MALI_PT_POOL  *Pool
  );

INTN
MaliPtMap (
  IN OUT MALI_PT_POOL  *Pool,
  IN     UINT64        GpuVa,
  IN     UINT64        Pa,
  IN     UINTN         Size              // multiple of 4 KiB
  );

//
// Pages by frame number (4 KiB each) at GpuVa, read / write; tables from
// the pool.  0 or -1 (pool exhausted).
//
INTN
MaliPtMapPages (
  IN OUT MALI_PT_POOL  *Pool,
  IN     UINT64        GpuVa,
  IN     CONST UINTN   *Pfn,
  IN     UINTN         Pages
  );

VOID
MaliPtUnmap (
  IN OUT MALI_PT_POOL  *Pool,
  IN     UINT64        GpuVa,
  IN     UINTN         Pages
  );

//
// TLB / cache maintenance after page-table changes in a range.
//
INTN
MaliAsFlush (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       As,
  IN UINT64       GpuVa,
  IN UINT64       Size
  );

INTN
MaliAsEnable (
  IN EXYNOS_MALI   *Mali,
  IN UINT32        As,
  IN MALI_PT_POOL  *Pool
  );

VOID
MaliAsDisable (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       As
  );

//
// Jobs.
//
VOID
MaliBuildNullJob (
  OUT MALI_JOB_HEADER  *Job,
  IN  UINT16           Index,
  IN  UINT64           NextGpuVa
  );

VOID
MaliBuildWriteValueJob (
  OUT MALI_WRITE_VALUE_JOB  *Job,
  IN  UINT16                Index,
  IN  UINT64                NextGpuVa,
  IN  UINT64                TargetGpuVa,
  IN  UINT32                Type             // MALI_WRITE_VALUE_*
  );

VOID
MaliBuildClearJob (
  OUT MALI_FRAGMENT_JOB  *Job,
  OUT MALI_FRAMEBUFFER   *Fbd,
  IN  UINT64             FbdGpuVa,
  IN  UINT64             PolygonListGpuVa,     // MALI_POLYGON_LIST_BYTES, zeroed
  IN  UINT64             TargetGpuVa,
  IN  UINT32             Width,
  IN  UINT32             Height,
  IN  UINT32             RowStride,            // bytes
  IN  UINT32             Color                 // raw 32-bit pixel
  );

INTN
MaliJobStart (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       Slot,
  IN UINT32       As,
  IN UINT64       ChainGpuVa
  );

//
// Polled completion (tests; the Windows driver takes the job interrupt):
// 0 done, > 0 fault (JS_STATUS), < 0 timeout.
//
INTN
MaliJobWait (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       Slot,
  IN UINT32       TimeoutMs
  );

VOID
MaliLogFault (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       Slot,
  IN UINT32       As
  );

#endif // __MALI_CORE_H__
