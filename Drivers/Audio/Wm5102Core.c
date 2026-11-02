/** @file
  WM5102 codec core (see Wm5102Core.h).  Sequences from the Samsung
  v1awifi kernel: drivers/mfd/arizona-core.c (arizona_dev_init(),
  arizona_wait_for_boot(), arizona_apply_hardware_patch()),
  drivers/mfd/wm5102-tables.c (wm5102_patch(): the rev B table is applied
  for every revision there, plus wm5102t_pwr_4 for wm5102t_output_pwr = 4),
  sound/soc/codecs/arizona.c (arizona_calc_fll() / arizona_enable_fll(),
  AIF format and BCLK tables) and register map
  include/linux/mfd/arizona/registers.h.

  SPI framing (wm5102_spi_regmap): 32-bit big-endian register address
  (bit 31 set for a read), 16 pad bits, 16-bit big-endian value.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Wm5102Core.h"

#define R_SOFTWARE_RESET          0x000
#define R_DEVICE_REVISION         0x001
#define R_WSEQ_CTRL_0             0x016
#define R_WSEQ_CTRL_1             0x017
#define R_WSEQ_STATE              0x019
#define R_CLOCK_32K_1             0x100
#define R_SYSTEM_CLOCK_1          0x101
#define R_SAMPLE_RATE_1           0x102
#define R_FLL1_CONTROL_1          0x171
#define R_FLL1_CONTROL_2          0x172
#define R_FLL1_CONTROL_3          0x173
#define R_FLL1_CONTROL_4          0x174
#define R_FLL1_CONTROL_5          0x175
#define R_FLL1_CONTROL_6          0x176
#define R_FLL1_GAIN               0x17A    // base + 9: the reference loop's gain
#define R_FLL1_SYNCHRONISER_1     0x181
#define R_FLL1_SYNCHRONISER_7     0x187
#define R_FLL1_SYNC_BW            0x188    // base + 0x17
#define R_OUTPUT_ENABLES_1        0x400
#define R_OUTPUT_STATUS_1         0x401
#define R_RAW_OUTPUT_STATUS_1     0x406
#define R_DAC_DIGITAL_VOLUME_1L   0x411
#define R_DAC_DIGITAL_VOLUME_1R   0x415
#define R_DAC_DIGITAL_VOLUME_4L   0x429
#define R_DAC_DIGITAL_VOLUME_4R   0x42D
#define R_AIF1_BCLK_CTRL          0x500
#define R_AIF1_TX_PIN_CTRL        0x501
#define R_AIF1_RX_PIN_CTRL        0x502
#define R_AIF1_RATE_CTRL          0x503
#define R_AIF1_FORMAT             0x504
#define R_AIF1_TX_BCLK_RATE       0x505
#define R_AIF1_RX_BCLK_RATE       0x506
#define R_AIF1_FRAME_CTRL_1       0x507
#define R_AIF1_FRAME_CTRL_2       0x508
#define R_MIC_CHARGE_PUMP_1       0x200
#define R_MIC_BIAS_CTRL_1         0x218
#define R_INPUT_ENABLES           0x300
#define R_IN1L_CONTROL            0x310
#define R_ADC_DIGITAL_VOLUME_1L   0x311
#define R_IN1R_CONTROL            0x314
#define R_ADC_DIGITAL_VOLUME_1R   0x315
#define R_AIF1TX1MIX_INPUT_1_SOURCE 0x700
#define R_AIF1TX1MIX_INPUT_1_VOLUME 0x701
#define R_AIF1TX2MIX_INPUT_1_SOURCE 0x708
#define R_AIF1TX2MIX_INPUT_1_VOLUME 0x709
#define R_AIF1_TX_ENABLES         0x519
#define R_AIF1_RX_ENABLES         0x51A
#define R_MICD_CLAMP_CONTROL      0x2A2
#define R_JACK_DETECT_ANALOGUE    0x2D3
#define R_GPIO5_CTRL              0xC04
#define R_AOD_IRQ_RAW_STATUS      0xD55
#define R_JACK_DETECT_DEBOUNCE    0xD56
#define R_OUT1LMIX_INPUT_1_SOURCE 0x680
#define R_OUT1LMIX_INPUT_1_VOLUME 0x681
#define R_OUT1RMIX_INPUT_1_SOURCE 0x688
#define R_OUT1RMIX_INPUT_1_VOLUME 0x689
#define R_OUT4LMIX_INPUT_1_SOURCE 0x6B0
#define R_OUT4LMIX_INPUT_1_VOLUME 0x6B1
#define R_OUT4RMIX_INPUT_1_SOURCE 0x6B8
#define R_OUT4RMIX_INPUT_1_VOLUME 0x6B9
#define R_WAKE_CONTROL            0x040
#define R_IRQ_CTRL_1              0xC0F
#define R_INTERRUPT_STATUS_1      0xD00
#define R_INTERRUPT_STATUS_1_MASK 0xD08
#define R_INTERRUPT_CONTROL       0xD0F
#define R_AOD_IRQ1                0xD51
#define R_AOD_IRQ_MASK_IRQ1       0xD53
#define R_INTERRUPT_STATUS_5      0xD04
#define R_INTERRUPT_RAW_STATUS_5  0xD23

#define BOOT_DONE_STS             0x0100
#define FLL1_CLOCK_OK_STS         0x0001
#define FLL1_ENA                  0x0001
#define FLL1_FREERUN              0x0002
#define FLL1_CTRL_UPD             0x8000
#define WSEQ_ENA                  0x0200
#define WSEQ_START                0x0400
#define WSEQ_ABORT                0x0800
#define WSEQ_BUSY                 0x0200
#define CLK_32K_ENA               0x0040
#define SYSCLK_ENA                0x0040
#define SYSCLK_SRC_FLL1           0x0004
#define SYSCLK_FREQ_24M576        (2u << 8)
#define SAMPLE_RATE_48K           0x0003
#define OUT1L_ENA                 0x0002
#define OUT1R_ENA                 0x0001
#define OUT1_ENA                  (OUT1L_ENA | OUT1R_ENA)
#define MICD_CLAMP_STS            0x0008
#define MICD_CLAMP_FALL_EINT1     0x0080
#define MICD_CLAMP_RISE_EINT1     0x0040
#define IRQ_POL                   0x0400       // set: pin active low (chip default)
#define IM_IRQ1                   0x0001
#define MICD_CLAMP_DB             0x0008
#define JD1_DB                    0x0001
#define JD1_ENA                   0x0001
#define GPIO5_INPUT_NOPULL        0x8101       // 0xC101 (dir in, pull-up, fn 1) without GPN_PU
#define OUT4L_ENA                 0x0080
#define OUT4R_ENA                 0x0040
#define OUT_VU                    0x0200
#define OUT_MUTE                  0x0100
#define AIF1_BCLK_MSTR            0x0020
#define AIF1_BCLK_1M536           11       // arizona_48k_bclk_rates[11]
#define AIF1_LRCLK_MSTR           0x0001
#define AIF1_LRCLK_FRC            0x0002       // drive LRCLK even with no channel enabled
#define AIF1TX_LRCLK_SRC          0x0008       // TX uses the shared AIF1 LRCLK (reset value)
#define AIF1_FMT_I2S              2
#define MIX_SRC_AIF1RX1           0x20
#define MIX_SRC_AIF1RX2           0x21
#define MIX_VOL_0DB               0x80
#define MIX_SRC_IN1L              0x10
#define MIX_SRC_IN1R              0x11
#define CPMIC_ENA                 0x0001
#define CPMIC_BYPASS              0x0002
#define MICB1_ENA                 0x0001
#define MICB1_DISCH               0x0004
#define MICB1_LVL_2V6             (((2600 - 1500) / 100) << 5)
#define IN1L_ENA                  0x0002
#define IN1R_ENA                  0x0001
#define IN_PGA_VOL_0DB            0x40         // IN1x_PGA_VOL [7:1], 1 dB steps up to 0x5F
#define IN_VU                     0x0200
#define IN_MUTE                   0x0100

typedef struct {
  UINT16  Reg;
  UINT16  Value;
} REG_VALUE;

STATIC CONST REG_VALUE  mRevBPatch[] = {
  { 0x019, 0x0001 }, { 0x080, 0x0003 }, { 0x081, 0xE022 }, { 0x410, 0x6080 },
  { 0x418, 0xA080 }, { 0x420, 0xA080 }, { 0x428, 0xE000 }, { 0x442, 0x3F0A },
  { 0x443, 0xDC1F }, { 0x4B0, 0x0066 }, { 0x458, 0x000B }, { 0x212, 0x0000 },
  { 0x171, 0x0000 }, { 0x35E, 0x000C }, { 0x2D4, 0x0000 }, { 0x4DC, 0x0900 },
  { 0x080, 0x0000 },
};

STATIC CONST REG_VALUE  mOutputPower4[] = {
  { 0x462, 0x0C00 }, { 0x464, 0x0C00 }, { 0x466, 0x0C00 }, { 0x468, 0x0C00 },
  { 0x46A, 0x0C00 }, { 0x46C, 0x0C00 }, { 0x46E, 0x0C00 }, { 0x470, 0x0C00 },
  { 0x472, 0x0C00 }, { 0x474, 0x0C00 }, { 0x476, 0x0C00 }, { 0x478, 0x0C00 },
  { 0x47A, 0x0C00 }, { 0x47C, 0x0C00 }, { 0x47E, 0x0C00 },
};

INTN
Wm5102Write (
  IN WM5102  *Codec,
  IN UINT32  Reg,
  IN UINT16  Value
  )
{
  UINT8  Tx[8];

  Tx[0] = (UINT8)(Reg >> 24);
  Tx[1] = (UINT8)(Reg >> 16);
  Tx[2] = (UINT8)(Reg >> 8);
  Tx[3] = (UINT8)Reg;
  Tx[4] = 0;
  Tx[5] = 0;
  Tx[6] = (UINT8)(Value >> 8);
  Tx[7] = (UINT8)Value;
  return Codec->Xfer (Codec->Context, Tx, sizeof (Tx), NULL, 0);
}

INTN
Wm5102Read (
  IN  WM5102  *Codec,
  IN  UINT32  Reg,
  OUT UINT16  *Value
  )
{
  UINT8  Tx[6];
  UINT8  Rx[2];
  INTN   Result;

  Reg  |= 0x80000000u;
  Tx[0] = (UINT8)(Reg >> 24);
  Tx[1] = (UINT8)(Reg >> 16);
  Tx[2] = (UINT8)(Reg >> 8);
  Tx[3] = (UINT8)Reg;
  Tx[4] = 0;
  Tx[5] = 0;
  Rx[0] = 0xFF;
  Rx[1] = 0xFF;
  Result = Codec->Xfer (Codec->Context, Tx, sizeof (Tx), Rx, sizeof (Rx));
  *Value = (UINT16)((Rx[0] << 8) | Rx[1]);
  return Result;
}

INTN
Wm5102Update (
  IN WM5102  *Codec,
  IN UINT32  Reg,
  IN UINT16  Mask,
  IN UINT16  Value
  )
{
  UINT16  Old;

  if (Wm5102Read (Codec, Reg, &Old) < 0) {
    return -1;
  }
  return Wm5102Write (Codec, Reg, (UINT16)((Old & ~Mask) | (Value & Mask)));
}

STATIC
INTN
WriteTable (
  IN WM5102           *Codec,
  IN CONST REG_VALUE  *Table,
  IN UINTN            Count
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; Index++) {
    if (Wm5102Write (Codec, Table[Index].Reg, Table[Index].Value) < 0) {
      return -1;
    }
  }
  return 0;
}

/**
  Poll Reg until (value & Mask) == Target, about TimeoutMs milliseconds.
**/
STATIC
INTN
PollReg (
  IN WM5102  *Codec,
  IN UINT32  Reg,
  IN UINT16  Mask,
  IN UINT16  Target,
  IN UINTN   TimeoutMs
  )
{
  UINTN   Ms;
  UINT16  Value;

  for (Ms = 0; Ms <= TimeoutMs; Ms++) {
    if ((Wm5102Read (Codec, Reg, &Value) == 0) && ((Value & Mask) == Target)) {
      return 0;
    }
    EnvSleepMs (1);
  }
  return -1;
}

