/** @file
  Shared SD / eMMC command layer (from Exynos441xPkg's SdCardDxe).

  Implements the standard SD and MMC identification and transfer state
  machine (CMD0/ACMD41 or CMD1/CMD2/CMD3/CMD7/CMD9/CMD16/CMD17/18/24/25)
  on top of a generic MMC_HOST ops vtable; the command/data engine is the
  host backend (Dwmci.c).

  eMMC: after selection the user data area is selected explicitly and the
  bus is widened with CMD6 SWITCH (8, then 4, then 1 bit), each width
  proven by reading LBA 0.  SD: ACMD6 4-bit, same proof.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MmcHost.h"

//
// Command indices.
//
#define CMD0_GO_IDLE        0
#define CMD1_SEND_OP_COND   1   // MMC
#define CMD2_ALL_SEND_CID   2
#define CMD3_SEND_RCA       3
#define CMD6_SWITCH         6
#define CMD7_SELECT         7
#define CMD8_SEND_IF_COND   8   // SD
#define CMD9_SEND_CSD       9
#define CMD12_STOP          12
#define CMD13_SEND_STATUS   13
#define CMD16_SET_BLOCKLEN  16
#define CMD17_READ_SINGLE   17
#define CMD18_READ_MULTI    18
#define CMD24_WRITE_SINGLE  24
#define CMD25_WRITE_MULTI   25
#define CMD55_APP_CMD       55
#define ACMD41_SEND_OP_COND 41  // SD
#define ACMD6_SET_BUS_WIDTH 6   // SD

//
// OCR bits.
//
#define OCR_BUSY        0x80000000U
#define OCR_CCS         0x40000000U  // card-capacity status (block addressing)
#define OCR_HCS         0x40000000U  // host-capacity support (in the request)
#define OCR_VDD_MMC     0x00FF8080U  // 2.7-3.6 V + 1.70-1.95 V window
#define OCR_VDD_SD      0x00FF8000U  // 2.7-3.6 V window

//
// MMC CMD8 is SEND_EXT_CSD (data read, 512 bytes); SEC_COUNT is bytes 212-215.
//
#define CMD8_SEND_EXT_CSD    8
#define EXT_CSD_SEC_COUNT    212
#define EXT_CSD_STROBE_SUPPORT  184
#define EXT_CSD_HS_TIMING       185
#define EXT_CSD_BUS_WIDTH_IDX   183
#define EXT_CSD_REV             192
#define EXT_CSD_DEVICE_TYPE     196
#define DEVICE_TYPE_HS52        (1U << 1)
#define DEVICE_TYPE_HS400_1V8   (1U << 6)
#define DEVICE_TYPE_DDR52_1V8   (1U << 2)
#define DEVICE_TYPE_HS200_1V8   (1U << 4)
#define HS_TIMING_HS200         2
#define MMC_HS200_CLOCK_HZ      200000000U
#define MMC_DDR52_CLOCK_HZ      52000000U
#define HS_TIMING_LEGACY        0
#define HS_TIMING_HS            1
#define HS_TIMING_HS400         3
#define BUS_WIDTH_DDR8          6
#define BUS_WIDTH_STROBE        0x80
#define MMC_HS_CLOCK_HZ         52000000U
#define MMC_HS400_CLOCK_HZ      200000000U

//
// MMC CMD6 SWITCH: Access[25:24] Index[23:16] Value[15:8].
//
#define EXT_CSD_PART_CONFIG  179
#define EXT_CSD_BUS_WIDTH    183
#define SWITCH_ACCESS_WRITE_BYTE  3
#define SWITCH_ACCESS_CLEAR_BITS  2
#define SWITCH_ARG(Access, Index, Value) \
  (((UINT32)(Access) << 24) | ((UINT32)(Index) << 16) | ((UINT32)(Value) << 8))
#define PART_CONFIG_ACCESS_MASK   0x07
#define R1_SWITCH_ERROR           (1U << 7)

//
// Transfer-state bus clock.  Neither card is switched to high-speed timing,
// so stay under the 25 MHz (SD) / 26 MHz (MMC) default-speed limit.
//
#define MMC_TRANSFER_CLOCK_HZ  20000000U


//
// Send a single command through the host, waiting for completion in the
// backend.  Resp may be NULL for commands whose response is not needed.
//
STATIC
EFI_STATUS
SendCmd (
  IN MMC_HOST  *Host,
  IN UINT32    Cmd,
  IN UINT32    Arg,
  IN UINT32    Flags,
  OUT UINT32   *Resp OPTIONAL
  )
{
  return Host->SendCmd (Host, Cmd, Arg, Flags, Resp);
}

//
// R1 card status: error bits (OUT_OF_RANGE .. AKE_SEQ_ERROR) and the
// CURRENT_STATE field.
//
#define R1_ERROR_MASK       0xFDF90008U
#define R1_STATE(r)         (((r) >> 9) & 0xF)
#define R1_STATE_TRAN       4

/**
  Wait until the card is back in the transfer state.  After a failed data
  phase the card may still be clocking out the block; a command sent then
  (ACMD6 in particular) is rejected and leaves the bus width mismatched.

  @retval EFI_SUCCESS  Card in tran.
  @retval EFI_TIMEOUT  Card never got there (~1 s).
**/
STATIC
EFI_STATUS
MmcWaitTran (
  IN MMC_HOST  *Host
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4];
  UINTN       Retry;
  UINTN       Errors;

  Resp[0] = 0;
  Errors  = 0;
  for (Retry = 0; Retry < 1000; Retry++) {
    Status = SendCmd (Host, CMD13_SEND_STATUS, (UINT32)Host->Card.Rca << 16,
                      HOST_CMD_RESP_SHORT, Resp);
    if (!EFI_ERROR (Status) && (R1_STATE (Resp[0]) == R1_STATE_TRAN)) {
      return EFI_SUCCESS;
    }
    //
    // A card that does not answer at all (each try costs the command
    // timeout, ~1 s) is not going to: give up after a few, the caller
    // resets the card.  A busy card still answers.
    //
    if (EFI_ERROR (Status) && (++Errors >= 5)) {
      break;
    }
    gBS->Stall (1000);
  }

  DEBUG ((DEBUG_ERROR, "MmcCore: 0x%x card not in tran, status 0x%x\n",
          Host->Base, Resp[0]));
  return EFI_TIMEOUT;
}

