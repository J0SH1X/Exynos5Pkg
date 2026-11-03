/** @file
  Mali-T628 core: see MaliCore.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MaliCore.h"

//
// GPU registers (panfrost_regs.h).
//
#define GPU_ID                 0x000
#define GPU_L2_FEATURES        0x004
#define GPU_CORE_FEATURES      0x008
#define GPU_TILER_FEATURES     0x00C
#define GPU_MEM_FEATURES       0x010
#define GPU_MMU_FEATURES       0x014
#define GPU_AS_PRESENT         0x018
#define GPU_JS_PRESENT         0x01C
#define GPU_INT_RAWSTAT        0x020
#define GPU_INT_CLEAR          0x024
#define GPU_INT_MASK           0x028
#define   GPU_IRQ_FAULT              (1u << 0)
#define   GPU_IRQ_RESET_COMPLETED    (1u << 8)
#define   GPU_IRQ_POWER_CHANGED_ALL  (1u << 10)
#define GPU_CMD                0x030
#define   GPU_CMD_SOFT_RESET         0x01
#define   GPU_CMD_HARD_RESET         0x02
#define GPU_STATUS             0x034
#define GPU_FAULT_STATUS       0x03C
#define GPU_FAULT_ADDRESS_LO   0x040
#define GPU_JS_FEATURES(n)     (0x0C0 + (n) * 4)
#define GPU_SHADER_PRESENT_LO  0x100
#define GPU_TILER_PRESENT_LO   0x110
#define GPU_L2_PRESENT_LO      0x120
#define SHADER_READY_LO        0x140
#define TILER_READY_LO         0x150
#define L2_READY_LO            0x160
#define SHADER_PWRON_LO        0x180
#define TILER_PWRON_LO         0x190
#define L2_PWRON_LO            0x1A0
#define SHADER_PWROFF_LO       0x1C0
#define TILER_PWROFF_LO        0x1D0
#define L2_PWROFF_LO           0x1E0
#define GPU_SHADER_CONFIG      0xF04
#define   SC_LS_PAUSEBUFFER_DISABLE  (1u << 16)
#define   SC_LS_ATTR_CHECK_DISABLE   (1u << 18)

#define JOB_INT_RAWSTAT        0x1000
#define JOB_INT_CLEAR          0x1004
#define JOB_INT_MASK           0x1008
#define JS_BASE                0x1800
#define JS_STRIDE              0x80
#define JS_HEAD_LO(n)          (JS_BASE + (n) * JS_STRIDE + 0x00)
#define JS_HEAD_HI(n)          (JS_BASE + (n) * JS_STRIDE + 0x04)
#define JS_STATUS(n)           (JS_BASE + (n) * JS_STRIDE + 0x24)
#define JS_HEAD_NEXT_LO(n)     (JS_BASE + (n) * JS_STRIDE + 0x40)
#define JS_HEAD_NEXT_HI(n)     (JS_BASE + (n) * JS_STRIDE + 0x44)
#define JS_AFFINITY_NEXT_LO(n) (JS_BASE + (n) * JS_STRIDE + 0x50)
#define JS_AFFINITY_NEXT_HI(n) (JS_BASE + (n) * JS_STRIDE + 0x54)
#define JS_CONFIG_NEXT(n)      (JS_BASE + (n) * JS_STRIDE + 0x58)
#define JS_COMMAND_NEXT(n)     (JS_BASE + (n) * JS_STRIDE + 0x60)
#define   JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE  (3u << 8)
#define   JS_CONFIG_START_MMU                     (1u << 10)
#define   JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE    (3u << 12)
#define   JS_CONFIG_THREAD_PRI(n)                 ((n) << 16)
#define   JS_COMMAND_START                        0x01

#define MMU_INT_RAWSTAT        0x2000
#define MMU_INT_CLEAR          0x2004
#define MMU_INT_MASK           0x2008
#define MMU_AS(as)             (0x2400 + ((as) << 6))
#define AS_TRANSTAB_LO(as)     (MMU_AS (as) + 0x00)
#define AS_TRANSTAB_HI(as)     (MMU_AS (as) + 0x04)
#define AS_MEMATTR_LO(as)      (MMU_AS (as) + 0x08)
#define AS_MEMATTR_HI(as)      (MMU_AS (as) + 0x0C)
#define AS_LOCKADDR_LO(as)     (MMU_AS (as) + 0x10)
#define AS_LOCKADDR_HI(as)     (MMU_AS (as) + 0x14)
#define AS_COMMAND(as)         (MMU_AS (as) + 0x18)
#define AS_FAULTSTATUS(as)     (MMU_AS (as) + 0x1C)
#define AS_FAULTADDRESS_LO(as) (MMU_AS (as) + 0x20)
#define AS_FAULTADDRESS_HI(as) (MMU_AS (as) + 0x24)
#define AS_STATUS(as)          (MMU_AS (as) + 0x28)
#define   AS_COMMAND_UPDATE      0x01
#define   AS_COMMAND_LOCK        0x02
#define   AS_COMMAND_FLUSH_MEM   0x05
#define   AS_STATUS_ACTIVE       0x01

//
// Mali LPAE (io-pgtable-arm.c ARM_MALI_LPAE).
//
#define PTE_TABLE              0x3ull
#define PTE_LEAF               0x1ull          // "block" type, also at level 3
#define PTE_HAP_READ           (1ull << 6)
#define PTE_HAP_WRITE          (2ull << 6)
#define PTE_SH_IS              (3ull << 8)
#define PTE_ADDR_MASK          0x0000FFFFFFFFF000ull
#define TRANSTAB_ADRMODE_TABLE 0x3ull
#define TRANSTAB_READ_INNER    (1ull << 2)
#define MEMATTR_MALI           (0x88ull | (0x8Dull << 8) | (0x88ull << 16))

//
// SoC: PMU G3D domain, CMU_TOP (offsets from 0x10020000).
//
#define PMU_G3D_CONFIG         0x080
#define PMU_G3D_STATUS         0x084
#define VPLL_LOCK              0x080
#define VPLL_CON0              0x170
#define   PLL_ENABLE             (1u << 31)
#define   PLL_LOCKED             (1u << 29)
#define SRC_TOP2               0x208           // [16] mout_aclk_g3d: 0 VPLL, 1 DPLL
#define SRC_TOP5               0x214           // [16] mout_user_aclk_g3d: 0 XXTI, 1 mout_sw_aclk_g3d
#define SRC_TOP6               0x218           // [4] mout_sclk_vpll: 1 FOUT_VPLL
#define SRC_TOP12              0x288           // [16] mout_sw_aclk_g3d: 0 dout_aclk_g3d, 1 SPLL
#define DIV_TOP2               0x508           // [18:16] dout_aclk_g3d
#define DIV_STAT_TOP2          0x608
#define GATE_IP_G3D            0x930           // bits 9, 8, 6, 1: "g3d" (Samsung), 9 (mainline CLK_G3D)
#define G3D_GATES              ((1u << 9) | (1u << 8) | (1u << 6) | (1u << 1))

#define RD(o)        MmioRead32 (Mali->Gpu + (o))
#define WR(o, v)     MmioWrite32 (Mali->Gpu + (o), (v))

STATIC CONST struct {
  UINT32  Mhz;
  UINT16  M;
  UINT8   P;
  UINT8   S;
} mVpll[] = {
  // clk-exynos5420.c exynos5420_vpll_24mhz_tbl
  { 177, 118, 2, 3 }, { 266, 266, 3, 3 }, { 350, 175, 3, 2 }, { 420, 140, 2, 2 },
  { 480, 160, 2, 2 }, { 533, 181, 2, 2 },      // 543 MHz entry for the 533 OPP
};

STATIC
BOOLEAN
WaitBits (
  IN UINTN   Address,
  IN UINT32  Mask,
  IN UINT32  Want,
  IN UINT32  TimeoutUs
  )
{
  UINT32  Waited;

  for (Waited = 0; Waited < TimeoutUs; Waited += 10) {
    if ((MmioRead32 (Address) & Mask) == Want) {
      return TRUE;
    }
    MicroSecondDelay (10);
  }
  return (MmioRead32 (Address) & Mask) == Want;
}

/**
  VPLL to the rate, aclk_g3d path onto it, gates on.
**/
STATIC
INTN
SetClock (
  IN OUT EXYNOS_MALI  *Mali,
  IN     UINT32       Mhz
  )
{
  UINTN   Index;
  UINT32  Con;

  for (Index = 0; (Index < ARRAY_SIZE (mVpll)) && (mVpll[Index].Mhz < Mhz); Index++) {
  }
  if (Index == ARRAY_SIZE (mVpll)) {
    Index--;
  }

  //
  // Run aclk_g3d from XXTI while the VPLL relocks (Samsung's
  // gpu_set_clock: clk_set_parent (aclk_g3d, ext_xtal) around the change).
  //
  MmioAnd32 (Mali->Cmu + SRC_TOP5, ~(1u << 16));
  MmioWrite32 (Mali->Cmu + VPLL_LOCK, (UINT32)mVpll[Index].P * 270);
  Con = PLL_ENABLE | ((UINT32)mVpll[Index].M << 16) | ((UINT32)mVpll[Index].P << 8) | mVpll[Index].S;
  MmioWrite32 (Mali->Cmu + VPLL_CON0, Con);
  if (!WaitBits (Mali->Cmu + VPLL_CON0, PLL_LOCKED, PLL_LOCKED, 10000)) {
    DEBUG ((DEBUG_ERROR, "Mali: VPLL does not lock (CON0 0x%08x)\n", MmioRead32 (Mali->Cmu + VPLL_CON0)));
    return -1;
  }
  MmioOr32 (Mali->Cmu + SRC_TOP6, 1u << 4);                  // mout_sclk_vpll = FOUT_VPLL
  MmioAnd32 (Mali->Cmu + SRC_TOP2, ~(1u << 16));             // mout_aclk_g3d = VPLL
  MmioAnd32 (Mali->Cmu + DIV_TOP2, ~(7u << 16));             // dout_aclk_g3d = / 1
  WaitBits (Mali->Cmu + DIV_STAT_TOP2, 1u << 16, 0, 1000);
  MmioAnd32 (Mali->Cmu + SRC_TOP12, ~(1u << 16));            // mout_sw_aclk_g3d = dout_aclk_g3d
  MmioOr32 (Mali->Cmu + SRC_TOP5, 1u << 16);                 // mout_user_aclk_g3d = mout_sw_aclk_g3d
  MmioOr32 (Mali->Cmu + GATE_IP_G3D, G3D_GATES);
  Mali->ClockHz = (UINT32)(24000000ull * mVpll[Index].M / ((UINT64)mVpll[Index].P << mVpll[Index].S));
  return 0;
}