/**
  arizona_apply_hardware_patch(): SYSCLK from FLL1 free-running, write
  sequence 160, then the previous FLL / SYSCLK settings back.
**/
STATIC
INTN
HardwarePatch (
  IN WM5102  *Codec
  )
{
  UINT16  Fll;
  UINT16  SysClk;
  INTN    Result;

  if ((Wm5102Read (Codec, R_FLL1_CONTROL_1, &Fll) < 0) || (Wm5102Read (Codec, R_SYSTEM_CLOCK_1, &SysClk) < 0)) {
    return -1;
  }
  Wm5102Write (Codec, R_FLL1_CONTROL_1, FLL1_ENA | FLL1_FREERUN);
  Result = PollReg (Codec, R_INTERRUPT_RAW_STATUS_5, FLL1_CLOCK_OK_STS, FLL1_CLOCK_OK_STS, 25);
  if (Result == 0) {
    Wm5102Write (Codec, R_SYSTEM_CLOCK_1, 0x0144);
    Wm5102Write (Codec, R_WSEQ_CTRL_0, WSEQ_ENA | WSEQ_START | 160);
    Result = PollReg (Codec, R_WSEQ_CTRL_1, WSEQ_BUSY, 0, 50);
    if (Result != 0) {
      Wm5102Write (Codec, R_WSEQ_CTRL_0, WSEQ_ABORT);
    }
    Wm5102Write (Codec, R_SYSTEM_CLOCK_1, SysClk);
  }
  Wm5102Write (Codec, R_FLL1_CONTROL_1, Fll);
  DEBUG ((DEBUG_INFO, "Wm5102: hardware patch %a\n", (Result == 0) ? "done" : "FAILED (FLL or sequencer timeout)"));
  return Result;
}

