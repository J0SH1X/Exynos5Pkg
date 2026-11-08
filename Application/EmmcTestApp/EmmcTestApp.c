/** @file
  EmmcTest.efi - eMMC bring-up test for interrupt-driven completion and the
  faster bus modes.  Everything is logged with DEBUG (USB log, "EmmcTest:").

    EmmcTest.efi                 full sequence
    EmmcTest.efi hs400 irq ...   words select parts: clock irq hs hs400 scan write stress
    EmmcTest.efi keep            leave the best working mode on at the end

  The write tests are non-destructive: a 16 MiB region 4 MiB before the end
  of the disk is saved in RAM, overwritten with a pattern, verified (in
  another mode too) and restored (hash-checked).  A power cut in the middle
  would leave the pattern there, so the region sits in free space at the end
  of the last partition.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ExynosMmcTest.h>

#define LOG(...)  DEBUG ((DEBUG_INFO, __VA_ARGS__))

#define BLK            512
#define CHUNK_BLOCKS   1024
#define BENCH_BLOCKS   16384            // 8 MiB per throughput run
#define REGION_BLOCKS  32768            // 16 MiB write region
#define SCAN_BLOCKS    256              // 128 KiB per delay-scan point
#define REF_LBA        262144           // 128 MiB into the disk

STATIC EXYNOS_MMC_TEST_PROTOCOL  *mT;
STATIC UINT64                    mBlocks;
STATIC UINTN                     mFails;
STATIC UINT8                     *mBuf;
STATIC UINT8                     *mSave;
STATIC UINT8                     *mOrig;
STATIC UINT64                    mRegion;
STATIC UINT32                    mRefHash;
STATIC UINT32                    mScanRef;
STATIC BOOLEAN                   mDdrWorks;

STATIC
CONST CHAR8 *
SpeedName (
  IN UINT32  S
  )
{
  return (S == 0) ? "default" : (S == 1) ? "hs52" : (S == 3) ? "ddr52" : (S == 4) ? "hs200" : "hs400";
}

STATIC
UINT64
NowUs (
  VOID
  )
{
  return GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000;
}

STATIC
UINT32
Hash (
  IN CONST UINT8  *P,
  IN UINTN        N,
  IN UINT32       H
  )
{
  UINTN  I;

  for (I = 0; I < N; I++) {
    H = (H ^ P[I]) * 16777619U;
  }
  return H;
}

STATIC
VOID
Fail (
  IN CONST CHAR8  *What,
  IN EFI_STATUS   Status
  )
{
  mFails++;
  LOG ("EmmcTest: FAIL %a - %r\n", What, Status);
}

STATIC
VOID
Info (
  IN CONST CHAR8  *Tag
  )
{
  EXYNOS_MMC_INFO  I;

  mT->GetInfo (mT, &I);
  LOG ("EmmcTest: [%a] speed %a, %u-bit, card clock %u Hz (CIU %u), irq mode %u taken %u missed %u, dqs %u\n",
       Tag, SpeedName (I.Speed), I.BusWidth, I.ClockHz, I.CiuHz, I.IrqMode, I.IrqCount, I.IrqMissed, I.DqsDelay);
}

/**
  Read Blocks from Lba in chunks: hash of the data, elapsed microseconds.
**/
STATIC
EFI_STATUS
ReadHash (
  IN  UINT64  Lba,
  IN  UINTN   Blocks,
  OUT UINT32  *H,
  OUT UINT64  *Us
  )
{
  UINTN       Done;
  UINTN       N;
  EFI_STATUS  Status;
  UINT64      T0;

  *H = 2166136261U;
  T0 = NowUs ();
  for (Done = 0; Done < Blocks; Done += N) {
    N      = MIN ((UINTN)CHUNK_BLOCKS, Blocks - Done);
    Status = mT->Read (mT, Lba + Done, N, mBuf);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    *H = Hash (mBuf, N * BLK, *H);
  }
  *Us = NowUs () - T0;
  return EFI_SUCCESS;
}