STATIC
INTN
PowerOnCores (
  IN OUT EXYNOS_MALI  *Mali,
  IN     UINT32       PwrOn,
  IN     UINT32       Ready,
  IN     UINT32       Present
  )
{
  if (Present == 0) {
    return 0;
  }
  WR (PwrOn, Present);
  return WaitBits (Mali->Gpu + Ready, Present, Present, 20000) ? 0 : -1;
}

INTN
MaliPowerUp (
  IN OUT EXYNOS_MALI  *Mali,
  IN     UINT32       ClockMhz
  )
{
  UINT32  Quirks;

  //
  // Clock first (gates on, so the domain's transition sees a clock), then
  // the G3D power domain.
  //
  Mali->Step = MALI_STEP_CLOCK;
  if (SetClock (Mali, ClockMhz) != 0) {
    return -1;
  }
  Mali->Step = MALI_STEP_DOMAIN;
  if ((MmioRead32 (Mali->Pmu + PMU_G3D_STATUS) & 7) != 7) {
    MmioWrite32 (Mali->Pmu + PMU_G3D_CONFIG, 7);
    if (!WaitBits (Mali->Pmu + PMU_G3D_STATUS, 7, 7, 10000)) {
      DEBUG ((DEBUG_ERROR, "Mali: G3D domain does not come up (status 0x%x)\n", MmioRead32 (Mali->Pmu + PMU_G3D_STATUS)));
      return -1;
    }
  }

  //
  // Soft reset (panfrost_gpu_soft_reset), hard reset if that times out.
  //
  Mali->Step = MALI_STEP_RESET;
  WR (GPU_INT_MASK, 0);
  WR (GPU_INT_CLEAR, 0xFFFFFFFF);
  WR (GPU_CMD, GPU_CMD_SOFT_RESET);
  if (!WaitBits (Mali->Gpu + GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 10000)) {
    DEBUG ((DEBUG_ERROR, "Mali: soft reset timed out, hard reset\n"));
    WR (GPU_CMD, GPU_CMD_HARD_RESET);
    if (!WaitBits (Mali->Gpu + GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 100000)) {
      return -1;
    }
  }
  WR (GPU_INT_CLEAR, 0xFFFFFFFF);

  Mali->GpuId         = RD (GPU_ID);
  Mali->L2Features    = RD (GPU_L2_FEATURES);
  Mali->CoreFeatures  = RD (GPU_CORE_FEATURES);
  Mali->TilerFeatures = RD (GPU_TILER_FEATURES);
  Mali->MemFeatures   = RD (GPU_MEM_FEATURES);
  Mali->MmuFeatures   = RD (GPU_MMU_FEATURES);
  Mali->AsPresent     = RD (GPU_AS_PRESENT);
  Mali->JsPresent     = RD (GPU_JS_PRESENT);
  Mali->ShaderPresent = RD (GPU_SHADER_PRESENT_LO);
  Mali->TilerPresent  = RD (GPU_TILER_PRESENT_LO);
  Mali->L2Present     = RD (GPU_L2_PRESENT_LO);
  Mali->JsFeatures[0] = RD (GPU_JS_FEATURES (0));
  Mali->JsFeatures[1] = RD (GPU_JS_FEATURES (1));
  Mali->JsFeatures[2] = RD (GPU_JS_FEATURES (2));

  //
  // T62x quirks (panfrost_gpu_init_quirks: issue 11035 on r0p1, the
  // load/store attribute check before T76x).
  //
  Quirks = SC_LS_ATTR_CHECK_DISABLE;
  if ((Mali->GpuId & 0xFFFF0000u) == 0x06200000u && (Mali->GpuId & 0xFFF0u) == 0x0010u) {
    Quirks |= SC_LS_PAUSEBUFFER_DISABLE;
  }
  WR (GPU_SHADER_CONFIG, Quirks);

  //
  // L2, then shader cores and tiler (panfrost_gpu_power_on).
  //
  Mali->Step = MALI_STEP_L2;
  if (PowerOnCores (Mali, L2_PWRON_LO, L2_READY_LO, Mali->L2Present) != 0) {
    return -1;
  }
  Mali->Step = MALI_STEP_CORES;
  if ((PowerOnCores (Mali, SHADER_PWRON_LO, SHADER_READY_LO, Mali->ShaderPresent) != 0) ||
      (PowerOnCores (Mali, TILER_PWRON_LO, TILER_READY_LO, Mali->TilerPresent) != 0)) {
    return -1;
  }

  WR (JOB_INT_CLEAR, 0xFFFFFFFF);
  WR (MMU_INT_CLEAR, 0xFFFFFFFF);
  Mali->Step = MALI_STEP_READY;
  return 0;
}

