/** @file
  AudioTest.efi: speaker bring-up for v1awifi.  Powers the WM5102 (LDOENA
  GPG0(1), /RESET GPJ4(0), MCLK1 = 24 MHz on CLKOUT), talks to it over SPI2
  (EXYNOS_SPI), and plays a 1 kHz tone through I2S0 + IDMA from DRAM:
  both speakers, then left only, then right only.  Logs the codec ID, the
  clock state and the IDMA position (it must advance 192000 bytes/s).

    AudioTest.efi [seconds-per-part [volume]]     defaults: 2, 0x70 (-8 dB)
    AudioTest.efi mic [headset]                   record 1.3 s from the main
                                                  (or headset) mic, log the
                                                  levels, play it back

  The shared cores (SpiDxe/SpiCore.c via the protocol, Audio/Wm5102Core.c,
  Audio/ExynosI2sCore.c) are what the Windows drivers build too.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>

#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosSpi.h>

#include "../../Drivers/Audio/Wm5102Core.h"
#include "../../Drivers/Audio/ExynosI2sCore.h"

#define CODEC_LDOENA     EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPG0, 1)
#define CODEC_RESET      EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPJ4, 0)
#define MAINMIC_BIAS     EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPY7, 6)   // GPIO_MICBIAS_EN
#define CODEC_SPI_BUS    2

//
// 48 kHz, 16-bit stereo: one 1 kHz cycle is 48 frames = 192 bytes; the
// buffer holds whole cycles so the loop is seamless.
//
#define CYCLE_FRAMES     48
#define BUFFER_CYCLES    340
#define BUFFER_BYTES     (CYCLE_FRAMES * 4 * BUFFER_CYCLES)      // 65280
#define PERIOD_BYTES     (BUFFER_BYTES / 4)

//
// Mic test: 1.3 s (IDMA plays at most 256 KiB back).
//
#define RECORD_FRAMES    62400
#define RECORD_BYTES     (RECORD_FRAMES * 4)                    // 249600
#define ALLOC_BYTES      RECORD_BYTES

//
// sin (2 pi i / 48) in Q15.
//
STATIC CONST INT16  mSine[CYCLE_FRAMES] = {
       0,   4277,   8481,  12539,  16384,  19947,  23170,  25997,
   28378,  30274,  31651,  32488,  32767,  32488,  31651,  30274,
   28378,  25997,  23170,  19947,  16384,  12539,   8481,   4277,
       0,  -4277,  -8481, -12539, -16384, -19947, -23170, -25997,
  -28378, -30274, -31651, -32488, -32767, -32488, -31651, -30274,
  -28378, -25997, -23170, -19947, -16384, -12539,  -8481,  -4277,
};

STATIC EXYNOS_SPI_PROTOCOL  *mSpi;

STATIC
INTN
CodecXfer (
  IN  VOID         *Context,
  IN  CONST UINT8  *Tx,
  IN  UINTN        TxLen,
  OUT UINT8        *Rx,
  IN  UINTN        RxLen
  )
{
  return EFI_ERROR (mSpi->Transfer (mSpi, CODEC_SPI_BUS, Tx, TxLen, Rx, RxLen)) ? -1 : 0;
}

/**
  The buffer as a tone at a quarter of full scale on the chosen channels.
**/
STATIC
VOID
FillTone (
  OUT INT16    *Buffer,
  IN  BOOLEAN  Left,
  IN  BOOLEAN  Right
  )
{
  UINTN  Frame;
  INT16  Sample;

  for (Frame = 0; Frame < BUFFER_BYTES / 4; Frame++) {
    Sample                = (INT16)(mSine[Frame % CYCLE_FRAMES] / 4);
    Buffer[2 * Frame]     = Left ? Sample : 0;
    Buffer[2 * Frame + 1] = Right ? Sample : 0;
  }
  WriteBackDataCacheRange (Buffer, BUFFER_BYTES);
}

/**
  Play for Seconds, logging the IDMA position four times a second.
**/
STATIC
UINT32
Watch (
  IN EXYNOS_I2S  *I2s,
  IN UINTN       Seconds
  )
{
  UINTN   Tick;
  UINT32  Position;
  UINT32  Last;
  UINT32  Moved;

  Last  = ExynosI2sPosition (I2s);
  Moved = 0;
  for (Tick = 0; Tick < Seconds * 4; Tick++) {
    gBS->Stall (250 * 1000);
    Position = ExynosI2sPosition (I2s);
    Moved   += (Position + BUFFER_BYTES - Last) % BUFFER_BYTES;
    Last     = Position;
  }
  DEBUG ((DEBUG_INFO, "AudioTest: IDMA moved ~%u bytes in %u s (expect ~%u; wraps undercount)\n", Moved,
          (UINT32)Seconds, (UINT32)(192000 * Seconds)));
  return Moved;
}