//
// SD: CMD8 SEND_IF_COND.  Echoes back the check pattern; a response means the
// card is SD v2.0 or later (and supports 2.7-3.6V).
//
STATIC
EFI_STATUS
SdCheckInterfaceCond (
  IN MMC_HOST  *Host,
  OUT BOOLEAN  *V2
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4] = { 0, 0, 0, 0 };

  Status = SendCmd (Host, CMD8_SEND_IF_COND, 0x000001AA,
                    HOST_CMD_RESP_SHORT, Resp);
  if (EFI_ERROR (Status)) {
    //
    // No response: card is SD v1.x or not SD at all.  Not fatal here.
    //
    *V2 = FALSE;
    return EFI_SUCCESS;
  }

  *V2 = TRUE;
  return EFI_SUCCESS;
}

//
// SD: ACMD41 (CMD55 + CMD41) loop until the card leaves the busy state.
//
STATIC
EFI_STATUS
SdSendOpCond (
  IN MMC_HOST  *Host,
  IN BOOLEAN   V2,
  OUT UINT32   *Ocr
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4];
  UINTN       Retry;

  for (Retry = 0; Retry < 1000; Retry++) {
    Status = SendCmd (Host, CMD55_APP_CMD, 0, HOST_CMD_RESP_SHORT, Resp);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // An argument with an empty voltage window is only an inquiry: the card
    // answers but never leaves the busy state.
    //
    Status = SendCmd (Host, ACMD41_SEND_OP_COND,
                      (V2 ? OCR_HCS : 0) | OCR_VDD_SD,
                      HOST_CMD_RESP_SHORT | HOST_CMD_NO_CRC, Resp);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Resp[0] & OCR_BUSY) {
      *Ocr = Resp[0];
      return EFI_SUCCESS;
    }

    gBS->Stall (1000);  // 1 ms between polls
  }

  return EFI_TIMEOUT;
}

//
// MMC: CMD1 SEND_OP_COND loop until the card leaves the busy state.
//
STATIC
EFI_STATUS
MmcSendOpCond (
  IN MMC_HOST  *Host,
  OUT UINT32   *Ocr
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4];
  UINTN       Retry;

  //
  // OCR query with argument 0 first, as Linux's mmc_attach_mmc() does.
  //
  Status = SendCmd (Host, CMD1_SEND_OP_COND, 0,
                    HOST_CMD_RESP_SHORT | HOST_CMD_NO_CRC, Resp);

  Resp[0] = 0;
  for (Retry = 0; Retry < 3000; Retry++) {
    //
    // Sector-mode request plus the voltage window; a CMD1 whose window does
    // not match the card sends it to the inactive state.
    //
    Status = SendCmd (Host, CMD1_SEND_OP_COND, OCR_HCS | OCR_VDD_MMC,
                      HOST_CMD_RESP_SHORT | HOST_CMD_NO_CRC, Resp);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: CMD1 #%u failed - %r\n", Retry, Status));
      return Status;
    }

    if (Resp[0] & OCR_BUSY) {
      *Ocr = Resp[0];
      return EFI_SUCCESS;
    }

    gBS->Stall (1000);
  }

  DEBUG ((DEBUG_ERROR, "MmcCore: CMD1 card still busy after %u polls, OCR 0x%x\n",
          Retry, Resp[0]));
  return EFI_TIMEOUT;
}

//
// Read a 128-bit CID (long response) and derive nothing from it for now; kept
// for DEBUG visibility of card identity.
//
STATIC
EFI_STATUS
SendAllSendCid (
  IN MMC_HOST  *Host
  )
{
  UINT32  Resp[4];

  return SendCmd (Host, CMD2_ALL_SEND_CID, 0, HOST_CMD_RESP_LONG, Resp);
}