VOID
MaliPowerDown (
  IN OUT EXYNOS_MALI  *Mali
  )
{
  if ((MmioRead32 (Mali->Pmu + PMU_G3D_STATUS) & 7) == 7) {
    WR (GPU_INT_MASK, 0);
    WR (JOB_INT_MASK, 0);
    WR (MMU_INT_MASK, 0);
    WR (SHADER_PWROFF_LO, Mali->ShaderPresent);
    WR (TILER_PWROFF_LO, Mali->TilerPresent);
    WaitBits (Mali->Gpu + SHADER_READY_LO, 0xFFFFFFFF, 0, 20000);
    WR (L2_PWROFF_LO, Mali->L2Present);
    WaitBits (Mali->Gpu + L2_READY_LO, 0xFFFFFFFF, 0, 20000);
    MmioWrite32 (Mali->Pmu + PMU_G3D_CONFIG, 0);
    WaitBits (Mali->Pmu + PMU_G3D_STATUS, 7, 0, 10000);
  }
  MmioAnd32 (Mali->Cmu + GATE_IP_G3D, ~G3D_GATES);
  Mali->Step = 0;
}

VOID
MaliLogState (
  IN EXYNOS_MALI  *Mali
  )
{
  DEBUG ((DEBUG_INFO, "Mali: GPU_ID 0x%08x (product 0x%04x r%up%u), %u MHz, step %u\n", Mali->GpuId,
          Mali->GpuId >> 16, (Mali->GpuId >> 12) & 0xF, (Mali->GpuId >> 4) & 0xFF, Mali->ClockHz / 1000000, Mali->Step));
  DEBUG ((DEBUG_INFO, "Mali: shaders 0x%x tiler 0x%x L2 0x%x, AS 0x%x JS 0x%x, MMU VA %u PA %u bits\n",
          Mali->ShaderPresent, Mali->TilerPresent, Mali->L2Present, Mali->AsPresent, Mali->JsPresent,
          Mali->MmuFeatures & 0xFF, (Mali->MmuFeatures >> 8) & 0xFF));
  DEBUG ((DEBUG_INFO, "Mali: job slot features 0x%x 0x%x 0x%x\n", Mali->JsFeatures[0], Mali->JsFeatures[1],
          Mali->JsFeatures[2]));
  DEBUG ((DEBUG_INFO, "Mali: features L2 0x%08x core 0x%08x tiler 0x%08x mem 0x%08x; ready shader 0x%x L2 0x%x\n",
          Mali->L2Features, Mali->CoreFeatures, Mali->TilerFeatures, Mali->MemFeatures,
          (Mali->Step >= MALI_STEP_RESET) ? RD (SHADER_READY_LO) : 0, (Mali->Step >= MALI_STEP_RESET) ? RD (L2_READY_LO) : 0));
}