//
// Fallback: the audio block's internal SRAM, where the stock kernel's IDMA
// reads for buffers up to 32 KiB (exynos5 srp pdata idma = INTMEM + 4).
//
#define SRAM_BUFFER      0x03000004
#define SRAM_BYTES       (CYCLE_FRAMES * 4 * 170)               // 32640

STATIC
UINT32
PlayFromSram (
  IN EXYNOS_I2S  *I2s,
  IN UINTN       Seconds
  )
{
  UINTN   Frame;
  UINT32  Sample;

  for (Frame = 0; Frame < SRAM_BYTES / 4; Frame++) {
    Sample = (UINT16)(mSine[Frame % CYCLE_FRAMES] / 4);
    MmioWrite32 (SRAM_BUFFER + Frame * 4, Sample | (Sample << 16));
  }
  ExynosI2sStop (I2s);
  ExynosI2sConfigure (I2s);
  ExynosI2sStart (I2s, SRAM_BUFFER, SRAM_BYTES, SRAM_BYTES / 4, FALSE);
  ExynosI2sLogStatus (I2s);
  Print (L"AudioTest: DRAM did not move, 1 kHz from audio SRAM\n");
  DEBUG ((DEBUG_INFO, "AudioTest: retry from audio SRAM 0x%08x\n", SRAM_BUFFER));
  return Watch (I2s, Seconds);
}

/**
  Do the codec's clocks reach I2S0?  Count level changes on GPZ0 (SCLK)
  and GPZ2 (LRCK) over many reads of the GPZ data register.
**/
STATIC
VOID
SamplePins (
  IN EXYNOS_I2S  *I2s
  )
{
  UINT32  Last;
  UINT32  Now;
  UINT32  Sclk;
  UINT32  Lrck;
  UINT32  Sdi;
  UINT32  Ones;
  UINTN   Index;

  Sclk = 0;
  Lrck = 0;
  Sdi  = 0;
  Ones = 0;
  Last = MmioRead32 (I2s->Gpz + 0x4);
  for (Index = 0; Index < 200000; Index++) {
    Now   = MmioRead32 (I2s->Gpz + 0x4);
    Sclk += ((Now ^ Last) >> 0) & 1;
    Lrck += ((Now ^ Last) >> 2) & 1;
    Sdi  += ((Now ^ Last) >> 3) & 1;
    Ones += Now & 1;
    Last  = Now;
  }
  DEBUG ((DEBUG_INFO, "AudioTest: GPZ CON 0x%08x DAT 0x%02x; over 200000 reads SCLK changed %u times (high %u), LRCK %u, SDI %u\n",
          MmioRead32 (I2s->Gpz), Last, Sclk, Ones, Lrck, Sdi));
}

STATIC
BOOLEAN
ArgIs (
  IN CONST CHAR16  *Line,
  IN UINTN         N,
  IN CONST CHAR16  *Word
  )
{
  UINTN  Index;
  UINTN  Arg;
  UINTN  Length;

  if (Line == NULL) {
    return FALSE;
  }
  Length = StrLen (Word);
  for (Index = 0, Arg = 0; Line[Index] != L'\0'; Arg++) {
    while (Line[Index] == L' ') {
      Index++;
    }
    if (Line[Index] == L'\0') {
      break;
    }
    if ((Arg == N + 1) && (StrnCmp (Line + Index, Word, Length) == 0) &&
        ((Line[Index + Length] == L' ') || (Line[Index + Length] == L'\0')))
    {
      return TRUE;
    }
    while ((Line[Index] != L'\0') && (Line[Index] != L' ')) {
      Index++;
    }
  }
  return FALSE;
}

