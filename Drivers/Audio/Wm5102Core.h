/** @file
  Wolfson WM5102 (Arizona) codec over SPI, shared by UEFI (AudioTestApp)
  and Windows (ExynosWinDrivers/exynosaudio), built unchanged in both
  through <ExynosEnv.h>.

  Bus-agnostic: every register access is one chip-select cycle through the
  caller's Xfer callback (UEFI: EXYNOS_SPI protocol; Windows: an SPB
  write-then-read sequence on the codec's SpiSerialBus connection).  The
  board (LDOENA, /RESET, MCLK1) is the caller's: release reset with the LDO
  on and MCLK1 running, then Wm5102Init().

  Playback path (fixed, as the stock machine driver adonisuniv_wm5102.c sets
  it up for the speakers): MCLK1 24 MHz -> FLL1 -> SYSCLK 24.576 MHz
  (48 kHz x 512); AIF1 clock master (BCLK 32 fs, I2S, 16-bit) receiving
  from Exynos I2S0; AIF1RX1/2 -> OUT4L/R mixers -> SPKOUTL/R.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __WM5102_CORE_H__
#define __WM5102_CORE_H__

#include <ExynosEnv.h>

/**
  One chip-select cycle: TxLen bytes out, then RxLen bytes in.  0 or < 0.
**/
typedef
INTN
(*WM5102_XFER) (
  IN  VOID         *Context,
  IN  CONST UINT8  *Tx,
  IN  UINTN        TxLen,
  OUT UINT8        *Rx,
  IN  UINTN        RxLen
  );

typedef struct {
  WM5102_XFER  Xfer;
  VOID         *Context;
  UINT16       Id;         // 0x5102 once Wm5102Init() found the chip
  UINT16       Rev;
  BOOLEAN      Playing;
  BOOLEAN      Capturing;
  BOOLEAN      ClocksOn;      // FLL1 / SYSCLK / AIF1 set up (shared by both directions)
  BOOLEAN      Headphones;    // output in use: HPOUT1 (TRUE) or the speakers
  BOOLEAN      HeadsetMic;    // input in use: IN1R (headset, MICBIAS1) or IN1L (main mic)
} WM5102;

INTN
Wm5102Write (
  IN WM5102  *Codec,
  IN UINT32  Reg,
  IN UINT16  Value
  );

INTN
Wm5102Read (
  IN  WM5102  *Codec,
  IN  UINT32  Reg,
  OUT UINT16  *Value
  );

INTN
Wm5102Update (
  IN WM5102  *Codec,
  IN UINT32  Reg,
  IN UINT16  Mask,
  IN UINT16  Value
  );

/**
  Identify (0x5102), wait for boot, apply Samsung's register patch and the
  write-sequencer hardware patch, 32 kHz clock from MCLK2 (arizona_dev_init()).
  0 or < 0.
**/
INTN
Wm5102Init (
  IN OUT WM5102  *Codec
  );

/**
  Clocks (FLL1 from MCLK1 = 24 MHz), AIF1 as master for 48 kHz 16-bit I2S
  stereo, AIF1RX1/2 to the speakers at DacVolume (0x80 = 0 dB, 0.5 dB
  steps).  0 or < 0 (FLL did not report its clock).
**/
INTN
Wm5102StartPlayback (
  IN OUT WM5102  *Codec,
  IN     UINT8   DacVolume
  );

/**
  Speaker DAC volume while playing (0x80 = 0 dB, 0.5 dB steps), or mute.
**/
INTN
Wm5102SetVolume (
  IN WM5102   *Codec,
  IN UINT8    DacVolume,
  IN BOOLEAN  Mute
  );

/**
  Headphone jack detection as the stock board runs it (jd_gpio5 +
  jd_gpio5_nopull, rev B "mic clamp" detection, switch-arizona.c): codec
  GPIO5 as input, clamp mode 9, debounced.  Needs the 32 kHz clock
  (Wm5102Init() turns it on).
**/
INTN
Wm5102JackInit (
  IN WM5102  *Codec
  );

/**
  TRUE when a plug is in the headphone jack (MICD_CLAMP_STS clear).
**/
BOOLEAN
Wm5102JackPresent (
  IN WM5102  *Codec
  );

/**
  The codec's IRQ pin (GPX2(5) on this board) active high, asserted by the
  jack detection (mic clamp rise / fall in the always-on block) only; every
  other source masked (arizona_irq_init(), wm5102_aod).
**/
INTN
Wm5102IrqInit (
  IN WM5102  *Codec
  );

/**
  Interrupt half (PASSIVE_LEVEL, it talks SPI): acknowledge what is pending.
  Returns the AOD_IRQ1 bits that were set (bit 6 / 7: jack in / out edges),
  0 if the codec had nothing pending.
**/
UINT16
Wm5102IrqAcknowledge (
  IN WM5102  *Codec
  );

/**
  Play through the headphones (HPOUT1) or the speakers (SPKOUT); switches
  over at once while playing, otherwise applies at the next start.
**/
INTN
Wm5102SelectOutput (
  IN OUT WM5102   *Codec,
  IN     BOOLEAN  Headphones
  );

/**
  Speaker DAC volumes per channel (0x80 = 0 dB, 0.5 dB steps, 0 = -64 dB)
  and mute, updated together (OUT_VU on the second write).
**/
INTN
Wm5102SetVolumeLR (
  IN WM5102   *Codec,
  IN UINT8    Left,
  IN UINT8    Right,
  IN BOOLEAN  Mute
  );

/**
  Recording into AIF1 TX (slots 0 / 1 = left / right, both the same mono
  mic) as the stock tinyucm.conf routes it: the main mic on IN1L (powered
  from MICVDD through the board's external bias, MAINMIC_BIAS = GPY7(6):
  the caller drives that pin) or the headset mic on IN1R (MICBIAS1, 2.6 V);
  analogue gain PgaGainDb (0..31; the stock value is 30), digital 0 dB.
**/
INTN
Wm5102StartCapture (
  IN OUT WM5102   *Codec,
  IN     BOOLEAN  HeadsetMic,
  IN     UINT8    PgaGainDb
  );

INTN
Wm5102StopCapture (
  IN OUT WM5102  *Codec
  );

INTN
Wm5102StopPlayback (
  IN OUT WM5102  *Codec
  );

/**
  Clock / output state to the log (bring-up).
**/
VOID
Wm5102LogStatus (
  IN WM5102  *Codec
  );

#endif // __WM5102_CORE_H__