//
// CMD9 SEND_CSD: parse the capacity out of the CSD register.
//
STATIC
EFI_STATUS
ParseCsd (
  IN MMC_HOST  *Host,
  IN CARD_TYPE Type
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4];
  UINT32      CsdStruct;
  UINT32      Mult, CSize;
  UINT32      ReadBlLen, CSizeMult;
  UINT64      Blocks;

  Status = SendCmd (Host, CMD9_SEND_CSD, (UINT32)Host->Card.Rca << 16,
                    HOST_CMD_RESP_LONG, Resp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // CSD is returned MSB-first in Resp[3] (bits 127:96) .. Resp[0] (31:0).
  //
  CsdStruct = (Resp[3] >> 30) & 0x3;

  if (CsdStruct == 1) {
    //
    // CSD v2.0 (SDHC / SDXC / eMMC >= 2GB): capacity = (C_SIZE+1) * 512KB.
    //
    // C_SIZE = CSD[69:48].
    CSize = ((Resp[2] & 0x3FU) << 16) | (Resp[1] >> 16);
    Blocks = ((UINT64)(CSize + 1)) * 1024;  // in 512-byte units
  } else {
    //
    // CSD v1.0: capacity = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN.
    //
    // READ_BL_LEN = CSD[83:80], C_SIZE = CSD[73:62], C_SIZE_MULT = CSD[49:47].
    ReadBlLen  = (Resp[2] >> 16) & 0xF;
    CSize      = ((Resp[2] & 0x3FFU) << 2) | (Resp[1] >> 30);
    CSizeMult  = (Resp[1] >> 15) & 0x7;
    Mult       = 1U << (CSizeMult + 2);
    Blocks     = ((UINT64)(CSize + 1)) * Mult;
    Blocks <<= ReadBlLen;
    Blocks >>= 9;  // normalise to 512-byte blocks
  }

  Host->Card.BlockCount = Blocks;
  return EFI_SUCCESS;
}

//
// MMC CMD8 SEND_EXT_CSD: one 512-byte block, take SEC_COUNT as the capacity.
//
EFI_STATUS
MmcRefreshExtCsd (
  IN  MMC_HOST  *Host,
  OUT UINT8     *ExtCsd
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4];

  Status = Host->PrepareData (Host, TRUE, 1, ExtCsd);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = SendCmd (Host, CMD8_SEND_EXT_CSD, 0,
                    HOST_CMD_RESP_SHORT | HOST_CMD_DATA, Resp);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Host->XferBlock (Host, TRUE, 1, ExtCsd);
}

STATIC
EFI_STATUS
MmcReadExtCsd (
  IN MMC_HOST  *Host
  )
{
  EFI_STATUS    Status;
  STATIC UINT8  ExtCsd[BLOCK_SIZE];
  UINT32        SecCount;

  Status = MmcRefreshExtCsd (Host, ExtCsd);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SecCount = ExtCsd[EXT_CSD_SEC_COUNT] |
             ((UINT32)ExtCsd[EXT_CSD_SEC_COUNT + 1] << 8) |
             ((UINT32)ExtCsd[EXT_CSD_SEC_COUNT + 2] << 16) |
             ((UINT32)ExtCsd[EXT_CSD_SEC_COUNT + 3] << 24);
  if (SecCount == 0) {
    return EFI_DEVICE_ERROR;
  }

  Host->Card.BlockCount    = SecCount;
  Host->Card.ExtCsdRev     = ExtCsd[EXT_CSD_REV];
  Host->Card.DeviceType    = ExtCsd[EXT_CSD_DEVICE_TYPE];
  Host->Card.StrobeSupport = ExtCsd[EXT_CSD_STROBE_SUPPORT];
  return EFI_SUCCESS;
}

//
// SD bus modes, fastest first.  Each is proven with a single-block read of
// LBA 0 before it is kept; MmcWaitTran() makes a failed attempt safe to
// follow with another ACMD6.
//
STATIC CONST struct {
  UINT32  Width;
  UINT32  ClockHz;
} mSdBusModes[] = {
  { 4, MMC_TRANSFER_CLOCK_HZ },
  { 1, MMC_TRANSFER_CLOCK_HZ },
  { 1, 3000000 },
};

STATIC
EFI_STATUS
SdTuneBus (
  IN MMC_HOST  *Host
  )
{
  EFI_STATUS    Status;
  UINT32        Resp[4];
  UINTN         Index;
  STATIC UINT8  Block[BLOCK_SIZE];

  for (Index = 0; Index < ARRAY_SIZE (mSdBusModes); Index++) {
    //
    // Card side first (ACMD6: 0 = 1-bit, 2 = 4-bit), then the host.
    //
    Status = MmcWaitTran (Host);
    if (!EFI_ERROR (Status)) {
      Status = SendCmd (Host, CMD55_APP_CMD, (UINT32)Host->Card.Rca << 16,
                        HOST_CMD_RESP_SHORT, Resp);
    }
    if (!EFI_ERROR (Status)) {
      Status = SendCmd (Host, ACMD6_SET_BUS_WIDTH,
                        (mSdBusModes[Index].Width == 4) ? 0x2 : 0x0,
                        HOST_CMD_RESP_SHORT, Resp);
    }
    if (!EFI_ERROR (Status) && (Resp[0] & R1_ERROR_MASK)) {
      Status = EFI_DEVICE_ERROR;
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: ACMD6 %u-bit failed - %r (R1 0x%x)\n",
              mSdBusModes[Index].Width, Status, Resp[0]));
      continue;
    }
    Host->SetBusWidth (Host, mSdBusModes[Index].Width);

    Status = Host->SetClock (Host, MIN (Host->MaxClockHz, mSdBusModes[Index].ClockHz));
    if (!EFI_ERROR (Status)) {
      Status = MmcReadBlocks (Host, 0, 1, Block);
    }

    DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
            "MmcCore: SD %u-bit @ %u Hz: %r (sig 0x%02x%02x)\n",
            mSdBusModes[Index].Width, mSdBusModes[Index].ClockHz, Status,
            Block[510], Block[511]));

    if (!EFI_ERROR (Status)) {
      Host->Card.BusWidth = mSdBusModes[Index].Width;
      return EFI_SUCCESS;
    }
  }

  return EFI_DEVICE_ERROR;
}

