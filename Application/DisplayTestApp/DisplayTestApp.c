/** @file
  Display test (see DisplayTestApp.inf).  Scans out the GOP framebuffer
  UEFI already uses (so the Shell's text is visible again afterwards);
  everything else is brought up from cold by the display cores.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include "../../Drivers/Display/ExynosDisplay.h"

STATIC EXYNOS_I2C_PROTOCOL  *mI2c;

BOOLEAN
EnvI2cWrite (
  IN UINT32  Bus,
  IN UINT8   Addr,
  IN UINT8   *Buf,
  IN UINTN   Count
  )
{
  return (mI2c != NULL) && (mI2c->Write (mI2c, Bus, Addr, Buf, Count) == (INTN)Count);
}

STATIC
VOID
Fill (
  IN UINT32  *Fb,
  IN UINT32  Stride,
  IN UINT32  Colour
  )
{
  UINT32  X;
  UINT32  Y;

  for (Y = 0; Y < EXYNOS_DISP_HEIGHT; Y++) {
    for (X = 0; X < EXYNOS_DISP_WIDTH; X++) {
      Fb[Y * (Stride / 4) + X] = Colour;
    }
  }
}

STATIC
VOID
Bars (
  IN UINT32  *Fb,
  IN UINT32  Stride
  )
{
  STATIC CONST UINT32  Colours[8] = {
    0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000
  };
  UINT32  X;
  UINT32  Y;

  for (Y = 0; Y < EXYNOS_DISP_HEIGHT; Y++) {
    for (X = 0; X < EXYNOS_DISP_WIDTH; X++) {
      Fb[Y * (Stride / 4) + X] = Colours[X / (EXYNOS_DISP_WIDTH / 8)];
    }
  }
}

EFI_STATUS
EFIAPI
DisplayTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EXYNOS_DISPLAY                Disp;
  UINT32                        *Fb;
  INTN                          Result;
  STATIC CONST UINT32           Colours[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };
  STATIC CONST CHAR8            *Names[4]  = { "red", "green", "blue", "white" };
  UINTN                         Index;

  gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&mI2c);
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop))) {
    DEBUG ((DEBUG_ERROR, "DisplayTest: no GOP\n"));
    return EFI_NOT_FOUND;
  }

  ZeroMem (&Disp, sizeof (Disp));
  Disp.Fimd   = EXYNOS_DISP_FIMD1_PHYS;
  Disp.Dp     = EXYNOS_DISP_DP1_PHYS;
  Disp.CmuTop = EXYNOS_DISP_CMU_TOP_PHYS;
  Disp.Pmu    = EXYNOS_DISP_PMU_PHYS;
  Disp.Sysreg = EXYNOS_DISP_SYSREG_PHYS;
  Disp.GpioLb = EXYNOS_DISP_GPIO_LB_PHYS;
  Disp.GpioRt = EXYNOS_DISP_GPIO_RT_PHYS;
  Disp.GpioB  = EXYNOS_DISP_GPIO_B_PHYS;
  Disp.Pwm    = EXYNOS_DISP_PWM_PHYS;
  Disp.FbPhys = (UINT32)Gop->Mode->FrameBufferBase;
  Disp.Stride = Gop->Mode->Info->PixelsPerScanLine * 4;
  Fb          = (UINT32 *)(UINTN)Disp.FbPhys;

  DEBUG ((DEBUG_INFO, "DisplayTest: framebuffer 0x%x stride %u (GOP %ux%u)\n", Disp.FbPhys, Disp.Stride,
          Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution));
  DEBUG ((DEBUG_INFO, "DisplayTest: state as the bootloader left it:\n"));
  ExynosDisplayLogState (&Disp);

  Fill (Fb, Disp.Stride, 0xFF0000);
  DEBUG ((DEBUG_INFO, "DisplayTest: cold bring-up (panel power cycle)\n"));
  Result = ExynosDisplayEnable (&Disp, 0xC0);
  DEBUG ((DEBUG_INFO, "DisplayTest: enable %d after %u tries, step %u, error %d\n", (INT32)Result, Disp.Tries,
          Disp.Step, Disp.Error));
  ExynosDisplayLogState (&Disp);
  if (Result != 0) {
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < 4; Index++) {
    DEBUG ((DEBUG_INFO, "DisplayTest: %a\n", Names[Index]));
    Fill (Fb, Disp.Stride, Colours[Index]);
    gBS->Stall (4 * 1000 * 1000);
  }
  DEBUG ((DEBUG_INFO, "DisplayTest: colour bars\n"));
  Bars (Fb, Disp.Stride);
  {
    STATIC CONST UINT8  Levels[4] = { 255, 16, 64, 32 };     // ends dim: full brightness drains ~2 A

    for (Index = 0; Index < 4; Index++) {
      DEBUG ((DEBUG_INFO, "DisplayTest: brightness %u\n", Levels[Index]));
      ExynosDisplaySetBrightness (&Disp, Levels[Index]);
      gBS->Stall (3 * 1000 * 1000);
    }
  }
  ExynosDisplayLogState (&Disp);
  DEBUG ((DEBUG_INFO, "DisplayTest: done\n"));
  return EFI_SUCCESS;
}