INTN
Wm5102Init (
  IN OUT WM5102  *Codec
  )
{
  UINT16  Id;
  UINT16  Rev;
  UINT16  Wseq;

  Codec->Id        = 0;
  Codec->Playing   = FALSE;
  Codec->Capturing = FALSE;
  Codec->ClocksOn  = FALSE;
  if ((Wm5102Read (Codec, R_SOFTWARE_RESET, &Id) < 0) || (Wm5102Read (Codec, R_DEVICE_REVISION, &Rev) < 0)) {
    DEBUG ((DEBUG_ERROR, "Wm5102: SPI transfer failed\n"));
    return -1;
  }
  DEBUG ((DEBUG_INFO, "Wm5102: device ID 0x%04x revision %u\n", Id, Rev & 0x7));
  if (Id != 0x5102) {
    return -1;
  }
  Codec->Id  = Id;
  Codec->Rev = Rev & 0x7;

  //
  // Booted already if the write sequencer ran (0x19 bit 0), else wait for
  // BOOT_DONE and acknowledge it.
  //
  if ((Wm5102Read (Codec, R_WSEQ_STATE, &Wseq) < 0) || ((Wseq & 0x1) == 0)) {
    if (PollReg (Codec, R_INTERRUPT_RAW_STATUS_5, BOOT_DONE_STS, BOOT_DONE_STS, 50) != 0) {
      DEBUG ((DEBUG_ERROR, "Wm5102: no BOOT_DONE\n"));
      return -1;
    }
    Wm5102Write (Codec, R_INTERRUPT_STATUS_5, BOOT_DONE_STS);
  }

  if ((WriteTable (Codec, mRevBPatch, ARRAY_SIZE (mRevBPatch)) < 0) ||
      (WriteTable (Codec, mOutputPower4, ARRAY_SIZE (mOutputPower4)) < 0) ||
      (HardwarePatch (Codec) < 0))
  {
    return -1;
  }

  //
  // 32 kHz clock from MCLK2 (the chip default source; 32.768 kHz on this
  // board), then FLL1 out of free-running mode (arizona_init_fll()).
  //
  Wm5102Write (Codec, R_CLOCK_32K_1, CLK_32K_ENA | (2 - 1));
  Wm5102Update (Codec, R_FLL1_CONTROL_1, FLL1_FREERUN, 0);
  return 0;
}