//
// ---------------------------------------------------------------------------
// MMU
// ---------------------------------------------------------------------------
//

STATIC
UINT64 *
PtAlloc (
  IN OUT MALI_PT_POOL  *Pool,
  OUT    UINT64        *Pa
  )
{
  UINT64  *Table;

  if (Pool->Used + 4096 > Pool->Size) {
    return NULL;
  }
  Table = (UINT64 *)(Pool->Va + Pool->Used);
  *Pa   = Pool->Pa + Pool->Used;
  Pool->Used += 4096;
  ZeroMem (Table, 4096);
  return Table;
}

INTN
MaliPtInit (
  IN OUT MALI_PT_POOL  *Pool
  )
{
  UINT64  Pa;

  Pool->Used = 0;
  Pool->Root = PtAlloc (Pool, &Pa);
  return (Pool->Root != NULL) ? 0 : -1;
}

/**
  4 KiB leaf entries; intermediate tables from the pool.  The whole pool
  is cleaned to memory afterwards (the GPU walks it uncached).
**/
INTN
MaliPtMap (
  IN OUT MALI_PT_POOL  *Pool,
  IN     UINT64        GpuVa,
  IN     UINT64        Pa,
  IN     UINTN         Size
  )
{
  UINTN   Done;
  UINT64  *Table;
  UINT64  TablePa;
  UINTN   Level;
  UINTN   Index;

  for (Done = 0; Done < Size; Done += 4096) {
    UINT64  Va = GpuVa + Done;

    Table = Pool->Root;
    for (Level = 0; Level < 3; Level++) {
      Index = (UINTN)((Va >> (39 - 9 * Level)) & 0x1FF);
      if ((Table[Index] & 3) != PTE_TABLE) {
        UINT64  *Next = PtAlloc (Pool, &TablePa);

        if (Next == NULL) {
          return -1;
        }
        Table[Index] = (TablePa & PTE_ADDR_MASK) | PTE_TABLE;
      }
      Table = (UINT64 *)(Pool->Va + ((Table[Index] & PTE_ADDR_MASK) - Pool->Pa));
    }
    Index = (UINTN)((Va >> 12) & 0x1FF);
    Table[Index] = ((Pa + Done) & PTE_ADDR_MASK) | PTE_LEAF | PTE_HAP_READ | PTE_HAP_WRITE | PTE_SH_IS;
  }
  EnvCacheClean (Pool->Va, Pool->Used);
  return 0;
}