/**
  MMC CMD6 SWITCH and wait for the card to finish programming.
**/
STATIC
EFI_STATUS
MmcSwitch (
  IN MMC_HOST  *Host,
  IN UINT32    Arg
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4];

  Resp[0] = 0;
  Status  = MmcWaitTran (Host);
  if (!EFI_ERROR (Status)) {
    Status = SendCmd (Host, CMD6_SWITCH, Arg, HOST_CMD_RESP_BUSY, Resp);
  }
  if (!EFI_ERROR (Status)) {
    Status = MmcWaitTran (Host);   // prg -> tran
  }
  if (!EFI_ERROR (Status)) {
    //
    // SWITCH_ERROR is reported in the status after the switch.
    //
    Status = SendCmd (Host, CMD13_SEND_STATUS, (UINT32)Host->Card.Rca << 16,
                      HOST_CMD_RESP_SHORT, Resp);
    if (!EFI_ERROR (Status) && (Resp[0] & (R1_SWITCH_ERROR | R1_ERROR_MASK))) {
      Status = EFI_DEVICE_ERROR;
    }
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MmcCore: CMD6 0x%08x failed - %r (status 0x%x)\n",
            Arg, Status, Resp[0]));
  }
  return Status;
}

//
// eMMC bus widths, widest first (EXT_CSD BUS_WIDTH value 2 = 8, 1 = 4, 0 = 1).
//
STATIC CONST struct {
  UINT32  Width;
  UINT8   ExtCsdValue;
} mMmcBusWidths[] = {
  { 8, 2 },
  { 4, 1 },
  { 1, 0 },
};

STATIC
EFI_STATUS
MmcTuneBus (
  IN MMC_HOST  *Host
  )
{
  EFI_STATUS    Status;
  UINTN         Index;
  STATIC UINT8  Block[BLOCK_SIZE];

  Status = Host->SetClock (Host, MIN (Host->MaxClockHz, MMC_TRANSFER_CLOCK_HZ));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < ARRAY_SIZE (mMmcBusWidths); Index++) {
    if (mMmcBusWidths[Index].Width > Host->MaxBusWidth) {
      continue;
    }

    Status = MmcSwitch (Host, SWITCH_ARG (SWITCH_ACCESS_WRITE_BYTE, EXT_CSD_BUS_WIDTH,
                                          mMmcBusWidths[Index].ExtCsdValue));
    if (EFI_ERROR (Status)) {
      continue;
    }
    Host->SetBusWidth (Host, mMmcBusWidths[Index].Width);

    //
    // Proof: LBA 0 of the (GPT) eMMC is the protective MBR, 0x55AA at 510.
    // A wrong width or sample point can still read "successfully".
    //
    SetMem (Block, sizeof (Block), 0);
    Status = MmcReadBlocks (Host, 0, 1, Block);
    if (!EFI_ERROR (Status) && ((Block[510] != 0x55) || (Block[511] != 0xAA))) {
      Status = EFI_CRC_ERROR;
    }
    DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
            "MmcCore: eMMC %u-bit @ %u Hz: %r\n", mMmcBusWidths[Index].Width,
            MIN (Host->MaxClockHz, MMC_TRANSFER_CLOCK_HZ), Status));
    if (!EFI_ERROR (Status)) {
      Host->Card.BusWidth = mMmcBusWidths[Index].Width;
      return EFI_SUCCESS;
    }
  }

  return EFI_DEVICE_ERROR;
}


/**
  CMD6 SWITCH without waiting for the card to settle (the next command's
  state depends on a host timing change that follows the switch, so CMD13
  cannot be used in between).  The busy time is covered by the host's DAT0
  wait in front of the next command.
**/
STATIC
EFI_STATUS
MmcSwitchNoWait (
  IN MMC_HOST  *Host,
  IN UINT32    Arg
  )
{
  UINT32  Resp[4];

  return SendCmd (Host, CMD6_SWITCH, Arg, HOST_CMD_RESP_BUSY, Resp);
}

