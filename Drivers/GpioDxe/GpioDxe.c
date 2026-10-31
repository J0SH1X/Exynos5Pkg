/** @file
  Exynos5420 GPIO DXE driver.

  Installs gExynosGpioProtocolGuid over the Exynos5420 GPIO register banks.
  Register layout follows the kernel's samsung_gpio_chip tables
  (drivers/gpio/gpio-samsung.c, exynos5420_gpios_{R,T,L,B,AUDIO}): every
  bank is a CON/DAT/PULL triplet (0x00/0x04/0x08) spaced 0x20 bytes apart
  inside one of the five GPIO controllers.

  CON is 4 bits per pin (s3c_gpio_setcfg_s3c64xx_4bit): 0 = input,
  1 = output, >= 2 = alternate function.  PULL is 2 bits per pin with the
  Exynos encoding (pull-up = 3, pull-down = 1, disabled = 0).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosGpio.h>

//
// The five GPIO controllers (map.h: EXYNOS5410_PA_GPIO_RT / _LB,
// EXYNOS5420_PA_GPIO_T / _B, EXYNOS4_PA_GPIO3).
//
#define EXYNOS5420_PA_GPIO_LB  0x14000000
#define EXYNOS5420_PA_GPIO_RT  0x13400000
#define EXYNOS5420_PA_GPIO_T   0x13410000
#define EXYNOS5420_PA_GPIO_B   0x14010000
#define EXYNOS5420_PA_GPIO_AUD 0x03860000

#define GPIO_CON_OFFSET    0x00
#define GPIO_DAT_OFFSET    0x04
#define GPIO_PULL_OFFSET   0x08

//
// One entry per ExynosGpioBank, in enum order.  Base is the full MMIO
// address of the bank, Ngpio its pin count (kernel EXYNOS5420_GPIO_*_NR
// tables).  GPX lives at +0xC00 in the RT controller, not at i*0x20.
//
typedef struct {
  UINT32  Base;
  UINT8   Ngpio;
} EXYNOS_GPIO_BANK_DESC;

STATIC CONST EXYNOS_GPIO_BANK_DESC mGpioBanks[EXYNOS_GPIO_BANK_MAX] = {
  // Controller B
  [EXYNOS_GPIO_BANK_GPA0] = { EXYNOS5420_PA_GPIO_B + 0x000, 8 },
  [EXYNOS_GPIO_BANK_GPA1] = { EXYNOS5420_PA_GPIO_B + 0x020, 6 },
  [EXYNOS_GPIO_BANK_GPA2] = { EXYNOS5420_PA_GPIO_B + 0x040, 8 },
  [EXYNOS_GPIO_BANK_GPB0] = { EXYNOS5420_PA_GPIO_B + 0x060, 5 },
  [EXYNOS_GPIO_BANK_GPB1] = { EXYNOS5420_PA_GPIO_B + 0x080, 5 },
  [EXYNOS_GPIO_BANK_GPB2] = { EXYNOS5420_PA_GPIO_B + 0x0A0, 4 },
  [EXYNOS_GPIO_BANK_GPB3] = { EXYNOS5420_PA_GPIO_B + 0x0C0, 8 },
  [EXYNOS_GPIO_BANK_GPB4] = { EXYNOS5420_PA_GPIO_B + 0x0E0, 2 },

  // Controller T
  [EXYNOS_GPIO_BANK_GPC0] = { EXYNOS5420_PA_GPIO_T + 0x000, 8 },
  [EXYNOS_GPIO_BANK_GPC1] = { EXYNOS5420_PA_GPIO_T + 0x020, 8 },
  [EXYNOS_GPIO_BANK_GPC2] = { EXYNOS5420_PA_GPIO_T + 0x040, 7 },
  [EXYNOS_GPIO_BANK_GPC3] = { EXYNOS5420_PA_GPIO_T + 0x060, 4 },
  [EXYNOS_GPIO_BANK_GPC4] = { EXYNOS5420_PA_GPIO_T + 0x080, 2 },
  [EXYNOS_GPIO_BANK_GPD1] = { EXYNOS5420_PA_GPIO_T + 0x0A0, 8 },
  [EXYNOS_GPIO_BANK_GPY0] = { EXYNOS5420_PA_GPIO_T + 0x0C0, 6 },
  [EXYNOS_GPIO_BANK_GPY1] = { EXYNOS5420_PA_GPIO_T + 0x0E0, 4 },
  [EXYNOS_GPIO_BANK_GPY2] = { EXYNOS5420_PA_GPIO_T + 0x100, 6 },
  [EXYNOS_GPIO_BANK_GPY3] = { EXYNOS5420_PA_GPIO_T + 0x120, 8 },
  [EXYNOS_GPIO_BANK_GPY4] = { EXYNOS5420_PA_GPIO_T + 0x140, 8 },
  [EXYNOS_GPIO_BANK_GPY5] = { EXYNOS5420_PA_GPIO_T + 0x160, 8 },
  [EXYNOS_GPIO_BANK_GPY6] = { EXYNOS5420_PA_GPIO_T + 0x180, 8 },

  // Controller LB
  [EXYNOS_GPIO_BANK_GPE0] = { EXYNOS5420_PA_GPIO_LB + 0x000, 8 },
  [EXYNOS_GPIO_BANK_GPE1] = { EXYNOS5420_PA_GPIO_LB + 0x020, 2 },
  [EXYNOS_GPIO_BANK_GPF0] = { EXYNOS5420_PA_GPIO_LB + 0x040, 6 },
  [EXYNOS_GPIO_BANK_GPF1] = { EXYNOS5420_PA_GPIO_LB + 0x060, 8 },
  [EXYNOS_GPIO_BANK_GPG0] = { EXYNOS5420_PA_GPIO_LB + 0x080, 8 },
  [EXYNOS_GPIO_BANK_GPG1] = { EXYNOS5420_PA_GPIO_LB + 0x0A0, 8 },
  [EXYNOS_GPIO_BANK_GPG2] = { EXYNOS5420_PA_GPIO_LB + 0x0C0, 2 },
  [EXYNOS_GPIO_BANK_GPJ4] = { EXYNOS5420_PA_GPIO_LB + 0x0E0, 4 },

  // Controller RT (GPY7 is at +0x000; GPX0-3 live at +0xC00..+0xC60)
  [EXYNOS_GPIO_BANK_GPY7] = { EXYNOS5420_PA_GPIO_RT + 0x000, 8 },
  [EXYNOS_GPIO_BANK_GPX0] = { EXYNOS5420_PA_GPIO_RT + 0xC00, 8 },
  [EXYNOS_GPIO_BANK_GPX1] = { EXYNOS5420_PA_GPIO_RT + 0xC20, 8 },
  [EXYNOS_GPIO_BANK_GPX2] = { EXYNOS5420_PA_GPIO_RT + 0xC40, 8 },
  [EXYNOS_GPIO_BANK_GPX3] = { EXYNOS5420_PA_GPIO_RT + 0xC60, 8 },

  // Controller AUDIO
  [EXYNOS_GPIO_BANK_GPZ]  = { EXYNOS5420_PA_GPIO_AUD + 0x000, 7 },
};

/**
  Look a pin up in the bank table.

  @param[in]  Pin   Encoded pin.
  @param[out] Base  MMIO base of the bank, on success.
  @param[out] Shift CON/PULL field shift for the pin, on success.

  @retval TRUE  Pin is valid; Base/Shift describe it.
  @retval FALSE Pin out of range.
**/
STATIC
BOOLEAN
LookupPin (
  IN  UINTN   Pin,
  OUT UINT32  *Base,
  OUT UINTN   *Shift
  )
{
  UINTN                      Bank;
  UINTN                      Offset;
  CONST EXYNOS_GPIO_BANK_DESC *Desc;

  Bank   = EXYNOS_GPIO_BANK (Pin);
  Offset = EXYNOS_GPIO_OFFSET (Pin);

  if (Bank >= EXYNOS_GPIO_BANK_MAX) {
    return FALSE;
  }

  Desc = &mGpioBanks[Bank];
  if (Offset >= Desc->Ngpio) {
    return FALSE;
  }

  *Base  = Desc->Base;
  *Shift = Offset;
  return TRUE;
}