/**
  Leaf table for GpuVa (allocating the levels above from the pool), or
  NULL when the pool is exhausted.
**/
STATIC
UINT64 *
PtLeafTable (
  IN OUT MALI_PT_POOL  *Pool,
  IN     UINT64        GpuVa,
  IN     BOOLEAN       Create
  )
{
  UINT64  *Table = Pool->Root;
  UINT64  TablePa;
  UINTN   Level;
  UINTN   Index;

  for (Level = 0; Level < 3; Level++) {
    Index = (UINTN)((GpuVa >> (39 - 9 * Level)) & 0x1FF);
    if ((Table[Index] & 3) != PTE_TABLE) {
      UINT64  *Next;

      if (!Create) {
        return NULL;
      }
      Next = PtAlloc (Pool, &TablePa);
      if (Next == NULL) {
        return NULL;
      }
      Table[Index] = (TablePa & PTE_ADDR_MASK) | PTE_TABLE;
      EnvCacheClean (&Table[Index], sizeof (UINT64));
    }
    Table = (UINT64 *)(Pool->Va + ((Table[Index] & PTE_ADDR_MASK) - Pool->Pa));
  }
  return Table;
}

INTN
MaliPtMapPages (
  IN OUT MALI_PT_POOL  *Pool,
  IN     UINT64        GpuVa,
  IN     CONST UINTN   *Pfn,
  IN     UINTN         Pages
  )
{
  UINTN   Page;
  UINT64  *Table = NULL;
  UINTN   First = 0;

  for (Page = 0; Page < Pages; Page++) {
    UINT64  Va = GpuVa + (UINT64)Page * 4096;
    UINTN   Index = (UINTN)((Va >> 12) & 0x1FF);

    if ((Table == NULL) || (Index == 0)) {
      if (Table != NULL) {
        EnvCacheClean (&Table[First], (512 - First) * sizeof (UINT64));
      }
      Table = PtLeafTable (Pool, Va, TRUE);
      if (Table == NULL) {
        return -1;
      }
      First = Index;
    }
    Table[Index] = (((UINT64)Pfn[Page] << 12) & PTE_ADDR_MASK) | PTE_LEAF | PTE_HAP_READ | PTE_HAP_WRITE | PTE_SH_IS;
    if ((Page + 1 == Pages) || (Index == 511)) {
      EnvCacheClean (&Table[First], (Index + 1 - First) * sizeof (UINT64));
      Table = (Index == 511) ? NULL : Table;
    }
  }
  return 0;
}