//
// Proof of a working mode: LBA 0 reads as a protective MBR, EXT_CSD reads
// back with the speed bits we set.
//
STATIC
EFI_STATUS
MmcProveMode (
  IN MMC_HOST  *Host,
  IN UINT8     ExpectHsTiming
  )
{
  EFI_STATUS    Status;
  STATIC UINT8  Block[BLOCK_SIZE];
  STATIC UINT8  Ext[BLOCK_SIZE];
  UINTN         Index;

  for (Index = 0; Index < 4; Index++) {
    SetMem (Block, sizeof (Block), 0);
    Status = MmcReadBlocks (Host, 0, 1, Block);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if ((Block[510] != 0x55) || (Block[511] != 0xAA)) {
      return EFI_CRC_ERROR;
    }
  }
  Status = MmcRefreshExtCsd (Host, Ext);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((Ext[EXT_CSD_HS_TIMING] & 0xF) != ExpectHsTiming) {
    DEBUG ((DEBUG_ERROR, "MmcCore: HS_TIMING reads %u, want %u\n",
            Ext[EXT_CSD_HS_TIMING] & 0xF, ExpectHsTiming));
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MmcEnterHs (
  IN MMC_HOST  *Host
  )
{
  EFI_STATUS  Status;

  if (!(Host->Card.DeviceType & DEVICE_TYPE_HS52) || (Host->SetSpeed == NULL)) {
    return EFI_UNSUPPORTED;
  }
  Status = MmcSwitch (Host, SWITCH_ARG (SWITCH_ACCESS_WRITE_BYTE, EXT_CSD_HS_TIMING, HS_TIMING_HS));
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Host->SetSpeed (Host, MmcSpeedHs);
  if (!EFI_ERROR (Status)) {
    Status = Host->SetClock (Host, MIN (Host->MaxClockHz, MMC_HS_CLOCK_HZ));
  }
  if (!EFI_ERROR (Status)) {
    Status = MmcProveMode (Host, HS_TIMING_HS);
  }
  return Status;
}

/**
  Card reset + full identification at the default speed (the way back from
  any faster mode, and the recovery after a failed switch).
**/
STATIC
EFI_STATUS
MmcBackToDefault (
  IN MMC_HOST  *Host
  )
{
  if (Host->SetSpeed != NULL) {
    Host->SetSpeed (Host, MmcSpeedDefault);
  }
  Host->SetClock (Host, 400000);
  Host->SetBusWidth (Host, 1);
  return MmcInitCard (Host, TRUE);
}

/**
  Switch the eMMC to a speed mode.  HS400 goes through HS, then the
  enhanced-strobe sequence of mmc.c (8-bit DDR + strobe, HS_TIMING 3, host
  timing, clock).  Any failure puts the card back to the default speed by
  re-initialising it.

  @retval EFI_SUCCESS      Mode active and proven.
  @retval EFI_UNSUPPORTED  Card or host cannot do it (nothing changed).
  @retval other            Mode failed; the card is back at default speed.
**/
EFI_STATUS
MmcSetSpeed (
  IN MMC_HOST   *Host,
  IN MMC_SPEED  Speed
  )
{
  EFI_STATUS  Status;

  if (Host->Card.Type != CardTypeMmc) {
    return EFI_UNSUPPORTED;
  }
  if (Speed == Host->Speed) {
    return EFI_SUCCESS;
  }

  if ((Speed == MmcSpeedDefault) || (Host->Speed == MmcSpeedHs400Es) || (Host->Speed == MmcSpeedDdr52) || (Host->Speed == MmcSpeedHs200)) {
    Status = MmcBackToDefault (Host);
    if (EFI_ERROR (Status) || (Speed == MmcSpeedDefault)) {
      return Status;
    }
  }

  if ((Speed == MmcSpeedHs200) &&
      (!(Host->Card.DeviceType & DEVICE_TYPE_HS200_1V8) || (Host->Card.BusWidth != 8) || (Host->SetSourceClock == NULL))) {
    return EFI_UNSUPPORTED;
  }
  if ((Speed == MmcSpeedDdr52) &&
      (!(Host->Card.DeviceType & DEVICE_TYPE_DDR52_1V8) || (Host->Card.BusWidth != 8))) {
    return EFI_UNSUPPORTED;
  }
  if ((Speed == MmcSpeedHs400Es) &&
      (!(Host->Card.DeviceType & DEVICE_TYPE_HS400_1V8) || (Host->SetSourceClock == NULL) || (Host->Card.BusWidth != 8))) {
    return EFI_UNSUPPORTED;
  }

  Status = EFI_SUCCESS;
  if (Speed == MmcSpeedHs200) {
    Status = MmcSwitch (Host, SWITCH_ARG (SWITCH_ACCESS_WRITE_BYTE, EXT_CSD_HS_TIMING, HS_TIMING_HS200));
    if (!EFI_ERROR (Status)) {
      Status = Host->SetSpeed (Host, MmcSpeedHs200);
    }
    if (!EFI_ERROR (Status)) {
      Status = Host->SetClock (Host, MIN (Host->MaxClockHz, (Host->DdrClockHz != 0) ? Host->DdrClockHz : MMC_HS200_CLOCK_HZ));
    }
    if (!EFI_ERROR (Status)) {
      Status = MmcProveMode (Host, HS_TIMING_HS200);
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: speed mode %u failed - %r, back to default\n", Speed, Status));
      MmcBackToDefault (Host);
      return Status;
    }
    DEBUG ((DEBUG_INFO, "MmcCore: eMMC speed mode %u (HS200), MaxClock %u Hz\n", Speed, Host->MaxClockHz));
    return EFI_SUCCESS;
  }
  if (Host->Speed == MmcSpeedDefault) {
    Status = MmcEnterHs (Host);
  }

  if (!EFI_ERROR (Status) && (Speed == MmcSpeedDdr52)) {
    Status = MmcSwitch (Host, SWITCH_ARG (SWITCH_ACCESS_WRITE_BYTE, EXT_CSD_BUS_WIDTH_IDX, BUS_WIDTH_DDR8));
    if (!EFI_ERROR (Status)) {
      //
      // Diagnostic: the card should now be in DDR while the host still runs
      // SDR, so an EXT_CSD read must fail (half data); if it succeeds the
      // card ignored the bus-width switch.
      //
      STATIC UINT8  Probe[BLOCK_SIZE];
      EFI_STATUS    ProbeStatus;

      ProbeStatus = MmcRefreshExtCsd (Host, Probe);
      DEBUG ((DEBUG_INFO, "MmcCore: card BUS_WIDTH switched to DDR8; SDR host reads EXT_CSD: %r (BUS_WIDTH %u HS_TIMING %u)\n",
              ProbeStatus, Probe[EXT_CSD_BUS_WIDTH_IDX], Probe[EXT_CSD_HS_TIMING]));
      MmcWaitTran (Host);
      Status = Host->SetSpeed (Host, MmcSpeedDdr52);
    }
    if (!EFI_ERROR (Status)) {
      Status = Host->SetClock (Host, MIN (Host->MaxClockHz, (Host->DdrClockHz != 0) ? Host->DdrClockHz : MMC_DDR52_CLOCK_HZ));
    }
    if (!EFI_ERROR (Status)) {
      Status = MmcProveMode (Host, HS_TIMING_HS);
    }
  }

  if (!EFI_ERROR (Status) && (Speed == MmcSpeedHs400Es)) {
    //
    // 8-bit DDR with enhanced strobe, then HS400 timing.  The card clock
    // stays at <= 52 MHz until the host side has switched.
    //
    Host->Strobe = (Host->Card.StrobeSupport & 1) != 0;
    Status = MmcSwitch (Host, SWITCH_ARG (SWITCH_ACCESS_WRITE_BYTE, EXT_CSD_BUS_WIDTH_IDX,
                                          BUS_WIDTH_DDR8 | (Host->Strobe ? BUS_WIDTH_STROBE : 0)));
    if (!EFI_ERROR (Status)) {
      Status = MmcSwitchNoWait (Host, SWITCH_ARG (SWITCH_ACCESS_WRITE_BYTE, EXT_CSD_HS_TIMING,
                                                  HS_TIMING_HS400));
    }
    if (!EFI_ERROR (Status)) {
      Status = Host->SetSpeed (Host, MmcSpeedHs400Es);
    }
    if (!EFI_ERROR (Status)) {
      Status = Host->SetClock (Host, MIN (Host->MaxClockHz, (Host->DdrClockHz != 0) ? Host->DdrClockHz : MMC_HS400_CLOCK_HZ));
    }
    if (!EFI_ERROR (Status)) {
      Status = MmcWaitTran (Host);
    }
    if (!EFI_ERROR (Status)) {
      Status = MmcProveMode (Host, HS_TIMING_HS400);
    }
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MmcCore: speed mode %u failed - %r, back to default\n", Speed, Status));
    MmcBackToDefault (Host);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "MmcCore: eMMC speed mode %u, MaxClock %u Hz\n", Speed, Host->MaxClockHz));
  return EFI_SUCCESS;
}

/**
  Bring a card on the given host to the transfer state and fill in Host->Card.

  @param[in,out] Host    Host controller (Base/ops already set by backend init).
  @param[in]     IsEmmc  TRUE for the eMMC (CMD1 path), FALSE for SD (ACMD41).

  @retval EFI_SUCCESS   Card identified and selected.
  @retval EFI_NO_MEDIA  No card / not responding.
  @retval EFI_TIMEOUT   Card never left the busy state.
**/
EFI_STATUS
MmcInitCard (
  IN MMC_HOST  *Host,
  IN BOOLEAN   IsEmmc
  )
{
  EFI_STATUS  Status;
  UINT32      Ocr;
  BOOLEAN     V2;

  Host->Card.Type        = CardTypeNone;
  Host->Card.BlockSize   = BLOCK_SIZE;
  Host->Card.HighCapacity = FALSE;


  //
  // CMD0: go idle.  No response.
  //
  Status = SendCmd (Host, CMD0_GO_IDLE, 0,
                    HOST_CMD_RESP_NONE | HOST_CMD_NO_CRC, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MmcCore: CMD0 failed - %r\n", Status));
    return Status;
  }

  if (IsEmmc) {
    Host->Card.Type = CardTypeMmc;

    Status = MmcSendOpCond (Host, &Ocr);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: CMD1 timeout\n"));
      return Status;
    }
  } else {
    Host->Card.Type = CardTypeSd;

    SdCheckInterfaceCond (Host, &V2);

    Status = SdSendOpCond (Host, V2, &Ocr);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: ACMD41 timeout\n"));
      return EFI_NO_MEDIA;
    }
  }

  Host->Card.HighCapacity = (Ocr & OCR_CCS) != 0;

  Status = SendAllSendCid (Host);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MmcCore: CMD2 failed - %r\n", Status));
    return Status;
  }

  //
  // CMD3: SD publishes its own RCA in the response; for MMC the host assigns
  // one (SET_RELATIVE_ADDR) and the response is a plain R1.
  //
  {
    UINT32  Resp[4];
    UINT32  Arg;

    Arg = (Host->Card.Type == CardTypeMmc) ? (1U << 16) : 0;
    Status = SendCmd (Host, CMD3_SEND_RCA, Arg, HOST_CMD_RESP_SHORT, Resp);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: CMD3 failed - %r\n", Status));
      return Status;
    }
    Host->Card.Rca = (Host->Card.Type == CardTypeMmc) ? 1 : (UINT16)(Resp[0] >> 16);
  }

  //
  // CMD9: read capacity from CSD.
  //
  Status = ParseCsd (Host, Host->Card.Type);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MmcCore: CMD9 failed - %r\n", Status));
    return Status;
  }

  //
  // CMD7: select the card into the transfer state.
  //
  {
    UINT32  Resp[4];

    Status = SendCmd (Host, CMD7_SELECT, (UINT32)Host->Card.Rca << 16,
                      HOST_CMD_RESP_BUSY, Resp);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: CMD7 failed - %r\n", Status));
      return Status;
    }
  }

  //
  // Sector-addressed eMMC (> 2 GB): CSD C_SIZE is a dummy 0xFFF, the real
  // capacity is EXT_CSD SEC_COUNT.  First data transfer on the bus, so it
  // also proves the read path.
  //
  if ((Host->Card.Type == CardTypeMmc) && Host->Card.HighCapacity) {
    Status = MmcReadExtCsd (Host);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: EXT_CSD read failed - %r\n", Status));
      return Status;
    }
  }

  //
  // CMD16: set the block length (only meaningful for byte-addressed cards).
  //
  if (!Host->Card.HighCapacity) {
    UINT32  Resp[4];

    Status = SendCmd (Host, CMD16_SET_BLOCKLEN, BLOCK_SIZE,
                      HOST_CMD_RESP_SHORT, Resp);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: CMD16 failed - %r\n", Status));
      return Status;
    }
  }

  Host->Card.BusWidth = 1;
  if (Host->Card.Type == CardTypeSd) {
    //
    // SD: pick the fastest bus mode that reads block 0 cleanly.
    //
    Status = SdTuneBus (Host);
  } else {
    //
    // eMMC: the bootloader may have left a boot partition selected; go back
    // to the user data area, clearing only PARTITION_ACCESS so the boot
    // configuration bits stay as they are.  Then widen the bus.
    //
    Status = MmcSwitch (Host, SWITCH_ARG (SWITCH_ACCESS_CLEAR_BITS, EXT_CSD_PART_CONFIG,
                                          PART_CONFIG_ACCESS_MASK));
    if (!EFI_ERROR (Status)) {
      Status = MmcTuneBus (Host);
    }
  }

  return Status;
}