STATIC
UINTN
EFIAPI
GpioGet (
  IN EXYNOS_GPIO_PROTOCOL  *This,
  IN UINTN                 Pin
  )
{
  UINT32  Base;
  UINTN   Shift;

  if (!LookupPin (Pin, &Base, &Shift)) {
    DEBUG ((DEBUG_ERROR, "%a: bad pin 0x%x\n", __FUNCTION__, (UINT32)Pin));
    return 0;
  }

  return (MmioRead32 (Base + GPIO_DAT_OFFSET) >> Shift) & 1;
}

STATIC
EFI_STATUS
EFIAPI
GpioSet (
  IN EXYNOS_GPIO_PROTOCOL  *This,
  IN UINTN                 Pin,
  IN UINTN                 Value
  )
{
  UINT32  Base;
  UINTN   Shift;
  UINT32  Dat;

  if (!LookupPin (Pin, &Base, &Shift)) {
    return EFI_INVALID_PARAMETER;
  }

  Dat = MmioRead32 (Base + GPIO_DAT_OFFSET);
  if (Value) {
    Dat |= (1U << Shift);
  } else {
    Dat &= ~(1U << Shift);
  }
  MmioWrite32 (Base + GPIO_DAT_OFFSET, Dat);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GpioConfig (
  IN EXYNOS_GPIO_PROTOCOL  *This,
  IN UINTN                 Pin,
  IN UINTN                 Function,
  IN UINTN                 Pull
  )
{
  UINT32  Base;
  UINTN   Shift;
  UINT32  Reg;
  UINTN   ConShift;

  if (!LookupPin (Pin, &Base, &Shift)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // CON: 4 bits per pin.
  //
  ConShift = Shift * 4;
  Reg      = MmioRead32 (Base + GPIO_CON_OFFSET);
  Reg     &= ~(0xFU << ConShift);
  Reg     |= ((UINT32)(Function & 0xF) << ConShift);
  MmioWrite32 (Base + GPIO_CON_OFFSET, Reg);

  //
  // PULL: 2 bits per pin, Exynos encoding (pull-up = 3).
  //
  Reg = MmioRead32 (Base + GPIO_PULL_OFFSET);
  Reg &= ~(0x3U << (Shift * 2));
  Reg |= ((UINT32)(Pull & 0x3) << (Shift * 2));
  MmioWrite32 (Base + GPIO_PULL_OFFSET, Reg);

  return EFI_SUCCESS;
}

STATIC EXYNOS_GPIO_PROTOCOL mExynosGpio = {
  GpioGet,
  GpioSet,
  GpioConfig,
};

EFI_STATUS
EFIAPI
GpioDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gExynosGpioProtocolGuid,
                  &mExynosGpio,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return Status;
}