VOID
MaliPtUnmap (
  IN OUT MALI_PT_POOL  *Pool,
  IN     UINT64        GpuVa,
  IN     UINTN         Pages
  )
{
  UINTN   Page;

  for (Page = 0; Page < Pages; Page++) {
    UINT64  Va = GpuVa + (UINT64)Page * 4096;
    UINT64  *Table = PtLeafTable (Pool, Va, FALSE);

    if (Table != NULL) {
      Table[(Va >> 12) & 0x1FF] = 0;
      EnvCacheClean (&Table[(Va >> 12) & 0x1FF], sizeof (UINT64));
    }
  }
}

STATIC
BOOLEAN
AsWait (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       As
  )
{
  return WaitBits (Mali->Gpu + AS_STATUS (As), AS_STATUS_ACTIVE, 0, 10000);
}

INTN
MaliAsEnable (
  IN EXYNOS_MALI   *Mali,
  IN UINT32        As,
  IN MALI_PT_POOL  *Pool
  )
{
  UINT64  Transtab = (Pool->Pa & PTE_ADDR_MASK) | TRANSTAB_READ_INNER | TRANSTAB_ADRMODE_TABLE;

  //
  // panfrost_mmu_enable: lock everything, flush, new tables, update.
  // Lock region: the whole space (width 63).
  //
  WR (MMU_INT_CLEAR, 0xFFFFFFFF);
  WR (AS_LOCKADDR_LO (As), 63);
  WR (AS_LOCKADDR_HI (As), 0);
  WR (AS_COMMAND (As), AS_COMMAND_LOCK);
  AsWait (Mali, As);
  WR (AS_COMMAND (As), AS_COMMAND_FLUSH_MEM);
  if (!AsWait (Mali, As)) {
    return -1;
  }
  WR (AS_TRANSTAB_LO (As), (UINT32)Transtab);
  WR (AS_TRANSTAB_HI (As), (UINT32)(Transtab >> 32));
  WR (AS_MEMATTR_LO (As), (UINT32)MEMATTR_MALI);
  WR (AS_MEMATTR_HI (As), (UINT32)(MEMATTR_MALI >> 32));
  WR (AS_COMMAND (As), AS_COMMAND_UPDATE);
  return AsWait (Mali, As) ? 0 : -1;
}