/**
  Record through the I2S0 RX FIFO (PIO), log quarter-second levels per
  channel, then play the recording back.
**/
STATIC
EFI_STATUS
MicTest (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio,
  IN EXYNOS_I2S            *I2s,
  IN WM5102                *Codec,
  IN UINT32                *Buffer,
  IN BOOLEAN               HeadsetMic,
  IN UINT8                 Volume
  )
{
  UINT32  Discard[256];
  UINT32  Got;
  UINT32  Part;
  UINT32  Frame;
  UINT32  Peak[2];
  UINT32  Sum[2];
  INT32   Sample;
  UINTN   Ch;

  if (!HeadsetMic) {
    Gpio->Set (Gpio, MAINMIC_BIAS, 1);
    Gpio->Config (Gpio, MAINMIC_BIAS, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  }
  ExynosI2sConfigure (I2s);
  if (Wm5102StartCapture (Codec, HeadsetMic, 30) != 0) {
    return EFI_DEVICE_ERROR;
  }
  ExynosI2sRxStart (I2s, FALSE);
  Wm5102LogStatus (Codec);
  ExynosI2sLogStatus (I2s);
  SamplePins (I2s);
  for (Part = 0; Part < 20; Part++) {               // ~0.1 s settling, dropped
    ExynosI2sRxRead (I2s, Discard, ARRAY_SIZE (Discard), 2000);
  }

  Print (L"AudioTest: recording 1.3 s from the %s mic - speak now\n", HeadsetMic ? L"headset" : L"main");
  Got = ExynosI2sRxRead (I2s, Buffer, RECORD_FRAMES, 2000);
  ExynosI2sLogStatus (I2s);
  ExynosI2sRxStop (I2s);
  Wm5102StopCapture (Codec);
  if (!HeadsetMic) {
    Gpio->Set (Gpio, MAINMIC_BIAS, 0);
  }
  DEBUG ((DEBUG_INFO, "AudioTest: %a mic, read %u of %u frames\n", HeadsetMic ? "headset" : "main", Got, RECORD_FRAMES));

  for (Part = 0; Part * 12000 < Got; Part++) {
    Peak[0] = Peak[1] = 0;
    Sum[0]  = Sum[1]  = 0;
    for (Frame = Part * 12000; (Frame < (Part + 1) * 12000) && (Frame < Got); Frame++) {
      for (Ch = 0; Ch < 2; Ch++) {
        Sample = (INT16)(Buffer[Frame] >> (16 * Ch));
        Sample = (Sample < 0) ? -Sample : Sample;
        Peak[Ch] = MAX (Peak[Ch], (UINT32)Sample);
        Sum[Ch] += (UINT32)Sample;
      }
    }
    DEBUG ((DEBUG_INFO, "AudioTest: %u.%02u s: left peak %5u avg %5u, right peak %5u avg %5u (full scale 32767)\n",
            Part / 4, (Part % 4) * 25, Peak[0], Sum[0] / 12000, Peak[1], Sum[1] / 12000));
  }
  if (Got < RECORD_FRAMES) {
    return EFI_TIMEOUT;                             // no frames: no clocks / RX path
  }

  //
  // Play it back.
  //
  WriteBackDataCacheRange (Buffer, RECORD_BYTES);
  if (Wm5102StartPlayback (Codec, Volume) != 0) {
    return EFI_DEVICE_ERROR;
  }
  ExynosI2sStart (I2s, (UINT32)(UINTN)Buffer, RECORD_BYTES, RECORD_BYTES / 4, FALSE);
  Print (L"AudioTest: playing the recording back\n");
  gBS->Stall (1300 * 1000);
  ExynosI2sStop (I2s);
  Wm5102StopPlayback (Codec);
  return EFI_SUCCESS;
}

STATIC
UINTN
ArgNumber (
  IN CONST CHAR16  *Line,
  IN UINTN         N,
  IN UINTN         Default
  )
{
  UINTN  Index;
  UINTN  Arg;
  UINTN  Start;

  if (Line == NULL) {
    return Default;
  }
  for (Index = 0, Arg = 0; Line[Index] != L'\0'; Arg++) {
    while (Line[Index] == L' ') {
      Index++;
    }
    if (Line[Index] == L'\0') {
      break;
    }
    Start = Index;
    while ((Line[Index] != L'\0') && (Line[Index] != L' ')) {
      Index++;
    }
    if (Arg == N + 1) {
      return ((Line[Start] == L'0') && ((Line[Start + 1] == L'x') || (Line[Start + 1] == L'X')))
             ? StrHexToUintn (Line + Start) : StrDecimalToUintn (Line + Start);
    }
  }
  return Default;
}

EFI_STATUS
EFIAPI
AudioTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EXYNOS_GPIO_PROTOCOL       *Gpio;
  EFI_LOADED_IMAGE_PROTOCOL  *Image;
  EXYNOS_I2S                 I2s;
  WM5102                     Codec;
  INT16                      *Buffer;
  UINTN                      Seconds;
  UINT8                      Volume;
  EFI_STATUS                 Status;

  Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&Gpio);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gExynosSpiProtocolGuid, NULL, (VOID **)&mSpi);
  }
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  }
  if (EFI_ERROR (Status)) {
    Print (L"AudioTest: GPIO / SPI protocol missing - %r\n", Status);
    return Status;
  }
  Seconds = ArgNumber (Image->LoadOptions, 0, 2);
  Volume  = (UINT8)ArgNumber (Image->LoadOptions, 1, 0x70);

  Buffer = AllocatePages (EFI_SIZE_TO_PAGES (ALLOC_BYTES));
  if ((Buffer == NULL) || ((UINTN)Buffer > 0xFFFFFFFFu - ALLOC_BYTES)) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Codec in reset, LDO (DCVDD) on, audio block + MCLK1 up, out of reset.
  //
  ZeroMem (&I2s, sizeof (I2s));
  I2s.I2s   = EXYNOS_I2S0_PHYS;
  I2s.Audss = EXYNOS_AUDSS_PHYS;
  I2s.Gpz   = EXYNOS_GPZ_PHYS;
  I2s.Pmu   = EXYNOS_PMU_PHYS;
  Gpio->Set (Gpio, CODEC_RESET, 0);
  Gpio->Config (Gpio, CODEC_RESET, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  Gpio->Set (Gpio, CODEC_LDOENA, 1);
  Gpio->Config (Gpio, CODEC_LDOENA, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  if (ExynosI2sPowerUp (&I2s) != 0) {
    Status = EFI_DEVICE_ERROR;
    goto Off;
  }
  gBS->Stall (10 * 1000);
  Gpio->Set (Gpio, CODEC_RESET, 1);
  gBS->Stall (5 * 1000);

  ZeroMem (&Codec, sizeof (Codec));
  Codec.Xfer = CodecXfer;
  if (Wm5102Init (&Codec) != 0) {
    Print (L"AudioTest: WM5102 not found or did not boot (see log)\n");
    Status = EFI_NOT_FOUND;
    goto Off;
  }
  Wm5102JackInit (&Codec);
  gBS->Stall (100 * 1000);                          // clamp detection debounce
  Codec.Headphones = Wm5102JackPresent (&Codec);
  Print (L"AudioTest: playing through the %s\n", Codec.Headphones ? L"headphones" : L"speakers");
  Wm5102LogStatus (&Codec);

  if (ArgIs (Image->LoadOptions, 0, L"mic")) {
    Status = MicTest (Gpio, &I2s, &Codec, (UINT32 *)Buffer, ArgIs (Image->LoadOptions, 1, L"headset"), 0x70);
    goto Off;
  }

  FillTone (Buffer, TRUE, TRUE);
  ExynosI2sConfigure (&I2s);
  if (Wm5102StartPlayback (&Codec, Volume) != 0) {
    Wm5102LogStatus (&Codec);
    Status = EFI_DEVICE_ERROR;
    goto Off;
  }
  ExynosI2sStart (&I2s, (UINT32)(UINTN)Buffer, BUFFER_BYTES, PERIOD_BYTES, FALSE);
  Wm5102LogStatus (&Codec);
  SamplePins (&I2s);
  ExynosI2sLogStatus (&I2s);
  Print (L"AudioTest: 1 kHz on both speakers, volume 0x%02x\n", Volume);
  if (Watch (&I2s, Seconds) == 0) {
    ExynosI2sLogStatus (&I2s);
    PlayFromSram (&I2s, Seconds);
    ExynosI2sLogStatus (&I2s);
    goto Done;
  }
  ExynosI2sLogStatus (&I2s);

  FillTone (Buffer, TRUE, FALSE);
  Print (L"AudioTest: left only\n");
  Watch (&I2s, Seconds);
  FillTone (Buffer, FALSE, TRUE);
  Print (L"AudioTest: right only\n");
  Watch (&I2s, Seconds);

Done:
  Wm5102LogStatus (&Codec);
  Wm5102StopPlayback (&Codec);
  ExynosI2sStop (&I2s);
  Status = EFI_SUCCESS;

Off:
  Gpio->Set (Gpio, CODEC_RESET, 0);
  Gpio->Set (Gpio, CODEC_LDOENA, 0);
  FreePages (Buffer, EFI_SIZE_TO_PAGES (ALLOC_BYTES));
  DEBUG ((DEBUG_INFO, "AudioTest: finished - %r; codec in reset\n", Status));
  Print (L"AudioTest: %r (details in the debug log)\n", Status);
  return Status;
}