/**
  FLL1, reference loop only, from MCLK1 = 24 MHz to 24.576 MHz
  (arizona_calc_fll(): refdiv /2 -> 12 MHz, outdiv 4 -> Fvco 98.304 MHz,
  FRATIO 0, N 8, THETA / LAMBDA 24 / 125, gain 4).
**/
STATIC
INTN
StartFll1 (
  IN WM5102  *Codec
  )
{
  Wm5102Update (Codec, R_FLL1_CONTROL_5, 0x070E, (0u << 8) | (4u << 1));
  Wm5102Write (Codec, R_FLL1_CONTROL_3, 24);
  Wm5102Write (Codec, R_FLL1_CONTROL_4, 125);
  Wm5102Update (Codec, R_FLL1_CONTROL_6, 0x00CF, (1u << 6) | 0);
  Wm5102Update (Codec, R_FLL1_GAIN, 0x003C, 4u << 2);
  Wm5102Update (Codec, R_FLL1_CONTROL_2, FLL1_CTRL_UPD | 0x03FF, FLL1_CTRL_UPD | 8);
  Wm5102Update (Codec, R_FLL1_SYNCHRONISER_1, 0x0001, 0);
  Wm5102Update (Codec, R_FLL1_SYNC_BW, 0x0001, 0x0001);
  Wm5102Write (Codec, R_INTERRUPT_STATUS_5, FLL1_CLOCK_OK_STS);
  Wm5102Update (Codec, R_FLL1_CONTROL_1, FLL1_ENA | FLL1_FREERUN, FLL1_ENA);
  if (PollReg (Codec, R_INTERRUPT_RAW_STATUS_5, FLL1_CLOCK_OK_STS, FLL1_CLOCK_OK_STS, 250) != 0) {
    DEBUG ((DEBUG_ERROR, "Wm5102: FLL1 reports no clock (is MCLK1 running?)\n"));
    return -1;
  }
  return 0;
}