/**
  After page-table changes in [GpuVa, GpuVa + Size): lock the region (base
  | log2(size) - 1, at least 4 KiB... panfrost lock_region), flush (writes
  back the GPU caches and drops the TLB entries, then unlocks).
**/
INTN
MaliAsFlush (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       As,
  IN UINT64       GpuVa,
  IN UINT64       Size
  )
{
  UINT32  Width = 12;
  UINT64  Region;

  while ((Width < 63) && ((1ull << Width) < Size)) {
    Width++;
  }
  // the region must be aligned to its size
  while ((Width < 63) && ((GpuVa & ~((1ull << Width) - 1)) + (1ull << Width) < GpuVa + Size)) {
    Width++;
  }
  Region = (GpuVa & ~((1ull << Width) - 1)) | (Width - 1);
  WR (AS_LOCKADDR_LO (As), (UINT32)Region);
  WR (AS_LOCKADDR_HI (As), (UINT32)(Region >> 32));
  WR (AS_COMMAND (As), AS_COMMAND_LOCK);
  AsWait (Mali, As);
  WR (AS_COMMAND (As), AS_COMMAND_FLUSH_MEM);
  return AsWait (Mali, As) ? 0 : -1;
}

VOID
MaliAsDisable (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       As
  )
{
  WR (AS_COMMAND (As), AS_COMMAND_FLUSH_MEM);
  AsWait (Mali, As);
  WR (AS_TRANSTAB_LO (As), 0);
  WR (AS_TRANSTAB_HI (As), 0);
  WR (AS_COMMAND (As), AS_COMMAND_UPDATE);
  AsWait (Mali, As);
}

//
// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------
//

STATIC
VOID
BuildHeader (
  OUT MALI_JOB_HEADER  *Header,
  IN  UINT32           Type,
  IN  UINT16           Index,
  IN  UINT64           Next
  )
{
  ZeroMem (Header, sizeof (*Header));
  Header->Control = 1u | (Type << 1) | ((UINT32)Index << 16);    // 64-bit descriptors
  Header->Next    = Next;
}

VOID
MaliBuildNullJob (
  OUT MALI_JOB_HEADER  *Job,
  IN  UINT16           Index,
  IN  UINT64           NextGpuVa
  )
{
  BuildHeader (Job, MALI_JOB_TYPE_NULL, Index, NextGpuVa);
}

VOID
MaliBuildWriteValueJob (
  OUT MALI_WRITE_VALUE_JOB  *Job,
  IN  UINT16                Index,
  IN  UINT64                NextGpuVa,
  IN  UINT64                TargetGpuVa,
  IN  UINT32                Type
  )
{
  ZeroMem (Job, sizeof (*Job));
  BuildHeader (&Job->Header, MALI_JOB_TYPE_WRITE_VALUE, Index, NextGpuVa);
  Job->Address = TargetGpuVa;
  Job->Type    = Type;
}

VOID
MaliBuildClearJob (
  OUT MALI_FRAGMENT_JOB  *Job,
  OUT MALI_FRAMEBUFFER   *Fbd,
  IN  UINT64             FbdGpuVa,
  IN  UINT64             PolygonListGpuVa,
  IN  UINT64             TargetGpuVa,
  IN  UINT32             Width,
  IN  UINT32             Height,
  IN  UINT32             RowStride,
  IN  UINT32             Color
  )
{
  UINT32  *P = Fbd->Parameters;

  ZeroMem (Fbd, sizeof (*Fbd));
  //
  // Local storage: no TLS, no workgroup memory (WLS instances = log2
  // (0x80000000)).
  //
  Fbd->LocalStorage[1] = 31;
  //
  // Parameters (genxml v4 "Framebuffer Parameters"): raw 32-bit internal /
  // writeback (1_32B_CHANNEL = 3), identity swizzle (R | G << 3 | B << 6 |
  // A << 9 = 0x688), linear (2), single sample, dithering, clean pixel
  // write (= clear), colour write; tie-break MINUS_180_IN_0_OUT (2), ZS
  // format D24S8 (5, unused).
  //
  P[0]  = (0x688u << 6) | (3u << 18) | (2u << 26) | (1u << 28) | (1u << 29) | (1u << 31);
  P[1]  = (2u << 11) | (5u << 18);
  P[2]  = 0;
  P[3]  = (Width - 1) | ((Height - 1) << 16);
  P[12] = (UINT32)TargetGpuVa;                 // Color Writeback: base, row stride
  P[13] = (UINT32)(TargetGpuVa >> 32);
  P[14] = RowStride;
  P[15] = 0;
  P[24] = Color;                               // Clear Color 0..3 (no colour preload)
  P[25] = Color;
  P[26] = Color;
  P[27] = Color;
  //
  // Tiler context, disabled (pan_emit_midgard_tiler with midgard.disable):
  // hierarchy mask DISABLED (4096), minimum header (512), heap = list.
  //
  Fbd->Tiler[0] = 512;
  Fbd->Tiler[1] = 4096;
  Fbd->Tiler[2] = (UINT32)PolygonListGpuVa;
  Fbd->Tiler[3] = (UINT32)(PolygonListGpuVa >> 32);
  Fbd->Tiler[4] = (UINT32)(PolygonListGpuVa + 512);
  Fbd->Tiler[5] = (UINT32)((PolygonListGpuVa + 512) >> 32);
  Fbd->Tiler[6] = (UINT32)PolygonListGpuVa;
  Fbd->Tiler[7] = (UINT32)(PolygonListGpuVa >> 32);
  Fbd->Tiler[8] = (UINT32)PolygonListGpuVa;
  Fbd->Tiler[9] = (UINT32)(PolygonListGpuVa >> 32);

  BuildHeader (&Job->Header, MALI_JOB_TYPE_FRAGMENT, 1, 0);
  Job->BoundMin    = 0;
  Job->BoundMax    = ((Width - 1) >> 4) | (((Height - 1) >> 4) << 16);
  Job->Framebuffer = FbdGpuVa;
}