//
// After a failed data phase, a multi-block command leaves the card in the
// data/receive state and every following command times out.  CMD12 brings
// it back to transfer state (its result is irrelevant); then wait for tran
// so the next command is not sent while the card is still busy.
//
STATIC
VOID
MmcAbortData (
  IN MMC_HOST  *Host,
  IN UINT32    Chunk
  )
{
  UINT32  Resp[4];

  if (Chunk > 1) {
    SendCmd (Host, CMD12_STOP, 0, HOST_CMD_RESP_BUSY, Resp);
  }
  MmcWaitTran (Host);
}

//
// Fallback after a failed multi-block command: CMD17/CMD24 per block.
//
STATIC
EFI_STATUS
MmcXferSingles (
  IN     MMC_HOST  *Host,
  IN     BOOLEAN   Read,
  IN     UINT32    Addr,
  IN     UINT32    Count,
  IN OUT UINT8     *Ptr
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4];
  UINT32      Index;

  for (Index = 0; Index < Count; Index++) {
    Status = Host->PrepareData (Host, Read, 1, Ptr);
    if (!EFI_ERROR (Status)) {
      Status = SendCmd (Host, Read ? CMD17_READ_SINGLE : CMD24_WRITE_SINGLE, Addr,
                        HOST_CMD_RESP_SHORT | HOST_CMD_DATA |
                        (Read ? 0 : HOST_CMD_WRITE), Resp);
    }
    if (!EFI_ERROR (Status)) {
      Status = Host->XferBlock (Host, Read, 1, Ptr);
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: 0x%x single-block addr 0x%x failed - %r\n",
              Host->Base, Addr, Status));
      MmcWaitTran (Host);
      return Status;
    }

    if (!Read) {
      Status = MmcWaitTran (Host);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    Ptr  += BLOCK_SIZE;
    Addr += Host->Card.HighCapacity ? 1 : Host->Card.BlockSize;
  }

  return EFI_SUCCESS;
}