/**
  FLL1, SYSCLK and AIF1 (both directions configured, none enabled), once
  for whichever of playback / capture starts first.
**/
STATIC
INTN
ClocksUp (
  IN OUT WM5102  *Codec
  )
{
  if (Codec->Id != 0x5102) {
    return -1;
  }
  if (Codec->ClocksOn) {
    return 0;
  }
  if (StartFll1 (Codec) < 0) {
    return -1;
  }
  //
  // AIF1: codec is BCLK / LRCLK master (SND_SOC_DAIFMT_CBM_CFM), I2S,
  // 32 BCLKs a frame (2 x 16-bit slots), slots 0 and 1 both ways.
  //
  // Format, rates and word lengths go in while SYSCLK is still off and
  // before the pins are mastered (like the kernel, where DAPM enables
  // SYSCLK after hw_params): once LRCLK runs (LRCLK_FRC) the codec keeps
  // AIF1_FORMAT and FRAME_CTRL_1/2 at their old values.
  //
  Wm5102Write (Codec, R_AIF1_RX_ENABLES, 0);
  Wm5102Write (Codec, R_AIF1_TX_ENABLES, 0);
  Wm5102Write (Codec, R_AIF1_RX_PIN_CTRL, 0);
  Wm5102Write (Codec, R_AIF1_TX_PIN_CTRL, AIF1TX_LRCLK_SRC);
  Wm5102Write (Codec, R_AIF1_RATE_CTRL, 0);          // SAMPLE_RATE_1
  Wm5102Write (Codec, R_AIF1_FORMAT, AIF1_FMT_I2S);
  Wm5102Write (Codec, R_AIF1_TX_BCLK_RATE, 32);
  Wm5102Write (Codec, R_AIF1_RX_BCLK_RATE, 32);
  Wm5102Write (Codec, R_AIF1_FRAME_CTRL_1, (16u << 8) | 16);
  Wm5102Write (Codec, R_AIF1_FRAME_CTRL_2, (16u << 8) | 16);

  Wm5102Write (Codec, R_SAMPLE_RATE_1, SAMPLE_RATE_48K);
  Wm5102Write (Codec, R_SYSTEM_CLOCK_1, SYSCLK_FREQ_24M576 | SYSCLK_ENA | SYSCLK_SRC_FLL1);

  //
  // Clocks out last.  One LRCLK pin for both directions: TX keeps
  // AIF1TX_LRCLK_SRC (its reset value; the kernel only updates the master /
  // invert bits), and the RX side drives it even when only recording
  // (LRCLK_FRC).
  //
  Wm5102Write (Codec, R_AIF1_BCLK_CTRL, AIF1_BCLK_MSTR | AIF1_BCLK_1M536);
  Wm5102Write (Codec, R_AIF1_TX_PIN_CTRL, AIF1TX_LRCLK_SRC | AIF1_LRCLK_MSTR);
  Wm5102Write (Codec, R_AIF1_RX_PIN_CTRL, AIF1_LRCLK_FRC | AIF1_LRCLK_MSTR);
  {
    UINT16  Format;
    UINT16  Frame1;
    UINT16  Frame2;

    Wm5102Read (Codec, R_AIF1_FORMAT, &Format);
    Wm5102Read (Codec, R_AIF1_FRAME_CTRL_1, &Frame1);
    Wm5102Read (Codec, R_AIF1_FRAME_CTRL_2, &Frame2);
    if ((Format != AIF1_FMT_I2S) || (Frame1 != ((16u << 8) | 16)) || (Frame2 != ((16u << 8) | 16))) {
      DEBUG ((DEBUG_ERROR, "Wm5102: AIF1 format 0x%04x frame 0x%04x 0x%04x not taken (want 0x0002 0x1010 0x1010)\n",
              Format, Frame1, Frame2));
    }
  }
  Codec->ClocksOn = TRUE;
  return 0;
}

