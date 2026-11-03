/** @file
  v1awifi Mali-T628 bring-up test (shell application) over the shared core
  (Drivers/Gpu/MaliCore.c):
    1. vdd_g3d (MAX77802 BUCK4) to the OPP's voltage,
    2. clock, G3D domain, soft reset, IDs and present masks, cores on,
    3. GPU page tables mapping a buffer 1:1 (GPU VA = PA), address space 0,
    4. a NULL job, a WRITE_VALUE "zero" job (the GPU clears a preset
       word), a NULL -> WRITE_VALUE "timestamp" chain,
    5. a shader pass (Drivers/Gpu/MaliShaderStream.h, generated on the host
       with Mesa's v4 packers and Midgard compiler): tiler chain with two
       draws -- a gradient (varyings) and a texture-sampled checkerboard --
       and a fragment job into the top 256 rows of the screen,
    6. everything off again.

    GpuTest.efi [MHz]          (default 266)

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosI2c.h>
#include <Protocol/LoadedImage.h>

#include "../../Drivers/Gpu/MaliCore.h"
#include "../../Drivers/Gpu/MaliShaderStream.h"

#define PMIC_BUS      7
#define PMIC_ADDR     0x09
#define PMIC_SDA      EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB2, 2)
#define PMIC_SCL      EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB2, 3)

#define BUF_PAGES     32              // 128 KiB: jobs +0, framebuffer descriptor +0x100,
                                      // results +0x1000, polygon list +0x2000, target +0x4000
#define TARGET_W      256
#define TARGET_H      64
#define GOP_FB        0x67000000ull   // UEFI's framebuffer (2560 x 1600 x 4)
#define GOP_STRIDE    (2560 * 4)
#define GOP_ROWS      64
#define PT_PAGES      32

VOID
EnvCacheClean (
  IN VOID   *Va,
  IN UINTN  Size
  )
{
  WriteBackDataCacheRange (Va, Size);
}

VOID
EnvCacheInvalidate (
  IN VOID   *Va,
  IN UINTN  Size
  )
{
  InvalidateDataCacheRange (Va, Size);
}

STATIC
UINT32
Voltage (
  UINT32  Mhz
  )
{
  // gpu_exynos5420.c OPP table (µV), 25 mV margin as CpuFreqDxe does
  if (Mhz <= 177) return 812500 + 25000;
  if (Mhz <= 266) return 862500 + 25000;
  if (Mhz <= 350) return 912500 + 25000;
  if (Mhz <= 420) return 962500 + 25000;
  if (Mhz <= 480) return 1000000 + 25000;
  return 1037500 + 25000;
}

STATIC
EFI_STATUS
SetVddG3d (
  UINT32  MicroVolts
  )
{
  EXYNOS_I2C_PROTOCOL   *I2c;
  EXYNOS_GPIO_PROTOCOL  *Gpio;
  UINT8                 Buf[2];
  UINT8                 Level[8];
  UINT8                 Ctrl;
  UINTN                 Index;
  EFI_STATUS            Status;

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&I2c);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&Gpio);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Gpio->Config (Gpio, PMIC_SDA, 3, EXYNOS_GPIO_PULL_UP);
  Gpio->Config (Gpio, PMIC_SCL, 3, EXYNOS_GPIO_PULL_UP);

  if ((I2c->Read (I2c, PMIC_BUS, PMIC_ADDR, MALI_PMIC_BUCK4_CTRL1, &Ctrl, 1) != 1) ||
      (I2c->Read (I2c, PMIC_BUS, PMIC_ADDR, MALI_PMIC_BUCK4_DVS1, Level, 8) != 8)) {
    DEBUG ((DEBUG_ERROR, "GpuTest: MAX77802 not answering\n"));
    return EFI_DEVICE_ERROR;
  }
  DEBUG ((DEBUG_INFO, "GpuTest: BUCK4 CTRL1 0x%02x, DVS1 %u mV (code 0x%02x)\n", Ctrl,
          (600000 + Level[0] * 6250) / 1000, Level[0]));
  for (Index = 0; Index < 8; Index++) {
    Buf[0] = (UINT8)(MALI_PMIC_BUCK4_DVS1 + Index);
    Buf[1] = MALI_BUCK4_CODE (MicroVolts);
    I2c->Write (I2c, PMIC_BUS, PMIC_ADDR, Buf, 2);
  }
  if (((Ctrl >> 4) & 3) != 3) {
    Buf[0] = MALI_PMIC_BUCK4_CTRL1;
    Buf[1] = (UINT8)(Ctrl | (3u << 4));          // normal mode (on)
    I2c->Write (I2c, PMIC_BUS, PMIC_ADDR, Buf, 2);
  }
  gBS->Stall (2000);
  I2c->Read (I2c, PMIC_BUS, PMIC_ADDR, MALI_PMIC_BUCK4_DVS1, Level, 1);
  I2c->Read (I2c, PMIC_BUS, PMIC_ADDR, MALI_PMIC_BUCK4_CTRL1, &Ctrl, 1);
  DEBUG ((DEBUG_INFO, "GpuTest: vdd_g3d now %u mV, CTRL1 0x%02x\n", (600000 + Level[0] * 6250) / 1000, Ctrl));
  return (Level[0] == MALI_BUCK4_CODE (MicroVolts)) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
VOID
DumpWords (
  CONST CHAR8  *What,
  VOID         *Va
  )
{
  UINT32  *W = Va;

  DEBUG ((DEBUG_INFO, "GpuTest: %a: %08x %08x %08x %08x %08x %08x %08x %08x\n", What, W[0], W[1], W[2], W[3], W[4], W[5],
          W[6], W[7]));
}

STATIC
INTN
RunChain (
  EXYNOS_MALI  *Mali,
  CONST CHAR8  *Name,
  UINT8        *Buf,
  UINT32       Slot,
  UINT64       ChainPa
  )
{
  INTN  Result;

  WriteBackDataCacheRange (Buf, BUF_PAGES * EFI_PAGE_SIZE);
  if (MaliJobStart (Mali, Slot, 0, ChainPa) != 0) {
    DEBUG ((DEBUG_ERROR, "GpuTest: %a: slot busy\n", Name));
    return -1;
  }
  Result = MaliJobWait (Mali, Slot, 1000);
  InvalidateDataCacheRange (Buf, BUF_PAGES * EFI_PAGE_SIZE);
  DEBUG ((DEBUG_INFO, "GpuTest: %a: %a (result %d)\n", Name, (Result == 0) ? "done" : (Result < 0) ? "TIMEOUT" : "FAULT",
          (INT32)Result));
  if (Result != 0) {
    MaliLogFault (Mali, Slot, 0);
  }
  return Result;
}

EFI_STATUS
EFIAPI
GpuTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EXYNOS_MALI           Mali;
  MALI_PT_POOL          Pool;
  EFI_PHYSICAL_ADDRESS  BufPa;
  EFI_PHYSICAL_ADDRESS  PtPa;
  EFI_PHYSICAL_ADDRESS  SsPa;
  UINT8                 *Ss;
  UINT8                 *Buf;
  UINT32                *Result;
  MALI_WRITE_VALUE_JOB  *Write;
  MALI_JOB_HEADER       *Null;
  UINT32                Mhz = 266;
  EFI_STATUS            Status;

  DEBUG ((DEBUG_INFO, "GpuTest: start\n"));
  ZeroMem (&Mali, sizeof (Mali));
  Mali.Gpu = MALI_GPU_PHYS;
  Mali.Cmu = MALI_CMU_TOP_PHYS;
  Mali.Pmu = MALI_PMU_G3D_PHYS;

  DEBUG ((DEBUG_INFO, "GpuTest: before: VPLL_CON0 0x%08x SRC_TOP2 0x%08x TOP5 0x%08x TOP6 0x%08x TOP12 0x%08x "
          "DIV_TOP2 0x%08x GATE_IP_G3D 0x%08x, G3D domain 0x%x\n", MmioRead32 (0x10020170), MmioRead32 (0x10020208),
          MmioRead32 (0x10020214), MmioRead32 (0x10020218), MmioRead32 (0x10020288), MmioRead32 (0x10020508),
          MmioRead32 (0x10020930), MmioRead32 (0x10044084)));
  Status = SetVddG3d (Voltage (Mhz));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GpuTest: vdd_g3d - %r, stopping\n", Status));
    return Status;
  }

  if (MaliPowerUp (&Mali, Mhz) != 0) {
    DEBUG ((DEBUG_ERROR, "GpuTest: power-up failed at step %u\n", Mali.Step));
    MaliLogState (&Mali);
    MaliPowerDown (&Mali);
    return EFI_DEVICE_ERROR;
  }
  MaliLogState (&Mali);

  //
  // GPU memory below 4 GiB, mapped 1:1.
  //
  BufPa = 0xFFFFFFFF;
  PtPa  = 0xFFFFFFFF;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, BUF_PAGES, &BufPa)) ||
      EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, PT_PAGES, &PtPa))) {
    MaliPowerDown (&Mali);
    return EFI_OUT_OF_RESOURCES;
  }
  SsPa = 0xFFFFFFFF;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, EFI_SIZE_TO_PAGES (MALI_SS_BUF_SIZE), &SsPa))) {
    MaliPowerDown (&Mali);
    return EFI_OUT_OF_RESOURCES;
  }
  Buf = (UINT8 *)(UINTN)BufPa;
  ZeroMem (Buf, BUF_PAGES * EFI_PAGE_SIZE);
  Pool.Va   = (UINT8 *)(UINTN)PtPa;
  Pool.Pa   = PtPa;
  Pool.Size = PT_PAGES * EFI_PAGE_SIZE;
  if ((MaliPtInit (&Pool) != 0) || (MaliPtMap (&Pool, BufPa, BufPa, BUF_PAGES * EFI_PAGE_SIZE) != 0) ||
      (MaliPtMap (&Pool, GOP_FB, GOP_FB, MALI_SS_TARGET_SIZE) != 0) ||
      (MaliPtMap (&Pool, MALI_SS_BASE, SsPa, MALI_SS_BUF_SIZE) != 0)) {
    DEBUG ((DEBUG_ERROR, "GpuTest: page tables failed\n"));
    MaliPowerDown (&Mali);
    return EFI_OUT_OF_RESOURCES;
  }
  DEBUG ((DEBUG_INFO, "GpuTest: buffer at 0x%lx, page tables at 0x%lx (%u bytes used)\n", BufPa, PtPa, Pool.Used));
  if (MaliAsEnable (&Mali, 0, &Pool) != 0) {
    DEBUG ((DEBUG_ERROR, "GpuTest: address space 0 setup failed\n"));
    MaliLogFault (&Mali, 0, 0);
  }

  Null   = (MALI_JOB_HEADER *)(Buf + 0x000);
  Write  = (MALI_WRITE_VALUE_JOB *)(Buf + 0x040);
  Result = (UINT32 *)(Buf + 0x1000);

  // 1. NULL job
  MaliBuildNullJob (Null, 1, 0);
  RunChain (&Mali, "NULL job", Buf, MALI_SLOT_OTHER, BufPa + 0x000);
  DumpWords ("NULL job header after", Null);

  // 2. WRITE_VALUE Zero over a preset pattern (T62x has no immediate values)
  Result[0] = 0xFFFFFFFF;
  Result[1] = 0xFFFFFFFF;
  MaliBuildWriteValueJob (Write, 1, 0, BufPa + 0x1000, MALI_WRITE_VALUE_ZERO);
  RunChain (&Mali, "WRITE_VALUE zero", Buf, MALI_SLOT_OTHER, BufPa + 0x040);
  DumpWords ("WRITE_VALUE job after", Write);
  DEBUG ((DEBUG_INFO, "GpuTest: result 0x%08x%08x (%a)\n", Result[1], Result[0],
          ((Result[0] | Result[1]) == 0) ? "GPU wrote it" : "unchanged"));

  // 3. chain: NULL -> WRITE_VALUE timestamp (non-zero 64-bit value)
  Result[0] = 0;
  Result[1] = 0;
  MaliBuildNullJob (Null, 1, BufPa + 0x040);
  MaliBuildWriteValueJob (Write, 2, 0, BufPa + 0x1000, MALI_WRITE_VALUE_TIMESTAMP);
  RunChain (&Mali, "NULL -> WRITE_VALUE timestamp chain", Buf, MALI_SLOT_OTHER, BufPa + 0x000);
  DEBUG ((DEBUG_INFO, "GpuTest: timestamp 0x%08x%08x (%a)\n", Result[1], Result[0],
          ((Result[0] | Result[1]) != 0) ? "chain ran" : "unchanged"));

  // 4. clear-only render pass into a 256 x 64 buffer (fragment job, slot 0)
  {
    MALI_FRAMEBUFFER   *Fbd  = (MALI_FRAMEBUFFER *)(Buf + 0x100);
    MALI_FRAGMENT_JOB  *Frag = (MALI_FRAGMENT_JOB *)(Buf + 0x280);
    UINT32             *Px   = (UINT32 *)(Buf + 0x4000);
    UINTN              Index;
    UINTN              Good;

    SetMem32 (Px, TARGET_W * TARGET_H * 4, 0x01010101);
    MaliBuildClearJob (Frag, Fbd, BufPa + 0x100, BufPa + 0x2000, BufPa + 0x4000, TARGET_W, TARGET_H, TARGET_W * 4,
                       0xFF2080E0);
    RunChain (&Mali, "clear 256x64", Buf, MALI_SLOT_FRAGMENT, BufPa + 0x280);
    DumpWords ("fragment job after", Frag);
    for (Index = 0, Good = 0; Index < TARGET_W * TARGET_H; Index++) {
      Good += (Px[Index] == 0xFF2080E0) ? 1 : 0;
    }
    DEBUG ((DEBUG_INFO, "GpuTest: clear: %u of %u pixels 0xff2080e0 (first %08x, last %08x)\n", Good,
            TARGET_W * TARGET_H, Px[0], Px[TARGET_W * TARGET_H - 1]));

    // 5. the same into the top of the screen (UEFI's framebuffer): visible
    if (Good == TARGET_W * TARGET_H) {
      MaliBuildClearJob (Frag, Fbd, BufPa + 0x100, BufPa + 0x2000, GOP_FB, 2560, GOP_ROWS, GOP_STRIDE, 0xFF00C000);
      RunChain (&Mali, "clear screen strip", Buf, MALI_SLOT_FRAGMENT, BufPa + 0x280);
      DEBUG ((DEBUG_INFO, "GpuTest: screen strip: first pixel %08x\n", *(volatile UINT32 *)(UINTN)GOP_FB));
    }
  }

  // 6. shader pass: polygon-list zero (WRITE_VALUE) -> tiler A -> tiler B on JS1, then fragment on JS0
  {
    INTN  R;

    Ss = (UINT8 *)(UINTN)SsPa;
    ZeroMem (Ss, MALI_SS_BUF_SIZE);
    CopyMem (Ss, mMaliShaderStream, sizeof (mMaliShaderStream));
    WriteBackDataCacheRange (Ss, MALI_SS_BUF_SIZE);
    R = MaliJobStart (&Mali, MALI_SLOT_OTHER, 0, MALI_SS_BASE + MALI_SS_TILER);
    R = (R == 0) ? MaliJobWait (&Mali, MALI_SLOT_OTHER, 1000) : -1;
    DEBUG ((DEBUG_INFO, "GpuTest: shader pass tiler chain: %a (result %d)\n", (R == 0) ? "done" : (R < 0) ? "TIMEOUT" : "FAULT", (INT32)R));
    if (R != 0) {
      MaliLogFault (&Mali, MALI_SLOT_OTHER, 0);
    } else {
      R = MaliJobStart (&Mali, MALI_SLOT_FRAGMENT, 0, MALI_SS_BASE + MALI_SS_FRAGMENT);
      R = (R == 0) ? MaliJobWait (&Mali, MALI_SLOT_FRAGMENT, 1000) : -1;
      DEBUG ((DEBUG_INFO, "GpuTest: shader pass fragment job: %a (result %d)\n", (R == 0) ? "done" : (R < 0) ? "TIMEOUT" : "FAULT", (INT32)R));
      if (R != 0) {
        MaliLogFault (&Mali, MALI_SLOT_FRAGMENT, 0);
      }
    }
    InvalidateDataCacheRange (Ss, MALI_SS_BUF_SIZE);
    DumpWords ("tiler job A after", Ss + 0x000);
    DumpWords ("tiler job B after", Ss + 0x100);
    DumpWords ("polygon list", Ss + 0x10000);
    DEBUG ((DEBUG_INFO, "GpuTest: screen: (10,10) %08x (1270,128) %08x (1290,10) %08x (2550,250) %08x\n",
            *(volatile UINT32 *)(UINTN)(GOP_FB + 10 * GOP_STRIDE + 10 * 4),
            *(volatile UINT32 *)(UINTN)(GOP_FB + 128 * GOP_STRIDE + 1270 * 4),
            *(volatile UINT32 *)(UINTN)(GOP_FB + 10 * GOP_STRIDE + 1290 * 4),
            *(volatile UINT32 *)(UINTN)(GOP_FB + 250 * GOP_STRIDE + 2550 * 4)));
    gBS->FreePages (SsPa, EFI_SIZE_TO_PAGES (MALI_SS_BUF_SIZE));
  }

  MaliAsDisable (&Mali, 0);
  MaliPowerDown (&Mali);
  gBS->FreePages (BufPa, BUF_PAGES);
  gBS->FreePages (PtPa, PT_PAGES);
  DEBUG ((DEBUG_INFO, "GpuTest: done, GPU off\n"));
  return EFI_SUCCESS;
}