//
// Translate an LBA to the card's addressing (block vs byte).
//
STATIC
UINT32
LbaToCardAddr (
  IN MMC_HOST  *Host,
  IN EFI_LBA   Lba
  )
{
  if (Host->Card.HighCapacity) {
    return (UINT32)Lba;
  }
  return (UINT32)Lba * Host->Card.BlockSize;
}

/**
  Read Blocks of 512 bytes starting at Lba.

  @param[in]  Host    Host whose card to read from.
  @param[in]  Lba     First block.
  @param[in]  Blocks  Number of blocks (>0).
  @param[out] Buffer  Destination, Blocks * 512 bytes.

  @retval EFI_SUCCESS           Data read.
  @retval EFI_INVALID_PARAMETER Blocks == 0 or Lba out of range.
  @retval EFI_DEVICE_ERROR      Command or transfer error.
**/
EFI_STATUS
MmcReadBlocks (
  IN MMC_HOST  *Host,
  IN EFI_LBA   Lba,
  IN UINTN     Blocks,
  OUT VOID     *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Remaining;
  UINT8       *Ptr;
  UINT32      Addr;

  if (Blocks == 0) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Lba + Blocks) > Host->Card.BlockCount) {
    return EFI_INVALID_PARAMETER;
  }

  Ptr       = Buffer;
  Remaining = Blocks;
  Addr      = LbaToCardAddr (Host, Lba);

  while (Remaining > 0) {
    UINT32  Chunk = (UINT32)MIN (Remaining, MMC_MAX_CHUNK_BLOCKS);
    UINT32  Cmd   = (Chunk > 1) ? CMD18_READ_MULTI : CMD17_READ_SINGLE;
    UINT32  Resp[4];

    Status = Host->PrepareData (Host, TRUE, Chunk, Ptr);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = SendCmd (Host, Cmd, Addr, HOST_CMD_RESP_SHORT | HOST_CMD_DATA,
                      Resp);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = Host->XferBlock (Host, TRUE, Chunk, Ptr);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: 0x%x read LBA %lu x%u failed - %r\n",
              Host->Base, Lba + (Blocks - Remaining), Chunk, Status));
      if ((Host->Speed == MmcSpeedHs400Es) || (Host->Speed == MmcSpeedDdr52) || (Host->Speed == MmcSpeedHs200)) {
        return Status;          // the caller resets the card; no slow abort / retry dance
      }
      MmcAbortData (Host, Chunk);
      if (Chunk == 1) {
        return Status;
      }

      //
      // Multi-block failed: redo this chunk one block at a time.
      //
      Status = MmcXferSingles (Host, TRUE, Addr, Chunk, Ptr);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Ptr        += Chunk * BLOCK_SIZE;
      Remaining  -= Chunk;
      Addr       += Host->Card.HighCapacity ? Chunk : Chunk * Host->Card.BlockSize;
      continue;
    }

    if (Chunk > 1) {
      //
      // CMD12: stop the multi-block read.
      //
      Status = SendCmd (Host, CMD12_STOP, 0,
                        HOST_CMD_RESP_BUSY, Resp);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    Ptr        += Chunk * BLOCK_SIZE;
    Remaining  -= Chunk;
    if (Host->Card.HighCapacity) {
      Addr += Chunk;
    } else {
      Addr += Chunk * Host->Card.BlockSize;
    }
  }

  return EFI_SUCCESS;
}