STATIC
VOID
ClocksDown (
  IN OUT WM5102  *Codec
  )
{
  if (!Codec->ClocksOn || Codec->Playing || Codec->Capturing) {
    return;
  }
  Wm5102Update (Codec, R_SYSTEM_CLOCK_1, SYSCLK_ENA, 0);
  Wm5102Update (Codec, R_FLL1_CONTROL_1, FLL1_ENA, 0);
  Codec->ClocksOn = FALSE;
}

INTN
Wm5102StartPlayback (
  IN OUT WM5102  *Codec,
  IN     UINT8   DacVolume
  )
{
  if (ClocksUp (Codec) < 0) {
    return -1;
  }
  Wm5102Write (Codec, R_AIF1_RX_ENABLES, 0x0003);

  //
  // AIF1RX1 -> OUT1L / OUT4L, AIF1RX2 -> OUT1R / OUT4R at 0 dB, DACs at
  // DacVolume; the selected output on.
  //
  Wm5102Write (Codec, R_OUT1LMIX_INPUT_1_SOURCE, MIX_SRC_AIF1RX1);
  Wm5102Write (Codec, R_OUT1LMIX_INPUT_1_VOLUME, MIX_VOL_0DB);
  Wm5102Write (Codec, R_OUT1RMIX_INPUT_1_SOURCE, MIX_SRC_AIF1RX2);
  Wm5102Write (Codec, R_OUT1RMIX_INPUT_1_VOLUME, MIX_VOL_0DB);
  Wm5102Write (Codec, R_OUT4LMIX_INPUT_1_SOURCE, MIX_SRC_AIF1RX1);
  Wm5102Write (Codec, R_OUT4LMIX_INPUT_1_VOLUME, MIX_VOL_0DB);
  Wm5102Write (Codec, R_OUT4RMIX_INPUT_1_SOURCE, MIX_SRC_AIF1RX2);
  Wm5102Write (Codec, R_OUT4RMIX_INPUT_1_VOLUME, MIX_VOL_0DB);
  Wm5102SetVolume (Codec, DacVolume, FALSE);
  Wm5102Update (Codec, R_OUTPUT_ENABLES_1, OUT1_ENA | OUT4L_ENA | OUT4R_ENA,
                Codec->Headphones ? OUT1_ENA : (OUT4L_ENA | OUT4R_ENA));
  EnvSleepMs (20);                                  // arizona_out_ev() settle time
  Codec->Playing = TRUE;
  return 0;
}

INTN
Wm5102SetVolume (
  IN WM5102   *Codec,
  IN UINT8    DacVolume,
  IN BOOLEAN  Mute
  )
{
  UINT16  Value;

  Value = (UINT16)(DacVolume | (Mute ? OUT_MUTE : 0));
  Wm5102Write (Codec, R_DAC_DIGITAL_VOLUME_1L, Value);
  Wm5102Write (Codec, R_DAC_DIGITAL_VOLUME_1R, (UINT16)(Value | OUT_VU));
  Wm5102Write (Codec, R_DAC_DIGITAL_VOLUME_4L, Value);
  return Wm5102Write (Codec, R_DAC_DIGITAL_VOLUME_4R, (UINT16)(Value | OUT_VU));
}

INTN
Wm5102SetVolumeLR (
  IN WM5102   *Codec,
  IN UINT8    Left,
  IN UINT8    Right,
  IN BOOLEAN  Mute
  )
{
  UINT16  MuteBit;

  MuteBit = Mute ? OUT_MUTE : 0;
  Wm5102Write (Codec, R_DAC_DIGITAL_VOLUME_1L, (UINT16)(Left | MuteBit));
  Wm5102Write (Codec, R_DAC_DIGITAL_VOLUME_1R, (UINT16)(Right | MuteBit | OUT_VU));
  Wm5102Write (Codec, R_DAC_DIGITAL_VOLUME_4L, (UINT16)(Left | MuteBit));
  return Wm5102Write (Codec, R_DAC_DIGITAL_VOLUME_4R, (UINT16)(Right | MuteBit | OUT_VU));
}

INTN
Wm5102JackInit (
  IN WM5102  *Codec
  )
{
  Wm5102Write (Codec, R_GPIO5_CTRL, GPIO5_INPUT_NOPULL);
  Wm5102Update (Codec, R_MICD_CLAMP_CONTROL, 0x000F, 9);
  Wm5102Update (Codec, R_JACK_DETECT_DEBOUNCE, MICD_CLAMP_DB | JD1_DB, MICD_CLAMP_DB | JD1_DB);
  return Wm5102Update (Codec, R_JACK_DETECT_ANALOGUE, JD1_ENA, JD1_ENA);
}