INTN
MaliJobStart (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       Slot,
  IN UINT32       As,
  IN UINT64       ChainGpuVa
  )
{
  if (RD (JS_COMMAND_NEXT (Slot)) != 0) {
    return -1;                                       // a job is already queued
  }
  WR (JOB_INT_CLEAR, (1u << Slot) | (1u << (Slot + 16)));
  WR (JS_HEAD_NEXT_LO (Slot), (UINT32)ChainGpuVa);
  WR (JS_HEAD_NEXT_HI (Slot), (UINT32)(ChainGpuVa >> 32));
  WR (JS_AFFINITY_NEXT_LO (Slot), Mali->ShaderPresent);
  WR (JS_AFFINITY_NEXT_HI (Slot), 0);
  // T620: HW_ISSUE_10649 -> START_MMU; caches cleaned / invalidated around the chain.
  WR (JS_CONFIG_NEXT (Slot), As | JS_CONFIG_THREAD_PRI (8) | JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE |
                             JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE | JS_CONFIG_START_MMU);
  WR (JS_COMMAND_NEXT (Slot), JS_COMMAND_START);
  return 0;
}

INTN
MaliJobWait (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       Slot,
  IN UINT32       TimeoutMs
  )
{
  UINT32  Raw;
  UINT32  Waited;

  for (Waited = 0; Waited < TimeoutMs * 100; Waited++) {
    Raw = RD (JOB_INT_RAWSTAT);
    if ((Raw & (1u << (Slot + 16))) != 0) {
      WR (JOB_INT_CLEAR, 1u << (Slot + 16));
      return (INTN)RD (JS_STATUS (Slot));
    }
    if ((Raw & (1u << Slot)) != 0) {
      WR (JOB_INT_CLEAR, 1u << Slot);
      return 0;
    }
    MicroSecondDelay (10);
  }
  return -1;
}

VOID
MaliLogFault (
  IN EXYNOS_MALI  *Mali,
  IN UINT32       Slot,
  IN UINT32       As
  )
{
  DEBUG ((DEBUG_INFO, "Mali: JOB_INT_RAWSTAT 0x%08x JS%u STATUS 0x%x HEAD 0x%08x; GPU_INT_RAWSTAT 0x%08x FAULT 0x%x at 0x%08x\n",
          RD (JOB_INT_RAWSTAT), Slot, RD (JS_STATUS (Slot)), RD (JS_HEAD_LO (Slot)), RD (GPU_INT_RAWSTAT),
          RD (GPU_FAULT_STATUS), RD (GPU_FAULT_ADDRESS_LO)));
  DEBUG ((DEBUG_INFO, "Mali: MMU_INT_RAWSTAT 0x%08x AS%u FAULTSTATUS 0x%08x FAULTADDRESS 0x%08x%08x STATUS 0x%x\n",
          RD (MMU_INT_RAWSTAT), As, RD (AS_FAULTSTATUS (As)), RD (AS_FAULTADDRESS_HI (As)),
          RD (AS_FAULTADDRESS_LO (As)), RD (AS_STATUS (As))));
}

#undef RD
#undef WR