/**
  Write Blocks of 512 bytes starting at Lba.

  @param[in]  Host    Host whose card to write to.
  @param[in]  Lba     First block.
  @param[in]  Blocks  Number of blocks (>0).
  @param[in]  Buffer  Source, Blocks * 512 bytes.

  @retval EFI_SUCCESS           Data written.
  @retval EFI_INVALID_PARAMETER Blocks == 0 or Lba out of range.
  @retval EFI_DEVICE_ERROR      Command or transfer error.
**/
EFI_STATUS
MmcWriteBlocks (
  IN MMC_HOST  *Host,
  IN EFI_LBA   Lba,
  IN UINTN     Blocks,
  IN VOID      *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Remaining;
  UINT8       *Ptr;
  UINT32      Addr;

  if (Blocks == 0) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Lba + Blocks) > Host->Card.BlockCount) {
    return EFI_INVALID_PARAMETER;
  }

  Ptr       = Buffer;
  Remaining = Blocks;
  Addr      = LbaToCardAddr (Host, Lba);

  while (Remaining > 0) {
    UINT32  Chunk = (UINT32)MIN (Remaining, MMC_MAX_CHUNK_BLOCKS);
    UINT32  Cmd   = (Chunk > 1) ? CMD25_WRITE_MULTI : CMD24_WRITE_SINGLE;
    UINT32  Resp[4];

    Status = Host->PrepareData (Host, FALSE, Chunk, Ptr);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = SendCmd (Host, Cmd, Addr,
                      HOST_CMD_RESP_SHORT | HOST_CMD_DATA | HOST_CMD_WRITE,
                      Resp);
    if (EFI_ERROR (Status)) {
      UINT32  St[4];

      St[0] = 0;
      SendCmd (Host, CMD13_SEND_STATUS, (UINT32)Host->Card.Rca << 16, HOST_CMD_RESP_SHORT, St);
      DEBUG ((DEBUG_ERROR, "MmcCore: CMD%u LBA %lu x%u - %r, card status 0x%08x\n",
              Cmd, Lba + (Blocks - Remaining), Chunk, Status, St[0]));
      return Status;
    }

    Status = Host->XferBlock (Host, FALSE, Chunk, Ptr);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcCore: 0x%x write LBA %lu x%u failed - %r\n",
              Host->Base, Lba + (Blocks - Remaining), Chunk, Status));
      if ((Host->Speed == MmcSpeedHs400Es) || (Host->Speed == MmcSpeedDdr52) || (Host->Speed == MmcSpeedHs200)) {
        return Status;          // the caller resets the card; no slow abort / retry dance
      }
      MmcAbortData (Host, Chunk);
      if (Chunk == 1) {
        return Status;
      }

      //
      // Multi-block failed: redo this chunk one block at a time.
      //
      Status = MmcXferSingles (Host, FALSE, Addr, Chunk, Ptr);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Ptr        += Chunk * BLOCK_SIZE;
      Remaining  -= Chunk;
      Addr       += Host->Card.HighCapacity ? Chunk : Chunk * Host->Card.BlockSize;
      continue;
    }

    if (Chunk > 1) {
      Status = SendCmd (Host, CMD12_STOP, 0,
                        HOST_CMD_RESP_BUSY, Resp);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    //
    // The card programs the data after the transfer (prg state, DAT0
    // busy); it takes the next command only once it is back in tran.
    //
    Status = MmcWaitTran (Host);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Ptr        += Chunk * BLOCK_SIZE;
    Remaining  -= Chunk;
    if (Host->Card.HighCapacity) {
      Addr += Chunk;
    } else {
      Addr += Chunk * Host->Card.BlockSize;
    }
  }

  return EFI_SUCCESS;
}