INTN
Wm5102IrqInit (
  IN WM5102  *Codec
  )
{
  UINT32  Index;

  Wm5102Write (Codec, R_WAKE_CONTROL, 0);
  Wm5102Update (Codec, R_IRQ_CTRL_1, IRQ_POL, 0);
  for (Index = 0; Index < 5; Index++) {
    Wm5102Write (Codec, R_INTERRUPT_STATUS_1_MASK + Index, 0xFFFF);
    Wm5102Write (Codec, R_INTERRUPT_STATUS_1 + Index, 0xFFFF);
  }
  Wm5102Write (Codec, R_AOD_IRQ1, 0xFFFF);
  Wm5102Write (Codec, R_AOD_IRQ_MASK_IRQ1, (UINT16)~(MICD_CLAMP_FALL_EINT1 | MICD_CLAMP_RISE_EINT1));
  return Wm5102Update (Codec, R_INTERRUPT_CONTROL, IM_IRQ1, 0);
}

UINT16
Wm5102IrqAcknowledge (
  IN WM5102  *Codec
  )
{
  UINT16  Aod;
  UINT16  Main;
  UINT32  Index;

  Aod = 0;
  if ((Wm5102Read (Codec, R_AOD_IRQ1, &Aod) == 0) && (Aod != 0)) {
    Wm5102Write (Codec, R_AOD_IRQ1, Aod);
  }

  //
  // The main block is masked; clear it anyway so a stray status cannot
  // hold the level-triggered pin up.
  //
  for (Index = 0; Index < 5; Index++) {
    if ((Wm5102Read (Codec, R_INTERRUPT_STATUS_1 + Index, &Main) == 0) && (Main != 0)) {
      Wm5102Write (Codec, R_INTERRUPT_STATUS_1 + Index, Main);
    }
  }
  return Aod;
}

BOOLEAN
Wm5102JackPresent (
  IN WM5102  *Codec
  )
{
  UINT16  Raw;

  if (Wm5102Read (Codec, R_AOD_IRQ_RAW_STATUS, &Raw) < 0) {
    return FALSE;
  }
  return (Raw & MICD_CLAMP_STS) == 0;
}

INTN
Wm5102SelectOutput (
  IN OUT WM5102   *Codec,
  IN     BOOLEAN  Headphones
  )
{
  Codec->Headphones = Headphones;
  if (!Codec->Playing) {
    return 0;
  }
  Wm5102Update (Codec, R_OUTPUT_ENABLES_1, OUT1_ENA | OUT4L_ENA | OUT4R_ENA,
                Headphones ? OUT1_ENA : (OUT4L_ENA | OUT4R_ENA));
  EnvSleepMs (20);
  return 0;
}

INTN
Wm5102StopPlayback (
  IN OUT WM5102  *Codec
  )
{
  Wm5102Update (Codec, R_OUTPUT_ENABLES_1, OUT1_ENA | OUT4L_ENA | OUT4R_ENA, 0);
  Wm5102Write (Codec, R_AIF1_RX_ENABLES, 0);
  Codec->Playing = FALSE;
  ClocksDown (Codec);
  return 0;
}

INTN
Wm5102StartCapture (
  IN OUT WM5102   *Codec,
  IN     BOOLEAN  HeadsetMic,
  IN     UINT8    PgaGainDb
  )
{
  UINT32  Control;
  UINT32  Digital;
  UINT16  Enable;
  UINT16  Source;

  if (ClocksUp (Codec) < 0) {
    return -1;
  }
  Codec->HeadsetMic = HeadsetMic;
  Control = HeadsetMic ? R_IN1R_CONTROL : R_IN1L_CONTROL;
  Digital = HeadsetMic ? R_ADC_DIGITAL_VOLUME_1R : R_ADC_DIGITAL_VOLUME_1L;
  Enable  = HeadsetMic ? IN1R_ENA : IN1L_ENA;
  Source  = HeadsetMic ? MIX_SRC_IN1R : MIX_SRC_IN1L;

  //
  // MICVDD (the mic charge pump, regulated: arizona-micsupp), then the
  // headset mic's MICBIAS1 (pdata micbias[0]: 2.6 V, discharge).
  //
  Wm5102Update (Codec, R_MIC_CHARGE_PUMP_1, CPMIC_ENA | CPMIC_BYPASS, CPMIC_ENA);
  EnvSleepMs (3);
  if (HeadsetMic) {
    Wm5102Write (Codec, R_MIC_BIAS_CTRL_1, MICB1_LVL_2V6 | MICB1_DISCH | MICB1_ENA);
  }

  //
  // Input PGA gain, input on, then unmuted at 0 dB digital (arizona_in_ev).
  //
  Wm5102Update (Codec, Control, 0x00FE, (UINT16)((IN_PGA_VOL_0DB + MIN (PgaGainDb, 31)) << 1));
  Wm5102Write (Codec, Digital, IN_MUTE | IN_VU | 0x80);
  Wm5102Update (Codec, R_INPUT_ENABLES, IN1L_ENA | IN1R_ENA, Enable);
  EnvSleepMs (2);
  Wm5102Write (Codec, Digital, IN_VU | 0x80);

  //
  // The mic into both AIF1 TX slots.
  //
  Wm5102Write (Codec, R_AIF1TX1MIX_INPUT_1_SOURCE, Source);
  Wm5102Write (Codec, R_AIF1TX1MIX_INPUT_1_VOLUME, MIX_VOL_0DB);
  Wm5102Write (Codec, R_AIF1TX2MIX_INPUT_1_SOURCE, Source);
  Wm5102Write (Codec, R_AIF1TX2MIX_INPUT_1_VOLUME, MIX_VOL_0DB);
  Wm5102Write (Codec, R_AIF1_TX_ENABLES, 0x0003);
  Codec->Capturing = TRUE;
  return 0;
}

