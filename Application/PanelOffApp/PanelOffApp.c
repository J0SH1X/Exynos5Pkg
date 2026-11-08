/** @file
  PanelOff.efi - panel, backlight and DP link fully off (the display cores'
  ExynosDisplayDisable).  For unattended tests on battery: the backlight is
  the biggest consumer.  DisplayTest.efi brings the panel back.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
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

EFI_STATUS
EFIAPI
PanelOffMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EXYNOS_DISPLAY                Disp;

  gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&mI2c);
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
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop))) {
    Disp.FbPhys = (UINT32)Gop->Mode->FrameBufferBase;
    Disp.Stride = Gop->Mode->Info->PixelsPerScanLine * 4;
  }

  DEBUG ((DEBUG_INFO, "PanelOff: switching the panel off\n"));
  ExynosDisplayDisable (&Disp);
  DEBUG ((DEBUG_INFO, "PanelOff: done\n"));
  return EFI_SUCCESS;
}