/**
  Throughput + integrity of the reference area (8 MiB) in the current mode.
**/
STATIC
BOOLEAN
Bench (
  IN CONST CHAR8  *Tag
  )
{
  UINT32      H;
  UINT64      Us;
  UINT32      KiBps;
  EFI_STATUS  Status;

  Status = ReadHash (REF_LBA, BENCH_BLOCKS, &H, &Us);
  if (EFI_ERROR (Status)) {
    Fail (Tag, Status);
    return FALSE;
  }
  KiBps = (UINT32)(((UINT64)BENCH_BLOCKS * BLK * 1000000ULL / 1024) / (Us ? Us : 1));
  if (mRefHash == 0) {
    mRefHash = H;
  }
  LOG ("EmmcTest: %a: read 8 MiB in %lu us = %u KiB/s (%u MB/s), hash %08x %a\n", Tag, Us, KiBps,
       KiBps / 1024, H, (H == mRefHash) ? "OK" : "MISMATCH");
  if (H != mRefHash) {
    mFails++;
    return FALSE;
  }
  return TRUE;
}

STATIC
VOID
Pattern (
  IN UINT8   *Buf,
  IN UINT64  Lba,
  IN UINTN   Blocks,
  IN UINT32  Seed
  )
{
  UINTN   I;
  UINT32  X;

  X = Seed ^ (UINT32)Lba;
  for (I = 0; I < Blocks * BLK / 4; I++) {
    X = X * 1664525U + 1013904223U;
    ((UINT32 *)Buf)[I] = X ^ (UINT32)(Lba + I / (BLK / 4));
  }
}