INTN
Wm5102StopCapture (
  IN OUT WM5102  *Codec
  )
{
  Wm5102Write (Codec, R_AIF1_TX_ENABLES, 0);
  Wm5102Update (Codec, R_INPUT_ENABLES, IN1L_ENA | IN1R_ENA, 0);
  Wm5102Update (Codec, R_MIC_BIAS_CTRL_1, MICB1_ENA, 0);
  Wm5102Update (Codec, R_MIC_CHARGE_PUMP_1, CPMIC_ENA, 0);
  Codec->Capturing = FALSE;
  ClocksDown (Codec);
  return 0;
}

VOID
Wm5102LogStatus (
  IN WM5102  *Codec
  )
{
  UINT16  Raw5;
  UINT16  SysClk;
  UINT16  OutEna;
  UINT16  OutSts;
  UINT16  RawOut;
  UINT16  Fll;

  Wm5102Read (Codec, R_INTERRUPT_RAW_STATUS_5, &Raw5);
  Wm5102Read (Codec, R_SYSTEM_CLOCK_1, &SysClk);
  Wm5102Read (Codec, R_FLL1_CONTROL_1, &Fll);
  Wm5102Read (Codec, R_OUTPUT_ENABLES_1, &OutEna);
  Wm5102Read (Codec, R_OUTPUT_STATUS_1, &OutSts);
  Wm5102Read (Codec, R_RAW_OUTPUT_STATUS_1, &RawOut);
  DEBUG ((DEBUG_INFO, "Wm5102: RAW_STATUS_5 0x%04x FLL1 0x%04x SYSCLK 0x%04x OUT_ENA 0x%04x OUT_STS 0x%04x RAW_OUT 0x%04x\n",
          Raw5, Fll, SysClk, OutEna, OutSts, RawOut));
  Wm5102Read (Codec, R_AOD_IRQ_RAW_STATUS, &Raw5);
  DEBUG ((DEBUG_INFO, "Wm5102: AOD_IRQ_RAW 0x%04x (jack %a)\n", Raw5, ((Raw5 & MICD_CLAMP_STS) == 0) ? "in" : "out"));
  Wm5102Read (Codec, R_AOD_IRQ_RAW_STATUS, &Raw5);
  DEBUG ((DEBUG_INFO, "Wm5102: AOD_IRQ_RAW 0x%04x (jack %a)\n", Raw5, ((Raw5 & MICD_CLAMP_STS) == 0) ? "in" : "out"));
  {
    UINT16  Aif[9];
    UINT16  RxEna;
    UINT32  Index;

    for (Index = 0; Index < 9; Index++) {
      Wm5102Read (Codec, R_AIF1_BCLK_CTRL + Index, &Aif[Index]);
    }
    Wm5102Read (Codec, R_AIF1_RX_ENABLES, &RxEna);
    DEBUG ((DEBUG_INFO, "Wm5102: AIF1 0x500.. %04x %04x %04x %04x %04x %04x %04x %04x %04x RX_ENA %04x\n", Aif[0], Aif[1],
            Aif[2], Aif[3], Aif[4], Aif[5], Aif[6], Aif[7], Aif[8], RxEna));
  }
}