STATIC
EFI_STATUS
WriteChunks (
  IN UINT64  Lba,
  IN UINTN   Blocks,
  IN UINT32  Seed,
  IN UINT8   *Source,           // NULL = generate the pattern
  OUT UINT32 *H
  )
{
  UINTN       Done;
  UINTN       N;
  EFI_STATUS  Status;

  *H = 2166136261U;
  for (Done = 0; Done < Blocks; Done += N) {
    N = MIN ((UINTN)CHUNK_BLOCKS, Blocks - Done);
    if (Source == NULL) {
      Pattern (mBuf, Lba + Done, N, Seed);
    } else {
      CopyMem (mBuf, Source + Done * BLK, N * BLK);
    }
    *H     = Hash (mBuf, N * BLK, *H);
    Status = mT->Write (mT, Lba + Done, N, mBuf);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  return EFI_SUCCESS;
}

/**
  Save the region, write a pattern in the current mode, verify it in
  VerifySpeed, restore the region.
**/
STATIC
VOID
WriteVerify (
  IN CONST CHAR8  *Tag,
  IN UINT32       Seed,
  IN UINT32       VerifySpeed
  )
{
  UINTN            Done;
  UINTN            N;
  EFI_STATUS       Status;
  UINT32           Hw;
  UINT32           Hr;
  UINT32           Orig;
  UINT64           T0;
  UINT64           Us;
  EXYNOS_MMC_INFO  I;

  mT->GetInfo (mT, &I);

  Status = ReadHash (mRegion, REGION_BLOCKS, &Orig, &Us);
  if (EFI_ERROR (Status)) {
    Fail ("write test pre-read", Status);
    return;
  }
  for (Done = 0; Done < REGION_BLOCKS; Done += N) {
    N      = MIN ((UINTN)CHUNK_BLOCKS, (UINTN)REGION_BLOCKS - Done);
    Status = mT->Read (mT, mRegion + Done, N, mSave + Done * BLK);
    if (EFI_ERROR (Status)) {
      Fail ("write test save", Status);
      return;
    }
  }

  T0     = NowUs ();
  Status = WriteChunks (mRegion, REGION_BLOCKS, Seed, NULL, &Hw);
  Us     = NowUs () - T0;
  if (EFI_ERROR (Status)) {
    Fail ("write", Status);
  } else {
    LOG ("EmmcTest: %a: wrote 16 MiB in %lu us = %u KiB/s\n", Tag, Us,
         (UINT32)(16384ULL * 1000000ULL / (Us ? Us : 1)));
    if (VerifySpeed != I.Speed) {
      Status = mT->SetSpeed (mT, VerifySpeed);
      if (EFI_ERROR (Status)) {
        Fail ("switch for verify", Status);
      }
    }
    Status = ReadHash (mRegion, REGION_BLOCKS, &Hr, &Us);
    if (EFI_ERROR (Status)) {
      Fail ("verify read", Status);
    } else if (Hr != Hw) {
      mFails++;
      LOG ("EmmcTest: FAIL %a: written hash %08x, read back %08x\n", Tag, Hw, Hr);
    } else {
      LOG ("EmmcTest: %a: write verified (read in %a, %lu us)\n", Tag, SpeedName (VerifySpeed), Us);
    }
    if (VerifySpeed != I.Speed) {
      mT->SetSpeed (mT, I.Speed);
    }
  }

  //
  // Restore (always) and prove it.
  //
  Status = WriteChunks (mRegion, REGION_BLOCKS, 0, mSave, &Hw);
  if (!EFI_ERROR (Status)) {
    Status = ReadHash (mRegion, REGION_BLOCKS, &Hr, &Us);
  }
  if (EFI_ERROR (Status) || (Hr != Orig)) {
    mFails++;
    LOG ("EmmcTest: FAIL restore of the test region: %r, hash %08x want %08x\n", Status, Hr, Orig);
  }
}

/**
  Random small read/write mix inside the test region in the current mode;
  every read is compared with a RAM model of the region.
**/
STATIC
VOID
Stress (
  IN UINT32  Rounds
  )
{
  UINT32      R;
  UINT32      X;
  UINT64      Lba;
  UINTN       Blocks;
  UINTN       Off;
  EFI_STATUS  Status;
  UINT32      Hw;
  UINT32      Orig;
  UINT64      Us;
  UINTN       Errors;
  UINT64      T0;

  Status = ReadHash (mRegion, REGION_BLOCKS, &Orig, &Us);
  if (EFI_ERROR (Status)) {
    Fail ("stress pre-read", Status);
    return;
  }
  for (Off = 0; Off < REGION_BLOCKS; Off += CHUNK_BLOCKS) {
    Status = mT->Read (mT, mRegion + Off, CHUNK_BLOCKS, mOrig + Off * BLK);
    if (EFI_ERROR (Status)) {
      Fail ("stress save", Status);
      return;
    }
  }
  CopyMem (mSave, mOrig, REGION_BLOCKS * BLK);       // the model

  //
  // mSave holds the model.  Operations: write random pattern (model updated),
  // read back and compare.
  //
  X      = 12345;
  Errors = 0;
  T0     = NowUs ();
  for (R = 0; R < Rounds; R++) {
    X      = X * 1664525U + 1013904223U;
    Blocks = 1 + ((X >> 8) % 128);
    X      = X * 1664525U + 1013904223U;
    Off    = (X >> 4) % (REGION_BLOCKS - Blocks);
    Lba    = mRegion + Off;

    if ((R & 1) == 0) {
      Pattern (mBuf, Lba, Blocks, R);
      Status = mT->Write (mT, Lba, Blocks, mBuf);
      if (EFI_ERROR (Status)) {
        Errors++;
        LOG ("EmmcTest: stress %u: write LBA %lu x%u - %r\n", R, Lba, Blocks, Status);
        break;
      }
      CopyMem (mSave + Off * BLK, mBuf, Blocks * BLK);
    } else {
      SetMem (mBuf, Blocks * BLK, 0xA5);
      Status = mT->Read (mT, Lba, Blocks, mBuf);
      if (EFI_ERROR (Status)) {
        Errors++;
        LOG ("EmmcTest: stress %u: read LBA %lu x%u - %r\n", R, Lba, Blocks, Status);
        break;
      }
      if (CompareMem (mBuf, mSave + Off * BLK, Blocks * BLK) != 0) {
        Errors++;
        LOG ("EmmcTest: stress %u: data mismatch LBA %lu x%u\n", R, Lba, Blocks);
        break;
      }
    }
  }
  LOG ("EmmcTest: stress %a: %u rounds, %u errors, %lu ms\n", "done", R, (UINT32)Errors, (NowUs () - T0) / 1000);
  if (Errors != 0) {
    mFails++;
  }

  //
  // Restore the original content of the region and prove it.
  //
  Status = WriteChunks (mRegion, REGION_BLOCKS, 0, mOrig, &Hw);
  if (!EFI_ERROR (Status)) {
    Status = ReadHash (mRegion, REGION_BLOCKS, &Hw, &Us);
  }
  if (EFI_ERROR (Status) || (Hw != Orig)) {
    mFails++;
    LOG ("EmmcTest: FAIL stress restore: %r, hash %08x want %08x\n", Status, Hw, Orig);
  }
}

/**
  Scan the HS400 sample phase (CLKSEL[2:0]) and read-strobe delay: each point
  re-enters HS400 from scratch (the switch itself proves LBA 0 and EXT_CSD),
  then reads 128 KiB and compares the hash.  Picks the phase with the longest
  working delay window and sets its middle.
**/
STATIC
BOOLEAN
ScanGrid (
  VOID
  )
{
  UINT32      Sample;
  UINT32      D;
  UINT32      H;
  UINT64      Us;
  UINT32      BestSample;
  UINT32      BestStart;
  UINT32      BestLen;
  UINT32      Start;
  UINT32      Len;
  UINT32      ClkI;
  UINT32      BestClk;
  EFI_STATUS  Status;
  BOOLEAN     Pass;
  CHAR8       Row[16];
  STATIC CONST UINT32  Clocks[] = { 200000000, 100000000, 50000000, 40000000, 20000000 };

  BestSample = 0;
  BestStart  = 0;
  BestLen    = 0;
  BestClk    = 0;
  mT->SetSpeed (mT, EXYNOS_MMC_SPEED_DEFAULT);
  mT->SetTiming (mT, 0xFF, 0);
  for (ClkI = 0; ClkI < ARRAY_SIZE (Clocks) && BestLen == 0; ClkI++) {
    mT->SetTiming (mT, 0xFF, Clocks[ClkI]);
    for (Sample = 0; Sample < 8; Sample++) {
      Start = 0;
      Len   = 0;
      for (D = 0; D < 8; D++) {
        mT->SetSamplePhase (mT, Sample);
        mT->SetDqsDelay (mT, D * 32);
        Status = mT->SetSpeed (mT, EXYNOS_MMC_SPEED_HS400ES);
        Pass   = FALSE;
        if (!EFI_ERROR (Status)) {
          Status = ReadHash (REF_LBA, SCAN_BLOCKS, &H, &Us);
          Pass   = !EFI_ERROR (Status) && (H == mScanRef);
          mT->SetSpeed (mT, EXYNOS_MMC_SPEED_DEFAULT);
        }
        Row[D] = Pass ? '+' : '.';
        if (Pass) {
          if (Len == 0) {
            Start = D;
          }
          Len++;
          if (Len > BestLen) {
            BestLen    = Len;
            BestStart  = Start;
            BestSample = Sample;
            BestClk    = Clocks[ClkI];
          }
        } else {
          Len = 0;
        }
      }
      Row[8] = 0;
      LOG ("EmmcTest: hs400 request %u Hz sample %u dqs 0..224 step 32: %a\n", Clocks[ClkI], Sample, Row);
    }
  }
  if (BestLen == 0) {
    LOG ("EmmcTest: grid: no working HS400 point\n");
    mFails++;
    mT->SetTiming (mT, 0xFF, 0);
    return FALSE;
  }
  D = (BestStart + (BestLen - 1) / 2) * 32;
  LOG ("EmmcTest: grid: clock %u Hz, sample %u, dqs window %u..%u, using %u\n", BestClk, BestSample, BestStart * 32,
       (BestStart + BestLen - 1) * 32, D);
  mT->SetTiming (mT, 0xFF, BestClk);
  mT->SetSamplePhase (mT, BestSample);
  mT->SetDqsDelay (mT, D);
  return TRUE;
}

STATIC
VOID
DumpExtCsd (
  VOID
  )
{
  STATIC UINT8  E[512];
  EFI_STATUS    Status;

  Status = mT->ReadExtCsd (mT, E);
  if (EFI_ERROR (Status)) {
    Fail ("EXT_CSD", Status);
    return;
  }
  LOG ("EmmcTest: EXT_CSD rev %u, DEVICE_TYPE 0x%02x, STROBE_SUPPORT 0x%02x, HS_TIMING 0x%02x, BUS_WIDTH 0x%02x, "
       "POWER_CLASS 0x%02x, SEC_COUNT %u\n", E[192], E[196], E[184], E[185], E[183], E[187], *(UINT32 *)&E[212]);
}

STATIC
BOOLEAN
Has (
  IN CHAR16  *Args,
  IN CHAR16  *Word,
  IN BOOLEAN All
  )
{
  return All || ((Args != NULL) && (StrStr (Args, Word) != NULL));
}

EFI_STATUS
EFIAPI
EmmcTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EXYNOS_MMC_INFO            I;
  CHAR16                     *Args;
  BOOLEAN                    All;
  BOOLEAN                    Keep;
  EFI_LOADED_IMAGE_PROTOCOL  *Li;
  UINT32                     H;
  UINT64                     Us;
  UINT32                     Hz;
  STATIC CONST UINT32        ClockProbe[] = { 5000000, 10000000, 20000000, 26000000 };
  UINTN                      K;

  Args = NULL;
  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li)) &&
      (Li->LoadOptionsSize != 0)) {
    Args = Li->LoadOptions;
  }
  All  = (Args == NULL) ||
         ((StrStr (Args, L"clock") == NULL) && (StrStr (Args, L"irq") == NULL) && (StrStr (Args, L"hs") == NULL) &&
          (StrStr (Args, L"scan") == NULL) && (StrStr (Args, L"ddr") == NULL) && (StrStr (Args, L"hs200") == NULL) && (StrStr (Args, L"write") == NULL) && (StrStr (Args, L"stress") == NULL));
  Keep = (Args != NULL) && (StrStr (Args, L"keep") != NULL);

  Status = gBS->LocateProtocol (&gExynosMmcTestProtocolGuid, NULL, (VOID **)&mT);
  if (EFI_ERROR (Status)) {
    LOG ("EmmcTest: no test protocol - %r\n", Status);
    return Status;
  }
  mT->GetInfo (mT, &I);
  mBlocks = I.BlockCount;
  LOG ("EmmcTest: start, %lu blocks (%lu MiB)\n", mBlocks, mBlocks / 2048);
  Info ("at start");
  DumpExtCsd ();

  mBuf  = AllocatePages (EFI_SIZE_TO_PAGES (CHUNK_BLOCKS * BLK));
  mSave = AllocatePages (EFI_SIZE_TO_PAGES (REGION_BLOCKS * BLK));
  mOrig = AllocatePages (EFI_SIZE_TO_PAGES (REGION_BLOCKS * BLK));
  if ((mBuf == NULL) || (mSave == NULL) || (mOrig == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }
  mRegion  = mBlocks - 8192 - REGION_BLOCKS;
  mRegion &= ~(UINT64)(CHUNK_BLOCKS - 1);

  //
  // Reference data and baseline.
  //
  mRefHash = 0;
  Bench ("default, polled");
  if (ReadHash (REF_LBA, SCAN_BLOCKS, &mScanRef, &Us) != EFI_SUCCESS) {
    Fail ("scan reference", EFI_DEVICE_ERROR);
    return EFI_DEVICE_ERROR;
  }

  if (Has (Args, L"clock", All)) {
    //
    // Which card clock does a requested Hz really give?  In SDR 8-bit mode
    // the throughput in MB/s equals the clock in MHz.
    //
    for (K = 0; K < ARRAY_SIZE (ClockProbe); K++) {
      Hz = ClockProbe[K];
      mT->SetClock (mT, Hz);
      if (!EFI_ERROR (ReadHash (REF_LBA, 4096, &H, &Us))) {
        LOG ("EmmcTest: clock request %u Hz -> 2 MiB in %lu us = %u KB/s (8-bit SDR => ~%u kHz)\n", Hz, Us,
             (UINT32)(2048ULL * 1000000ULL / (Us ? Us : 1)), (UINT32)(2048ULL * 1000000ULL / (Us ? Us : 1)));
      }
    }
    mT->SetClock (mT, 20000000);
  }

  if (Has (Args, L"irq", All)) {
    Status = mT->SetIrq (mT, TRUE);
    if (EFI_ERROR (Status)) {
      Fail ("enable interrupt mode", Status);
    } else {
      Bench ("default, interrupt");
      Info ("after irq bench");
      if (Has (Args, L"write", All)) {
        WriteVerify ("default+irq", 1, EXYNOS_MMC_SPEED_DEFAULT);
      }
    }
  }

  if (Has (Args, L"hs", All)) {
    Status = mT->SetSpeed (mT, EXYNOS_MMC_SPEED_HS);
    if (EFI_ERROR (Status)) {
      Fail ("switch to HS", Status);
    } else {
      Info ("hs52");
      Bench ("hs52");
      if (Has (Args, L"write", All)) {
        WriteVerify ("hs52", 2, EXYNOS_MMC_SPEED_HS);
      }
      if (Has (Args, L"stress", FALSE)) {
        Stress (3000);
      }
    }
  }

  if (Has (Args, L"hs200", All)) {
    UINT32  Phase;
    UINT32  Drive;
    UINT32  BestPhase;
    UINT32  BestDrive;
    UINT32  BestLen;
    UINT32  RunStart;
    UINT32  RunLen;
    UINT32  BestStart;
    CHAR8   Row[16];

    BestPhase = 0xFF;
    BestDrive = 3;
    BestLen   = 0;
    BestStart = 0;
    mT->SetTiming (mT, 0xFF, 0);
    for (Drive = 0; Drive < 8; Drive++) {
      mT->SetTiming (mT, Drive, 0);
      RunLen   = 0;
      RunStart = 0;
      for (Phase = 0; Phase < 8; Phase++) {
        mT->SetSamplePhase (mT, Phase);
        Status     = mT->SetSpeed (mT, EXYNOS_MMC_SPEED_HS200);
        Row[Phase] = '.';
        if (!EFI_ERROR (Status)) {
          UINT32  Hc;
          UINT64  UsC;

          if (!EFI_ERROR (ReadHash (REF_LBA, SCAN_BLOCKS, &Hc, &UsC)) && (Hc == mScanRef)) {
            Row[Phase] = '+';
          }
          mT->SetSpeed (mT, EXYNOS_MMC_SPEED_DEFAULT);
        }
        if (Row[Phase] == '+') {
          if (RunLen == 0) {
            RunStart = Phase;
          }
          RunLen++;
          if (RunLen > BestLen) {
            BestLen   = RunLen;
            BestStart = RunStart;
            BestPhase = RunStart + (RunLen - 1) / 2;
            BestDrive = Drive;
          }
        } else {
          RunLen = 0;
        }
      }
      Row[8] = 0;
      LOG ("EmmcTest: hs200 drive %u, sample 0..7: %a\n", Drive, Row);
    }
    if (BestPhase == 0xFF) {
      Fail ("HS200 at any drive / sample phase", EFI_DEVICE_ERROR);
    } else {
      LOG ("EmmcTest: hs200: drive %u, sample window %u..%u, using %u\n", BestDrive, BestStart, BestStart + BestLen - 1, BestPhase);
      mT->SetTiming (mT, BestDrive, 0);
      mT->SetSamplePhase (mT, BestPhase);
      Status = mT->SetSpeed (mT, EXYNOS_MMC_SPEED_HS200);
      if (EFI_ERROR (Status)) {
        Fail ("switch to HS200", Status);
      } else {
        Info ("hs200");
        Bench ("hs200");
        if (Has (Args, L"write", All)) {
          WriteVerify ("hs200", 6, EXYNOS_MMC_SPEED_HS200);
        }
        if (Has (Args, L"stress", FALSE)) {
          Stress (3000);
        }
      }
    }
  }

  if (Has (Args, L"ddr", All)) {
    UINT32  Phase;
    UINT32  Drive;
    UINT32  ClkI;
    UINT32  FoundDrive;
    UINT32  FoundClk;
    UINT32  FoundPhase;
    CHAR8   Row[16];
    STATIC CONST UINT32  Clocks[] = { 52000000, 20000000, 5000000 };

    FoundPhase = 0xFF;
    FoundDrive = 0xFF;
    FoundClk   = 0;
    mT->SetOptions (mT, 1, 0xFFFFFFFFU);
    for (ClkI = 0; ClkI < ARRAY_SIZE (Clocks); ClkI++) {
      for (Drive = 0; Drive < 8; Drive++) {
        mT->SetTiming (mT, Drive, Clocks[ClkI]);
        for (Phase = 0; Phase < 8; Phase++) {
          mT->SetSamplePhase (mT, Phase);
          Status     = mT->SetSpeed (mT, EXYNOS_MMC_SPEED_DDR52);
          Row[Phase] = '.';
          if (!EFI_ERROR (Status)) {
            UINT32  Hc;
            UINT64  UsC;

            if (!EFI_ERROR (ReadHash (REF_LBA, SCAN_BLOCKS, &Hc, &UsC)) && (Hc == mScanRef)) {
              Row[Phase] = '+';
              if (FoundPhase == 0xFF) {
                FoundPhase = Phase;
                FoundDrive = Drive;
                FoundClk   = Clocks[ClkI];
              }
            }
            mT->SetSpeed (mT, EXYNOS_MMC_SPEED_DEFAULT);
          }
        }
        Row[8] = 0;
        LOG ("EmmcTest: ddr52 %u Hz drive %u, sample 0..7: %a\n", Clocks[ClkI], Drive, Row);
      }
      if (FoundPhase != 0xFF) {
        break;
      }
    }
    if (FoundPhase == 0xFF) {
      Fail ("DDR52 with any clock / drive / sample phase", EFI_DEVICE_ERROR);
    } else {
      mT->SetTiming (mT, FoundDrive, FoundClk);
      mT->SetSamplePhase (mT, FoundPhase);
      Status = mT->SetSpeed (mT, EXYNOS_MMC_SPEED_DDR52);
      if (EFI_ERROR (Status)) {
        Fail ("switch to DDR52", Status);
      } else {
        Info ("ddr52");
        mDdrWorks = Bench ("ddr52");
        if (Has (Args, L"write", All)) {
          WriteVerify ("ddr52", 5, EXYNOS_MMC_SPEED_DDR52);
        }
      }
    }
  }

  if (Has (Args, L"hs400", All) && (mDdrWorks || !All || !Has (Args, L"ddr", All) || TRUE)) {
    Status = mT->SetSpeed (mT, EXYNOS_MMC_SPEED_HS400ES);
    if (EFI_ERROR (Status) || Has (Args, L"scan", FALSE)) {
      if (!EFI_ERROR (Status)) {
        Info ("hs400 works with the board values");
      } else {
        LOG ("EmmcTest: HS400 with the board values failed (%r), scanning the delay / sample grid\n", Status);
      }
      if (ScanGrid ()) {
        Status = mT->SetSpeed (mT, EXYNOS_MMC_SPEED_HS400ES);
      }
    }
    if (EFI_ERROR (Status)) {
      Fail ("switch to HS400", Status);
    } else {
      Info ("hs400");
      Bench ("hs400");
      if (Has (Args, L"write", All)) {
        WriteVerify ("hs400", 3, EXYNOS_MMC_SPEED_HS400ES);
        WriteVerify ("hs400 write, default read", 4, EXYNOS_MMC_SPEED_DEFAULT);
        mT->SetSpeed (mT, EXYNOS_MMC_SPEED_HS400ES);
      }
      if (Has (Args, L"stress", All)) {
        Stress (2000);
      }
    }
  }

  Info ("end");
  if (!Keep) {
    mT->SetSpeed (mT, EXYNOS_MMC_SPEED_DEFAULT);
    mT->SetIrq (mT, FALSE);
    Info ("restored");
  }
  LOG ("EmmcTest: DONE, %u failure(s)\n", (UINT32)mFails);
  return EFI_SUCCESS;
}
