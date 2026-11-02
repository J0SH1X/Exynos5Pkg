/** @file
  Broadcom BCM4354 (FullMAC, SDIO) host side: see BcmSdio.h.

  Sequences follow the kernel's bcmdhd (dhd_sdio.c, aiutils.c, bcmsdh*.c),
  with what the device taught us on top (WlanTestApp, 2026-11-01):
    - the dw_mmc channel has a 64-bit FIFO; the IDMAC stalls on small SDIO
      transfers, so data goes through the FIFO by the CPU, each entry as two
      32-bit accesses (low half at DATA, then high half at DATA + 4)
    - function block sizes are 0 after reset: byte mode moves nothing
      beyond them (set 64 for F1, 512 for F2 like bcmsdh_sdmmc)
    - after ALP_AVAIL, CHIPCLKCSR must be FORCE_HW_CLKREQ_OFF | FORCE_ALP or
      every backplane access hangs
    - a failed CMD53 leaves the function in the transfer state: abort it
      through CCCR I/O Abort

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "BcmSdio.h"

#define B(n)                 (1U << (n))

//
// SDIO.
//
#define CMD3_SEND_RCA        3
#define CMD5_IO_OP_COND      5
#define CMD7_SELECT          7
#define CMD52_IO_RW_DIRECT   52
#define CMD53_IO_RW_EXTENDED 53

#define CCCR_IO_ENABLE       0x02
#define CCCR_IO_READY        0x03
#define CCCR_IO_ABORT        0x06
#define CCCR_BUS_IF          0x07
#define FBR_BLKSIZE(fn)      ((fn) * 0x100 + 0x10)
#define F1_BLOCK_SIZE        64
#define F2_BLOCK_SIZE        512

//
// Broadcom function 1 registers (sbsdio.h).
//
#define SB_FUNC1_SBADDRLOW   0x1000A
#define SB_FUNC1_SBADDRMID   0x1000B
#define SB_FUNC1_SBADDRHIGH  0x1000C
#define SB_FUNC1_CHIPCLKCSR  0x1000E
#define SB_FUNC1_FRAMECTRL   0x1000D
#define SB_FUNC1_RFRAMEBCLO  0x1001B
#define SB_FUNC1_RFRAMEBCHI  0x1001C
#define SFC_RF_TERM          B (0)
#define SB_FORCE_ALP         0x01
#define SB_FORCE_HT          0x02
#define SB_ALP_AVAIL_REQ     0x08
#define SB_HT_AVAIL_REQ      0x10
#define SB_FORCE_HW_CLKREQ_OFF 0x20
#define SB_ALP_AVAIL         0x40
#define SB_HT_AVAIL          0x80
#define SB_WINDOW_MASK       0xFFFF8000U
#define SB_OFFSET_MASK       0x7FFF
#define SB_ACCESS_2_4B_FLAG  0x08000
#define CHIPCOMMON_BASE      0x18000000
#define CHIPCOMMON_EROMPTR   0xFC

//
// AI enumeration ROM and core wrapper (aidmp.h, hndsoc.h).
//
#define ER_TAG               0xE
#define ER_TAG1              0x6
#define ER_VALID             1
#define ER_CI                0
#define ER_MP                2
#define ER_ADD               4
#define ER_END               0xE
#define ER_SZ_MAX            4096
#define CIA_MFG(x)           (((x) & 0xFFF00000) >> 20)
#define CIA_CID(x)           (((x) & 0x000FFF00) >> 8)
#define CIB_REV(x)           (((x) & 0xFF000000) >> 24)
#define CIB_NSW(x)           (((x) & 0x00F80000) >> 19)
#define CIB_NMW(x)           (((x) & 0x0007C000) >> 14)
#define CIB_NSP(x)           (((x) & 0x00003E00) >> 9)
#define CIB_NMP(x)           (((x) & 0x000001F0) >> 4)
#define AD_ADDR_MASK         0xFFFFF000
#define AD_SP(x)             (((x) & 0x00000F00) >> 8)
#define AD_ST_MASK           0x000000C0
#define AD_ST_SLAVE          0x00000000
#define AD_ST_BRIDGE         0x00000040
#define AD_ST_SWRAP          0x00000080
#define AD_ST_MWRAP          0x000000C0
#define AD_SZ_MASK           0x00000030
#define AD_SZ_SHIFT          4
#define AD_SZ_SZD            0x00000030
#define AD_AG32              0x00000008
#define AD_SZ_BASE           0x00001000
#define SD_SZ_MASK           0xFFFFF000
#define SD_SG32              0x00000008
#define SI_CORE_SIZE         0x1000
#define MFGID_ARM            0x43B
#define DEF_AI_COMP          0xFFF
#define OOB_ROUTER_CORE_ID   0x367

#define CORE_ID_SDIOD        0x829
#define CORE_ID_ARMCR4       0x83E

#define AI_IOCTRL            0x408
#define AI_RESETCTRL         0x800
#define AI_RESETSTATUS       0x804
#define AIRC_RESET           1
#define SICF_CLOCK_EN        0x0001
#define SICF_FGC             0x0002
#define SICF_CPUHALT         0x0020

#define CR4_CAP              0x04
#define CR4_BANKIDX          0x40
#define CR4_BANKINFO         0x44
#define CR4_BSZ_MULT         8192
#define CR4_4350_RAM_BASE    0x180000

//
// SDIO device core (sbsdpcmdev.h) and SDPCM (bcmsdpcm.h).
//
#define SDIOD_INTSTATUS      0x20
#define SDIOD_HOSTINTMASK    0x24
#define SDIOD_TOSBMAILBOX    0x40
#define SDIOD_TOSBMAILBOXDATA 0x48
#define SDIOD_TOHOSTMAILBOXDATA 0x4C
#define I_HMB_SW_MASK        0x000000F0
#define I_HMB_FRAME_IND      B (6)
#define I_HMB_HOST_INT       B (7)
#define SMB_NAK              B (0)
#define SMB_INT_ACK          B (1)
#define SDPCM_PROT_VERSION   4
#define SMB_DATA_VERSION_SHIFT 16
#define HMB_DATA_DEVREADY    0x02
#define HMB_DATA_FWREADY     0x08
#define HMB_DATA_FWHALT      0x10

#define SDPCM_FRAMETAG_LEN   4
#define SDPCM_HDRLEN         12
#define FIRSTREAD            32
#define RXALIGN              32

//
// BCDC (bcmcdc.h).
//
#define CDC_HDRLEN           16
#define CDCF_IOC_ERROR       0x01
#define CDCF_IOC_SET         0x02
#define CDCF_IOC_ID_SHIFT    16
#define BDC_HDRLEN           4

//
// dw_mmc registers used for the PIO data path.
//
#define DW_CTRL              0x00
#define DW_BLKSIZ            0x1C
#define DW_BYTCNT            0x20
#define DW_RINTSTS           0x44
#define DW_INTMASK           0x24
#define DW_MINTSTS           0x40
#define DW_STATUS            0x48
#define DW_FIFOTH            0x4C
#define DW_TCBCNT            0x5C
#define DW_BMOD              0x80
#define DW_DATA              0x200
#define CTRL_FIFO_RST        B (1)
#define CTRL_IDMAC           B (25)
#define RINT_DTO             B (3)
#define RINT_DCRC            B (7)
#define RINT_DRTO            B (9)
#define RINT_HTO             B (10)
#define RINT_SBE             B (13)
#define RINT_EBE             B (15)
#define RINT_DATA_ERR        (RINT_DCRC | RINT_DRTO | RINT_HTO | RINT_SBE | RINT_EBE)
#define STATUS_FIFO_FULL     B (3)
#define RINT_SDIO            B (16)     // card interrupt (DAT1)
#define RINT_ALL_BUT_SDIO    (~RINT_SDIO)
#define CCCR_INT_ENABLE      0x04
#define FIFO_COUNT(s)        (((s) >> 17) & 0x1FFF)

STATIC
UINT32
StrLen8 (
  IN CONST CHAR8  *S
  )
{
  UINT32  N;

  for (N = 0; S[N] != '\0'; N++) {
  }
  return N;
}

STATIC
UINT32
Get32Le (
  IN CONST UINT8  *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC
VOID
Put32Le (
  OUT UINT8   *P,
  IN  UINT32  V
  )
{
  P[0] = (UINT8)V;
  P[1] = (UINT8)(V >> 8);
  P[2] = (UINT8)(V >> 16);
  P[3] = (UINT8)(V >> 24);
}

//
// ---------------------------------------------------------------------------
// SDIO commands.
// ---------------------------------------------------------------------------
//

STATIC
EFI_STATUS
Cmd52 (
  IN  BCM_SDIO  *Bcm,
  IN  UINT32    Function,
  IN  UINT32    Address,
  IN  BOOLEAN   Write,
  IN  UINT8     Data,
  OUT UINT8     *Out   OPTIONAL
  )
{
  UINT32      Resp[4];
  UINT32      Arg;
  UINTN       Try;
  EFI_STATUS  Status;

  Arg    = ((Write ? 1U : 0U) << 31) | (Function << 28) | ((Address & 0x1FFFF) << 9) | Data;
  Status = EFI_DEVICE_ERROR;
  for (Try = 0; Try < 3; Try++) {
    Status = Bcm->Host.SendCmd (&Bcm->Host, CMD52_IO_RW_DIRECT, Arg, HOST_CMD_RESP_SHORT, Resp);
    if (!EFI_ERROR (Status)) {
      if (((Resp[0] >> 8) & 0xCB) != 0) {
        DEBUG ((DEBUG_INFO, "Bcm: CMD52 fn %u 0x%05x R5 flags 0x%02x\n", Function, Address, (Resp[0] >> 8) & 0xFF));
        return EFI_DEVICE_ERROR;
      }
      if (Out != NULL) {
        *Out = (UINT8)(Resp[0] & 0xFF);
      }
      return EFI_SUCCESS;
    }
    gBS->Stall (1000);
  }
  DEBUG ((DEBUG_INFO, "Bcm: CMD52 fn %u 0x%05x - %r\n", Function, Address, Status));
  return Status;
}

STATIC
UINT8
Read8 (
  IN BCM_SDIO  *Bcm,
  IN UINT32    Function,
  IN UINT32    Address
  )
{
  UINT8  Value;

  Value = 0xFF;
  Cmd52 (Bcm, Function, Address, FALSE, 0, &Value);
  return Value;
}

STATIC
UINT32
BlockSizeOf (
  IN UINT32  Function
  )
{
  return (Function == 2) ? F2_BLOCK_SIZE : F1_BLOCK_SIZE;
}

/**
  CMD53 through the FIFO (PIO): byte mode up to the function's block size,
  block mode for whole blocks above it.  Length is a multiple of 4.
**/
STATIC
EFI_STATUS
Cmd53 (
  IN     BCM_SDIO  *Bcm,
  IN     UINT32    Function,
  IN     UINT32    Address,
  IN     BOOLEAN   Write,
  IN OUT UINT8     *Buffer,
  IN     UINT32    Length
  )
{
  UINT32      Resp[4];
  UINT32      Base;
  UINT32      Arg;
  UINT32      Bs;
  UINT32      Word[2];
  UINT32      Done;
  UINT32      Pushed;
  UINT32      Chunk;
  UINT32      Int;
  UINTN       Try;
  UINTN       Retry;
  EFI_STATUS  Status;

  Base = Bcm->Host.Base;
  Bs   = BlockSizeOf (Function);
  if ((Length == 0) || ((Length & 3) != 0) || ((Length > Bs) && ((Length % Bs) != 0)) || ((Length / Bs) > 511)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Length <= Bs) {
    Arg = (1U << 26) | ((Address & 0x1FFFF) << 9) | ((Length == 512) ? 0 : Length);
  } else {
    Arg = (1U << 27) | (1U << 26) | ((Address & 0x1FFFF) << 9) | (Length / Bs);
  }
  Arg |= ((Write ? 1U : 0U) << 31) | (Function << 28);

  Status = EFI_DEVICE_ERROR;
  for (Try = 0; Try < 3; Try++) {
    Resp[0] = 0;
    MmioAnd32 (Base + DW_CTRL, ~CTRL_IDMAC);
    MmioWrite32 (Base + DW_BMOD, 0);
    MmioOr32 (Base + DW_CTRL, CTRL_FIFO_RST);
    for (Retry = 0; (Retry < 1000) && ((MmioRead32 (Base + DW_CTRL) & CTRL_FIFO_RST) != 0); Retry++) {
      gBS->Stall (10);
    }
    MmioWrite32 (Base + DW_FIFOTH, (0U << 28) | (0U << 16) | 32);
    MmioWrite32 (Base + DW_BLKSIZ, (Length <= Bs) ? Length : Bs);
    MmioWrite32 (Base + DW_BYTCNT, Length);
    MmioWrite32 (Base + DW_RINTSTS, RINT_ALL_BUT_SDIO);

    Pushed = 0;
    if (Write) {
      while ((Pushed < Length) && ((MmioRead32 (Base + DW_STATUS) & STATUS_FIFO_FULL) == 0)) {
        Word[0] = 0;
        Word[1] = 0;
        CopyMem (Word, Buffer + Pushed, MIN ((UINT32)8, Length - Pushed));
        MmioWrite32 (Base + DW_DATA, Word[0]);
        MmioWrite32 (Base + DW_DATA + 4, Word[1]);
        Pushed += MIN ((UINT32)8, Length - Pushed);
      }
    }

    Status = Bcm->Host.SendCmd (&Bcm->Host, CMD53_IO_RW_EXTENDED, Arg,
                                HOST_CMD_RESP_SHORT | HOST_CMD_DATA | (Write ? HOST_CMD_WRITE : 0), Resp);
    if (EFI_ERROR (Status)) {
      gBS->Stall (1000);
      continue;
    }

    Done = 0;
    Int  = 0;
    for (Retry = 0; Retry < 400000; Retry++) {
      Int = MmioRead32 (Base + DW_RINTSTS);
      if (Write) {
        while ((Pushed < Length) && ((MmioRead32 (Base + DW_STATUS) & STATUS_FIFO_FULL) == 0)) {
          Word[0] = 0;
          Word[1] = 0;
          CopyMem (Word, Buffer + Pushed, MIN ((UINT32)8, Length - Pushed));
          MmioWrite32 (Base + DW_DATA, Word[0]);
          MmioWrite32 (Base + DW_DATA + 4, Word[1]);
          Pushed += MIN ((UINT32)8, Length - Pushed);
        }
      } else {
        while ((FIFO_COUNT (MmioRead32 (Base + DW_STATUS)) > 0) && (Done < Length)) {
          Word[0] = MmioRead32 (Base + DW_DATA);
          Word[1] = MmioRead32 (Base + DW_DATA + 4);
          Chunk   = MIN ((UINT32)8, Length - Done);
          CopyMem (Buffer + Done, Word, Chunk);
          Done += Chunk;
        }
      }
      if ((Int & RINT_DATA_ERR) != 0) {
        break;
      }
      if (((Int & RINT_DTO) != 0) && (Write ? (Pushed >= Length) : (Done >= Length))) {
        break;
      }
      if ((Retry & 0xFF) == 0xFF) {
        gBS->Stall (2);
      }
    }
    MmioWrite32 (Base + DW_RINTSTS, Int & RINT_ALL_BUT_SDIO);

    if (((Int & RINT_DATA_ERR) != 0) || ((Int & RINT_DTO) == 0) || (!Write && (Done < Length))) {
      DEBUG ((DEBUG_INFO, "Bcm: CMD53 arg 0x%08x data failed, RINTSTS 0x%x, %u/%u bytes, TCBCNT %u, R5 0x%08x\n",
              Arg, Int, Write ? Pushed : Done, Length, MmioRead32 (Base + DW_TCBCNT), Resp[0]));
      Status = ((Int & RINT_DRTO) != 0) ? EFI_TIMEOUT : EFI_DEVICE_ERROR;
      Cmd52 (Bcm, 0, CCCR_IO_ABORT, TRUE, (UINT8)Function, NULL);
      gBS->Stall (1000);
      continue;
    }
    if (((Resp[0] >> 8) & 0xCB) != 0) {
      DEBUG ((DEBUG_INFO, "Bcm: CMD53 fn %u 0x%05x R5 flags 0x%02x\n", Function, Address, (Resp[0] >> 8) & 0xFF));
      return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
  }
  Bcm->Errors++;
  DEBUG ((DEBUG_ERROR, "Bcm: CMD53 fn %u 0x%05x %u bytes %a - %r\n", Function, Address, Length,
          Write ? "write" : "read", Status));
  return Status;
}

//
// ---------------------------------------------------------------------------
// Backplane.
// ---------------------------------------------------------------------------
//

STATIC
EFI_STATUS
SetWindow (
  IN BCM_SDIO  *Bcm,
  IN UINT32    Address
  )
{
  UINT32      Window;
  EFI_STATUS  Status;

  Window = Address & SB_WINDOW_MASK;
  if (Window == Bcm->Window) {
    return EFI_SUCCESS;
  }
  Status = Cmd52 (Bcm, 1, SB_FUNC1_SBADDRLOW, TRUE, (UINT8)((Window >> 8) & 0x80), NULL);
  if (!EFI_ERROR (Status)) {
    Status = Cmd52 (Bcm, 1, SB_FUNC1_SBADDRMID, TRUE, (UINT8)((Window >> 16) & 0xFF), NULL);
  }
  if (!EFI_ERROR (Status)) {
    Status = Cmd52 (Bcm, 1, SB_FUNC1_SBADDRHIGH, TRUE, (UINT8)((Window >> 24) & 0xFF), NULL);
  }
  Bcm->Window = EFI_ERROR (Status) ? 0xFFFFFFFFU : Window;
  return Status;
}

STATIC
UINT32
Bp32 (
  IN BCM_SDIO  *Bcm,
  IN UINT32    Address
  )
{
  UINT8  Data[4];

  if (EFI_ERROR (SetWindow (Bcm, Address)) ||
      EFI_ERROR (Cmd53 (Bcm, 1, (Address & SB_OFFSET_MASK) | SB_ACCESS_2_4B_FLAG, FALSE, Data, 4))) {
    return 0xFFFFFFFFU;
  }
  return Get32Le (Data);
}

STATIC
EFI_STATUS
Bp32Write (
  IN BCM_SDIO  *Bcm,
  IN UINT32    Address,
  IN UINT32    Value
  )
{
  UINT8       Data[4];
  EFI_STATUS  Status;

  Put32Le (Data, Value);
  Status = SetWindow (Bcm, Address);
  if (!EFI_ERROR (Status)) {
    Status = Cmd53 (Bcm, 1, (Address & SB_OFFSET_MASK) | SB_ACCESS_2_4B_FLAG, TRUE, Data, 4);
  }
  return Status;
}

/**
  Backplane memory (dhdsdio_membytes()): whole F1 blocks where possible,
  never across a 32 KB window.  Size is a multiple of 4.
**/
STATIC
EFI_STATUS
BpMemory (
  IN     BCM_SDIO  *Bcm,
  IN     BOOLEAN   Write,
  IN     UINT32    Address,
  IN OUT UINT8     *Data,
  IN     UINT32    Size
  )
{
  UINT32      Chunk;
  UINT32      Room;
  EFI_STATUS  Status;

  while (Size > 0) {
    Room  = SB_OFFSET_MASK + 1 - (Address & SB_OFFSET_MASK);
    Chunk = MIN (Size, MIN (Room, (UINT32)(F1_BLOCK_SIZE * 64)));
    if (Chunk > F1_BLOCK_SIZE) {
      Chunk -= Chunk % F1_BLOCK_SIZE;
    }
    Status = SetWindow (Bcm, Address);
    if (!EFI_ERROR (Status)) {
      Status = Cmd53 (Bcm, 1, Address & SB_OFFSET_MASK, Write, Data, Chunk);
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Bcm: backplane %a 0x%08x (%u bytes) failed - %r\n", Write ? "write" : "read",
              Address, Chunk, Status));
      return Status;
    }
    Address += Chunk;
    Data    += Chunk;
    Size    -= Chunk;
  }
  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// Cores (ai_scan(), ai_core_reset()).
// ---------------------------------------------------------------------------
//

STATIC
UINT32
EromEntry (
  IN BCM_SDIO  *Bcm,
  IN UINT32    Mask,
  IN UINT32    Match
  )
{
  UINT32  Entry;
  UINT32  Size;

  for (Size = 0; ; ) {
    Entry         = Bp32 (Bcm, Bcm->EromPtr);
    Bcm->EromPtr += 4;
    if (Mask == 0) {
      break;
    }
    if ((Entry & ER_VALID) == 0) {
      continue;
    }
    if ((Entry == (ER_END | ER_VALID)) || ((Entry & Mask) == Match)) {
      break;
    }
    Size += 4;
    if (Size >= ER_SZ_MAX) {
      break;
    }
  }
  return Entry;
}

STATIC
UINT32
GetAsd (
  IN  BCM_SDIO  *Bcm,
  IN  UINT32    Sp,
  IN  UINT32    St,
  OUT UINT32    *AddrL,
  OUT UINT32    *AddrH,
  OUT UINT32    *SizeL,
  OUT UINT32    *SizeH
  )
{
  UINT32  Asd;
  UINT32  Szd;

  Asd = EromEntry (Bcm, ER_VALID, ER_VALID);
  if (((Asd & ER_TAG1) != ER_ADD) || (AD_SP (Asd) != Sp) || ((Asd & AD_ST_MASK) != St)) {
    Bcm->EromPtr -= 4;
    return 0;
  }
  *AddrL = Asd & AD_ADDR_MASK;
  *AddrH = (Asd & AD_AG32) ? EromEntry (Bcm, 0, 0) : 0;
  *SizeH = 0;
  if ((Asd & AD_SZ_MASK) == AD_SZ_SZD) {
    Szd    = EromEntry (Bcm, 0, 0);
    *SizeL = Szd & SD_SZ_MASK;
    if (Szd & SD_SG32) {
      *SizeH = EromEntry (Bcm, 0, 0);
    }
  } else {
    *SizeL = AD_SZ_BASE << ((Asd & AD_SZ_MASK) >> AD_SZ_SHIFT);
  }
  return Asd;
}

STATIC
VOID
ScanCores (
  IN BCM_SDIO  *Bcm
  )
{
  UINT32   Cia, Cib, Cid, Nmw, Nsw, Nmp, Nsp;
  UINT32   Asd, AddrL, AddrH, SizeL, SizeH;
  UINT32   I, J, Limit, Base, Wrap;
  BOOLEAN  Bridge;

  Bcm->CoreCount = 0;
  Bcm->EromPtr   = Bp32 (Bcm, CHIPCOMMON_BASE + CHIPCOMMON_EROMPTR);
  if ((Bcm->EromPtr == 0) || (Bcm->EromPtr == 0xFFFFFFFFU)) {
    return;
  }

  for (Limit = 0; Limit < 256; Limit++) {
    Bridge = FALSE;
    Wrap   = 0;
    Cia    = EromEntry (Bcm, ER_TAG, ER_CI);
    if (Cia == (ER_END | ER_VALID)) {
      return;
    }
    Cib = EromEntry (Bcm, 0, 0);
    if ((Cib & ER_TAG) != ER_CI) {
      return;
    }
    Cid = CIA_CID (Cia);
    Nmw = CIB_NMW (Cib);
    Nsw = CIB_NSW (Cib);
    Nmp = CIB_NMP (Cib);
    Nsp = CIB_NSP (Cib);

    if (((CIA_MFG (Cia) == MFGID_ARM) && (Cid == DEF_AI_COMP)) || (Nsp == 0)) {
      continue;
    }
    if ((Nmw + Nsw) == 0) {
      if (Cid == OOB_ROUTER_CORE_ID) {
        GetAsd (Bcm, 0, AD_ST_SLAVE, &AddrL, &AddrH, &SizeL, &SizeH);
      }
      continue;
    }
    for (I = 0; I < Nmp; I++) {
      if ((EromEntry (Bcm, ER_VALID, ER_VALID) & ER_TAG) != ER_MP) {
        return;
      }
    }

    Asd = GetAsd (Bcm, 0, AD_ST_SLAVE, &AddrL, &AddrH, &SizeL, &SizeH);
    if (Asd == 0) {
      for ( ; ; ) {
        Asd = GetAsd (Bcm, 0, AD_ST_BRIDGE, &AddrL, &AddrH, &SizeL, &SizeH);
        if (Asd != 0) {
          Bridge = TRUE;
        } else if (Bridge) {
          break;
        } else if ((AddrH != 0) || (SizeH != 0) || (SizeL != SI_CORE_SIZE)) {
          return;
        }
      }
    }
    Base = AddrL;
    do {
      Asd = GetAsd (Bcm, 0, AD_ST_SLAVE, &AddrL, &AddrH, &SizeL, &SizeH);
    } while (Asd != 0);
    for (I = 1; I < Nsp; I++) {
      for (J = 0; GetAsd (Bcm, I, AD_ST_SLAVE, &AddrL, &AddrH, &SizeL, &SizeH) != 0; J++) {
      }
      if (J == 0) {
        return;
      }
    }
    for (I = 0; I < Nmw; I++) {
      if (GetAsd (Bcm, I, AD_ST_MWRAP, &AddrL, &AddrH, &SizeL, &SizeH) == 0) {
        return;
      }
      if (I == 0) {
        Wrap = AddrL;
      }
    }
    for (I = 0; I < Nsw; I++) {
      if (GetAsd (Bcm, ((Nsp == 1) ? 0 : 1) + I, AD_ST_SWRAP, &AddrL, &AddrH, &SizeL, &SizeH) == 0) {
        return;
      }
      if ((Nmw == 0) && (I == 0)) {
        Wrap = AddrL;
      }
    }
    if (Bridge || (Bcm->CoreCount >= BCM_MAX_CORES)) {
      continue;
    }
    Bcm->Cores[Bcm->CoreCount].Id   = Cid;
    Bcm->Cores[Bcm->CoreCount].Rev  = CIB_REV (Cib);
    Bcm->Cores[Bcm->CoreCount].Base = Base;
    Bcm->Cores[Bcm->CoreCount].Wrap = Wrap;
    Bcm->CoreCount++;
  }
}

STATIC
BCM_CORE *
FindCore (
  IN BCM_SDIO  *Bcm,
  IN UINT32    Id
  )
{
  UINT32  Index;

  for (Index = 0; Index < Bcm->CoreCount; Index++) {
    if (Bcm->Cores[Index].Id == Id) {
      return &Bcm->Cores[Index];
    }
  }
  return NULL;
}

STATIC
VOID
WaitIdle (
  IN BCM_SDIO  *Bcm,
  IN UINT32    Wrap
  )
{
  UINTN  Retry;

  for (Retry = 0; (Retry < 30) && (Bp32 (Bcm, Wrap + AI_RESETSTATUS) != 0); Retry++) {
    gBS->Stall (10);
  }
}

STATIC
VOID
CoreReset (
  IN BCM_SDIO  *Bcm,
  IN UINT32    Wrap,
  IN UINT32    Bits,
  IN UINT32    ResetBits
  )
{
  UINTN  Loop;

  WaitIdle (Bcm, Wrap);
  Bp32Write (Bcm, Wrap + AI_RESETCTRL, AIRC_RESET);
  gBS->Stall (10);
  WaitIdle (Bcm, Wrap);
  Bp32Write (Bcm, Wrap + AI_IOCTRL, Bits | ResetBits | SICF_FGC | SICF_CLOCK_EN);
  Bp32 (Bcm, Wrap + AI_IOCTRL);
  WaitIdle (Bcm, Wrap);
  for (Loop = 10; (Bp32 (Bcm, Wrap + AI_RESETCTRL) != 0) && (Loop > 0); Loop--) {
    WaitIdle (Bcm, Wrap);
    Bp32Write (Bcm, Wrap + AI_RESETCTRL, 0);
    WaitIdle (Bcm, Wrap);
  }
  Bp32Write (Bcm, Wrap + AI_IOCTRL, Bits | SICF_CLOCK_EN);
  Bp32 (Bcm, Wrap + AI_IOCTRL);
  gBS->Stall (1);
}

//
// ---------------------------------------------------------------------------
// Attach.
// ---------------------------------------------------------------------------
//

STATIC
EFI_STATUS
Enumerate (
  IN BCM_SDIO  *Bcm
  )
{
  UINT32      Resp[4];
  UINT32      Ocr;
  UINTN       Try;
  EFI_STATUS  Status;

  Status = Bcm->Host.SendCmd (&Bcm->Host, 0, 0, HOST_CMD_RESP_NONE, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  gBS->Stall (2000);
  Status = Bcm->Host.SendCmd (&Bcm->Host, CMD5_IO_OP_COND, 0, HOST_CMD_RESP_SHORT | HOST_CMD_NO_CRC, Resp);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Bcm: no SDIO card (CMD5 - %r)\n", Status));
    return Status;
  }
  Ocr = Resp[0] & 0x00FFFF00;
  for (Try = 0; Try < 200; Try++) {
    Status = Bcm->Host.SendCmd (&Bcm->Host, CMD5_IO_OP_COND, Ocr, HOST_CMD_RESP_SHORT | HOST_CMD_NO_CRC, Resp);
    if (!EFI_ERROR (Status) && ((Resp[0] & B (31)) != 0)) {
      break;
    }
    gBS->Stall (10000);
  }
  if (EFI_ERROR (Status) || ((Resp[0] & B (31)) == 0)) {
    return EFI_TIMEOUT;
  }
  Status = Bcm->Host.SendCmd (&Bcm->Host, CMD3_SEND_RCA, 0, HOST_CMD_RESP_SHORT, Resp);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Bcm->Rca = (UINT16)(Resp[0] >> 16);
  return Bcm->Host.SendCmd (&Bcm->Host, CMD7_SELECT, (UINT32)Bcm->Rca << 16, HOST_CMD_RESP_SHORT | HOST_CMD_RESP_BUSY, Resp);
}

EFI_STATUS
BcmSdioAttach (
  IN OUT BCM_SDIO  *Bcm,
  IN     UINT32    HostBase,
  IN     UINT32    CiuClockHz
  )
{
  EFI_STATUS  Status;
  UINT8       Value;
  UINTN       Try;
  UINT32      Cap;
  UINT32      Banks;
  UINT32      Index;

  Bcm->Window  = 0xFFFFFFFFU;
  Bcm->TxSeq   = 0;
  Bcm->TxMax   = 4;
  Bcm->IoctlId = 0;

  Status = DwmciInit (&Bcm->Host, HostBase, CiuClockHz, 4, 0x03040000U, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Enumerate (Bcm);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Bcm: SDIO enumeration failed - %r\n", Status));
    return Status;
  }

  //
  // 4-bit bus at up to 20 MHz.
  //
  Value = Read8 (Bcm, 0, CCCR_BUS_IF);
  Cmd52 (Bcm, 0, CCCR_BUS_IF, TRUE, (UINT8)((Value & ~0x03) | 0x02), NULL);
  Bcm->Host.SetBusWidth (&Bcm->Host, 4);
  Bcm->Host.SetClock (&Bcm->Host, 20000000);

  //
  // Function 1, block sizes, clocks.
  //
  Cmd52 (Bcm, 0, CCCR_IO_ENABLE, TRUE, B (1), NULL);
  for (Try = 0; (Try < 100) && ((Read8 (Bcm, 0, CCCR_IO_READY) & B (1)) == 0); Try++) {
    gBS->Stall (1000);
  }
  Cmd52 (Bcm, 0, FBR_BLKSIZE (1), TRUE, F1_BLOCK_SIZE & 0xFF, NULL);
  Cmd52 (Bcm, 0, FBR_BLKSIZE (1) + 1, TRUE, (F1_BLOCK_SIZE >> 8) & 0xFF, NULL);
  Cmd52 (Bcm, 0, FBR_BLKSIZE (2), TRUE, F2_BLOCK_SIZE & 0xFF, NULL);
  Cmd52 (Bcm, 0, FBR_BLKSIZE (2) + 1, TRUE, (F2_BLOCK_SIZE >> 8) & 0xFF, NULL);

  Cmd52 (Bcm, 1, SB_FUNC1_CHIPCLKCSR, TRUE, SB_FORCE_HW_CLKREQ_OFF | SB_ALP_AVAIL_REQ, NULL);
  Value = 0;
  for (Try = 0; Try < 100; Try++) {
    Value = Read8 (Bcm, 1, SB_FUNC1_CHIPCLKCSR);
    if ((Value & SB_ALP_AVAIL) != 0) {
      break;
    }
    gBS->Stall (1000);
  }
  if ((Value & SB_ALP_AVAIL) == 0) {
    DEBUG ((DEBUG_ERROR, "Bcm: ALP clock never available (CHIPCLKCSR 0x%02x)\n", Value));
    return EFI_DEVICE_ERROR;
  }
  Cmd52 (Bcm, 1, SB_FUNC1_CHIPCLKCSR, TRUE, SB_FORCE_HW_CLKREQ_OFF | SB_FORCE_ALP, NULL);
  gBS->Stall (100);

  //
  // Chip, cores, RAM.
  //
  Bcm->ChipId  = Bp32 (Bcm, CHIPCOMMON_BASE);
  Bcm->ChipRev = (Bcm->ChipId >> 16) & 0xF;
  DEBUG ((DEBUG_INFO, "Bcm: chip 0x%04x rev %u\n", Bcm->ChipId & 0xFFFF, Bcm->ChipRev));
  if ((Bcm->ChipId & 0xFFFF) != 0x4354) {
    return EFI_UNSUPPORTED;
  }
  ScanCores (Bcm);
  Bcm->Cr4   = FindCore (Bcm, CORE_ID_ARMCR4);
  Bcm->Sdiod = FindCore (Bcm, CORE_ID_SDIOD);
  if ((Bcm->Cr4 == NULL) || (Bcm->Sdiod == NULL) || (Bcm->Cr4->Wrap == 0)) {
    DEBUG ((DEBUG_ERROR, "Bcm: CPU or SDIO core missing (%u cores)\n", Bcm->CoreCount));
    return EFI_NOT_FOUND;
  }

  CoreReset (Bcm, Bcm->Cr4->Wrap, SICF_CPUHALT, SICF_CPUHALT);
  Cap   = Bp32 (Bcm, Bcm->Cr4->Base + CR4_CAP);
  Banks = (Cap & 0xF) + ((Cap & 0xF0) >> 4);
  Bcm->RamSize = 0;
  for (Index = 0; Index < Banks; Index++) {
    Bp32Write (Bcm, Bcm->Cr4->Base + CR4_BANKIDX, Index);
    Bcm->RamSize += ((Bp32 (Bcm, Bcm->Cr4->Base + CR4_BANKINFO) & 0x3F) + 1) * CR4_BSZ_MULT;
  }
  Bcm->RamBase = CR4_4350_RAM_BASE;
  DEBUG ((DEBUG_INFO, "Bcm: %u cores, Cortex-R4 RAM 0x%08x %u KiB, SDIO core 0x%08x\n",
          Bcm->CoreCount, Bcm->RamBase, Bcm->RamSize / 1024, Bcm->Sdiod->Base));
  return (Bcm->RamSize != 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

//
// ---------------------------------------------------------------------------
// Firmware.
// ---------------------------------------------------------------------------
//

/**
  The NVRAM text as the firmware wants it: comment and empty lines dropped,
  "name=value\0" per line, an extra NUL, padded to 4.
**/
STATIC
UINT32
BuildNvram (
  IN  CONST CHAR8  *Text,
  IN  UINT32       Size,
  OUT UINT8        *Out,
  IN  UINT32       OutSize
  )
{
  UINT32  In;
  UINT32  Length;
  UINT32  LineStart;
  UINT32  LineEnd;
  UINT32  Index;

  Length = 0;
  In     = 0;
  while (In < Size) {
    LineStart = In;
    while ((In < Size) && (Text[In] != '\n')) {
      In++;
    }
    LineEnd = In;
    if (In < Size) {
      In++;
    }
    while ((LineEnd > LineStart) && ((Text[LineEnd - 1] == '\r') || (Text[LineEnd - 1] == ' ') || (Text[LineEnd - 1] == '\t'))) {
      LineEnd--;
    }
    if ((LineEnd == LineStart) || (Text[LineStart] == '#')) {
      continue;
    }

    //
    // muxenab=0x10 (Samsung's file) moves the host interrupt to the out-of-
    // band WLAN_HOST_WAKE pin (GPX2(0)); without it the chip signals on
    // DAT1, which is what BcmSdioEnableInterrupts() uses.
    //
    if ((LineEnd - LineStart >= 8) && (CompareMem (Text + LineStart, "muxenab=", 8) == 0)) {
      DEBUG ((DEBUG_INFO, "Bcm: NVRAM muxenab dropped (in-band interrupt)\n"));
      continue;
    }
    if (Length + (LineEnd - LineStart) + 8 > OutSize) {
      return 0;
    }
    for (Index = LineStart; Index < LineEnd; Index++) {
      Out[Length++] = (UINT8)Text[Index];
    }
    Out[Length++] = 0;
  }
  Out[Length++] = 0;
  while ((Length & 3) != 0) {
    Out[Length++] = 0;
  }
  return Length;
}

EFI_STATUS
BcmSdioDownload (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST UINT8  *Firmware,
  IN     UINT32       FirmwareSize,
  IN     CONST CHAR8  *NvramText,
  IN     UINT32       NvramTextSize
  )
{
  EFI_STATUS  Status;
  UINT32      NvramSize;
  UINT32      Body;
  UINT32      VarAddr;
  UINT32      Token;
  UINT32      Words;
  UINT32      Shared;
  UINT8       Tail[4];
  UINT8       Word[4];
  UINTN       Try;

  //
  // NVRAM is built in RxBuf (free at this point).
  //
  NvramSize = BuildNvram (NvramText, NvramTextSize, Bcm->RxBuf, sizeof (Bcm->RxBuf));
  if ((NvramSize == 0) || (FirmwareSize < 4) || (FirmwareSize + NvramSize + 8 > Bcm->RamSize)) {
    DEBUG ((DEBUG_ERROR, "Bcm: firmware %u / NVRAM %u bytes do not fit in %u bytes of RAM\n",
            FirmwareSize, NvramSize, Bcm->RamSize));
    return EFI_BAD_BUFFER_SIZE;
  }
  DEBUG ((DEBUG_INFO, "Bcm: downloading %u bytes of firmware, %u bytes of NVRAM\n", FirmwareSize, NvramSize));

  CoreReset (Bcm, Bcm->Cr4->Wrap, SICF_CPUHALT, SICF_CPUHALT);

  //
  // The image: whole words from the caller's buffer, the last partial word
  // through a padded copy (the buffer may not be readable past its end).
  //
  Body   = FirmwareSize & ~3U;
  Status = BpMemory (Bcm, TRUE, Bcm->RamBase, (UINT8 *)Firmware, Body);
  if (!EFI_ERROR (Status) && (Body < FirmwareSize)) {
    ZeroMem (Tail, sizeof (Tail));
    CopyMem (Tail, Firmware + Body, FirmwareSize - Body);
    Status = BpMemory (Bcm, TRUE, Bcm->RamBase + Body, Tail, 4);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  VarAddr = Bcm->RamBase + (Bcm->RamSize - 4) - NvramSize;
  Status  = BpMemory (Bcm, TRUE, VarAddr, Bcm->RxBuf, NvramSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Words = NvramSize / 4;
  Token = (~Words << 16) | (Words & 0xFFFF);
  Put32Le (Word, Token);
  Status = BpMemory (Bcm, TRUE, Bcm->RamBase + Bcm->RamSize - 4, Word, 4);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Reset vector (first word of the image) to address 0, then run.
  //
  CopyMem (Word, Firmware, 4);
  Status = BpMemory (Bcm, TRUE, 0, Word, 4);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  CoreReset (Bcm, Bcm->Cr4->Wrap, 0, 0);

  Cmd52 (Bcm, 1, SB_FUNC1_CHIPCLKCSR, TRUE, SB_HT_AVAIL_REQ, NULL);
  Shared = Token;
  for (Try = 0; Try < 300; Try++) {
    gBS->Stall (10000);
    Shared = 0;
    if (!EFI_ERROR (BpMemory (Bcm, FALSE, Bcm->RamBase + Bcm->RamSize - 4, Word, 4))) {
      Shared = Get32Le (Word);
    }
    if ((Shared != Token) && (Shared != 0) && (((~Shared >> 16) & 0xFFFF) != (Shared & 0xFFFF))) {
      break;
    }
  }
  if ((Shared == Token) || (Shared == 0) || (((~Shared >> 16) & 0xFFFF) == (Shared & 0xFFFF))) {
    DEBUG ((DEBUG_ERROR, "Bcm: the firmware did not start (last RAM word 0x%08x)\n", Shared));
    return EFI_TIMEOUT;
  }
  Bcm->Shared = Shared;
  DEBUG ((DEBUG_INFO, "Bcm: firmware running, shared structure at 0x%08x after %u ms\n", Shared, (UINT32)(Try * 10)));
  return EFI_SUCCESS;
}

VOID
BcmLogConsole (
  IN OUT BCM_SDIO  *Bcm
  )
{
  UINT8   Sh[32];
  UINT8   Cons[24];
  UINT32  Buf;
  UINT32  Size;
  UINT32  Index;
  UINT32  Length;
  UINT32  Pos;
  UINT32  Chunk;
  CHAR8   Line[100];

  if ((Bcm->Shared == 0) || EFI_ERROR (BpMemory (Bcm, FALSE, Bcm->Shared, Sh, sizeof (Sh))) ||
      (Get32Le (&Sh[20]) == 0) || EFI_ERROR (BpMemory (Bcm, FALSE, Get32Le (&Sh[20]), Cons, sizeof (Cons)))) {
    return;
  }
  Buf  = Get32Le (&Cons[8]);
  Size = MIN (Get32Le (&Cons[12]), (UINT32)sizeof (Bcm->RxBuf));
  if ((Buf == 0) || (Size == 0)) {
    return;
  }
  for (Pos = 0; Pos < Size; Pos += Chunk) {
    Chunk = MIN (Size - Pos, (UINT32)2048) & ~3U;
    if ((Chunk == 0) || EFI_ERROR (BpMemory (Bcm, FALSE, Buf + Pos, Bcm->RxBuf + Pos, Chunk))) {
      break;
    }
  }
  Size   = Pos;
  Length = 0;
  for (Index = 0; Index < Size; Index++) {
    if ((Bcm->RxBuf[Index] == '\n') || (Length + 2 >= sizeof (Line))) {
      Line[Length] = '\0';
      if (Length > 0) {
        DEBUG ((DEBUG_INFO, "Bcm: fw | %a\n", Line));
      }
      Length = 0;
    } else if ((Bcm->RxBuf[Index] >= 0x20) && (Bcm->RxBuf[Index] < 0x7F)) {
      Line[Length++] = (CHAR8)Bcm->RxBuf[Index];
    }
  }
}

//
// ---------------------------------------------------------------------------
// SDPCM bus.
// ---------------------------------------------------------------------------
//

EFI_STATUS
BcmSdioBusInit (
  IN OUT BCM_SDIO  *Bcm
  )
{
  UINT8  Clk;
  UINTN  Try;
  UINT8  Ready;

  //
  // HT on the backplane so the F2 interrupt propagates (dhd_bus_init()).
  //
  Clk = Read8 (Bcm, 1, SB_FUNC1_CHIPCLKCSR);
  Cmd52 (Bcm, 1, SB_FUNC1_CHIPCLKCSR, TRUE, (UINT8)(Clk | SB_FORCE_HT), NULL);
  for (Try = 0; (Try < 100) && ((Read8 (Bcm, 1, SB_FUNC1_CHIPCLKCSR) & SB_HT_AVAIL) == 0); Try++) {
    gBS->Stall (1000);
  }

  Bp32Write (Bcm, Bcm->Sdiod->Base + SDIOD_TOSBMAILBOXDATA, SDPCM_PROT_VERSION << SMB_DATA_VERSION_SHIFT);
  Cmd52 (Bcm, 0, CCCR_IO_ENABLE, TRUE, B (1) | B (2), NULL);
  Ready = 0;
  for (Try = 0; Try < 3000; Try++) {
    Ready = Read8 (Bcm, 0, CCCR_IO_READY);
    if ((Ready & B (2)) != 0) {
      break;
    }
    gBS->Stall (1000);
  }
  DEBUG ((DEBUG_INFO, "Bcm: F2 %a after %u ms (IORDY 0x%02x), CHIPCLKCSR 0x%02x\n",
          (Ready & B (2)) ? "ready" : "NOT ready", (UINT32)Try, Ready, Read8 (Bcm, 1, SB_FUNC1_CHIPCLKCSR)));
  if ((Ready & B (2)) == 0) {
    Cmd52 (Bcm, 0, CCCR_IO_ENABLE, TRUE, B (1), NULL);
    return EFI_TIMEOUT;
  }
  Bp32Write (Bcm, Bcm->Sdiod->Base + SDIOD_HOSTINTMASK, I_HMB_SW_MASK);
  BcmSdioPoll (Bcm);
  return EFI_SUCCESS;
}

/**
  Send the frame whose payload is already at TxBuf + SDPCM_HDRLEN.
**/
STATIC
EFI_STATUS
SendPrepared (
  IN BCM_SDIO  *Bcm,
  IN UINT8     Channel,
  IN UINT32    PayloadLength
  )
{
  UINT32      Total;
  UINT32      Padded;
  UINT8       *F;
  EFI_STATUS  Status;

  Total = SDPCM_HDRLEN + PayloadLength;
  if (Total > sizeof (Bcm->TxBuf)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  Padded = (Total + 3) & ~3U;
  if (Padded > F2_BLOCK_SIZE) {
    Padded = (Padded + F2_BLOCK_SIZE - 1) & ~(F2_BLOCK_SIZE - 1);
  }
  if (Padded > sizeof (Bcm->TxBuf)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  ZeroMem (Bcm->TxBuf + Total, Padded - Total);

  F    = Bcm->TxBuf;
  F[0] = (UINT8)Total;
  F[1] = (UINT8)(Total >> 8);
  F[2] = (UINT8)~Total;
  F[3] = (UINT8)(~Total >> 8);
  F[4] = Bcm->TxSeq;
  F[5] = Channel & 0x0F;
  F[6] = 0;
  F[7] = SDPCM_HDRLEN;
  F[8] = 0;
  F[9] = 0;
  F[10] = 0;
  F[11] = 0;

  Status = Cmd53 (Bcm, 2, 0, TRUE, F, Padded);
  if (!EFI_ERROR (Status)) {
    Bcm->TxSeq++;
  }
  return Status;
}

STATIC
BOOLEAN
HaveCredit (
  IN BCM_SDIO  *Bcm
  )
{
  UINT8  Room;

  Room = (UINT8)(Bcm->TxMax - Bcm->TxSeq);
  return (Room != 0) && ((Room & 0x80) == 0);
}

EFI_STATUS
BcmSdioSend (
  IN OUT BCM_SDIO     *Bcm,
  IN     UINT8        Channel,
  IN     CONST UINT8  *Payload,
  IN     UINT32       Length
  )
{
  UINTN  Try;

  if (SDPCM_HDRLEN + Length > sizeof (Bcm->TxBuf)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  for (Try = 0; (Try < 20) && !HaveCredit (Bcm); Try++) {
    BcmSdioPoll (Bcm);
    if (!HaveCredit (Bcm)) {
      gBS->Stall (500);
    }
  }
  if (!HaveCredit (Bcm)) {
    return EFI_NOT_READY;
  }
  CopyMem (Bcm->TxBuf + SDPCM_HDRLEN, Payload, Length);
  return SendPrepared (Bcm, Channel, Length);
}

/**
  One frame off F2 into RxBuf.  EFI_NOT_FOUND when the dongle has none.
**/
STATIC
EFI_STATUS
ReadFrame (
  IN  BCM_SDIO  *Bcm,
  OUT UINT32    *FrameLength
  )
{
  UINT32      Len;
  UINT32      Check;
  UINT32      Rest;
  EFI_STATUS  Status;

  Status = Cmd53 (Bcm, 2, 0, FALSE, Bcm->RxBuf, FIRSTREAD);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Len   = Bcm->RxBuf[0] | ((UINT32)Bcm->RxBuf[1] << 8);
  Check = Bcm->RxBuf[2] | ((UINT32)Bcm->RxBuf[3] << 8);
  if ((Len | Check) == 0) {
    return EFI_NOT_FOUND;
  }
  if ((UINT16)~(Len ^ Check) != 0) {
    DEBUG ((DEBUG_INFO, "Bcm: bad frame tag 0x%04x 0x%04x\n", Len, Check));
    Bcm->Errors++;
    return EFI_CRC_ERROR;
  }
  if ((Len < SDPCM_HDRLEN) || (Len > sizeof (Bcm->RxBuf))) {
    Bcm->Errors++;
    return EFI_BAD_BUFFER_SIZE;
  }
  if (Len > FIRSTREAD) {
    //
    // The rest, rounded like dhdsdio_readframes(): to 32 bytes, or to whole
    // blocks above one block.
    //
    Rest = Len - FIRSTREAD;
    if (Rest > F2_BLOCK_SIZE) {
      Rest = (Rest + F2_BLOCK_SIZE - 1) & ~(F2_BLOCK_SIZE - 1);
    } else {
      Rest = (Rest + RXALIGN - 1) & ~(RXALIGN - 1);
    }
    if (FIRSTREAD + Rest > sizeof (Bcm->RxBuf)) {
      return EFI_BAD_BUFFER_SIZE;
    }
    Status = Cmd53 (Bcm, 2, 0, FALSE, Bcm->RxBuf + FIRSTREAD, Rest);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  *FrameLength = Len;
  return EFI_SUCCESS;
}

STATIC
VOID
Dispatch (
  IN BCM_SDIO  *Bcm,
  IN UINT8     *Buf,
  IN UINT32    Len
  )
{
  UINT8   *H;
  UINT8   Channel;
  UINT8   DataOffset;
  UINT8   TxMax;
  UINT8   *P;
  UINT32  PLen;
  UINT32  Flags;
  UINT32  BdcOffset;

  H          = Buf + SDPCM_FRAMETAG_LEN;
  Channel    = H[1] & 0x0F;
  DataOffset = H[3];
  TxMax      = H[5];
  if ((DataOffset < SDPCM_HDRLEN) || (DataOffset > Len)) {
    Bcm->Errors++;
    return;
  }
  if ((UINT8)(TxMax - Bcm->TxSeq) <= 0x70) {
    Bcm->TxMax = TxMax;
  }
  Bcm->Frames++;
  Bcm->WpaSeed[Bcm->Frames % sizeof (Bcm->WpaSeed)] ^= (UINT8)GetPerformanceCounter ();
  P    = Buf + DataOffset;
  PLen = Len - DataOffset;

  if (Channel == BCM_CHANNEL_GLOM) {
    //
    // Glom descriptor: little-endian 16-bit lengths of the frames that the
    // next read brings as one superframe.
    //
    if ((H[1] & 0x80) != 0) {
      for (Bcm->GlomCount = 0; (Bcm->GlomCount < BCM_GLOM_FRAMES) && (2 * Bcm->GlomCount + 1 < PLen); Bcm->GlomCount++) {
        Bcm->GlomLen[Bcm->GlomCount] = (UINT16)(P[2 * Bcm->GlomCount] | (P[2 * Bcm->GlomCount + 1] << 8));
      }
    }
    return;
  }
  if (Channel == BCM_CHANNEL_CONTROL) {
    if (PLen < CDC_HDRLEN) {
      return;
    }
    Flags = Get32Le (P + 8);
    if ((Flags >> CDCF_IOC_ID_SHIFT) == Bcm->IoctlId) {
      Bcm->IoctlStatus = (Flags & CDCF_IOC_ERROR) ? Get32Le (P + 12) : 0;   // BCME_xxx (< 0)
      Bcm->IoctlFailed = (Flags & CDCF_IOC_ERROR) != 0;
      Bcm->IoctlLength = MIN (PLen - CDC_HDRLEN, (UINT32)sizeof (Bcm->IoctlBuf));
      CopyMem (Bcm->IoctlBuf, P + CDC_HDRLEN, Bcm->IoctlLength);
      Bcm->IoctlDone = TRUE;
    }
    return;
  }
  if ((Channel == BCM_CHANNEL_EVENT) || (Channel == BCM_CHANNEL_DATA)) {
    if (PLen < BDC_HDRLEN) {
      return;
    }
    BdcOffset = BDC_HDRLEN + (UINT32)P[3] * 4;
    if (BdcOffset > PLen) {
      return;
    }
    //
    // EAPOL (802.1X) frames are the supplicant's: queued for BcmWpaService().
    //
    if ((Channel == BCM_CHANNEL_DATA) && Bcm->WpaActive && (PLen - BdcOffset >= 14) &&
        (P[BdcOffset + 12] == 0x88) && (P[BdcOffset + 13] == 0x8E))
    {
      if (PLen - BdcOffset <= sizeof (Bcm->Eapol)) {
        Bcm->EapolLength = PLen - BdcOffset;
        CopyMem (Bcm->Eapol, P + BdcOffset, Bcm->EapolLength);
      }
      return;
    }
    if (Bcm->RxHandler != NULL) {
      Bcm->RxHandler (Bcm->RxContext, Channel, P + BdcOffset, PLen - BdcOffset);
    }
  }
}

/**
  Drop the frame being read and get back in step with the dongle
  (dhdsdio_rxfail()): abort F2, terminate the frame, wait for the read
  frame byte count to drain; with Retransmit, NAK it to the dongle.
**/
STATIC
VOID
RxFail (
  IN OUT BCM_SDIO  *Bcm,
  IN     BOOLEAN   Retransmit
  )
{
  UINTN  Try;

  Cmd52 (Bcm, 0, CCCR_IO_ABORT, TRUE, 2, NULL);
  Cmd52 (Bcm, 1, SB_FUNC1_FRAMECTRL, TRUE, SFC_RF_TERM, NULL);
  for (Try = 0; Try < 1000; Try++) {
    if ((Read8 (Bcm, 1, SB_FUNC1_RFRAMEBCHI) == 0) && (Read8 (Bcm, 1, SB_FUNC1_RFRAMEBCLO) == 0)) {
      break;
    }
  }
  if (Retransmit) {
    Bp32Write (Bcm, Bcm->Sdiod->Base + SDIOD_TOSBMAILBOX, SMB_NAK);
  }
  Bcm->GlomCount = 0;
  Bcm->RxFails++;
  if (Bcm->RxFails <= 20) {
    DEBUG ((DEBUG_INFO, "Bcm: rx resync #%u (%u polls to drain)\n", Bcm->RxFails, (UINT32)Try));
  }
}

/**
  The superframe announced by a glom descriptor: one read of the summed
  lengths, the superframe header (channel 3), then the frames, each at
  the offset the descriptor gives (the first one after the superframe
  header).
**/
STATIC
EFI_STATUS
ReadGlom (
  IN OUT BCM_SDIO  *Bcm
  )
{
  UINT32      Total;
  UINT32      Read;
  UINT32      I;
  UINT32      Start;
  UINT32      End;
  UINT32      Sub;
  UINT32      Count;
  UINT8       *S;
  EFI_STATUS  Status;

  Count = Bcm->GlomCount;
  Bcm->GlomCount = 0;
  for (Total = 0, I = 0; I < Count; I++) {
    Total += Bcm->GlomLen[I];
  }
  if ((Total < SDPCM_FRAMETAG_LEN + SDPCM_HDRLEN) || (Total > sizeof (Bcm->GlomBuf))) {
    DEBUG ((DEBUG_INFO, "Bcm: glom of %u frames, %u bytes - not handled\n", Count, Total));
    return EFI_BAD_BUFFER_SIZE;
  }
  Read = (Total > F2_BLOCK_SIZE) ? ((Total + F2_BLOCK_SIZE - 1) & ~(F2_BLOCK_SIZE - 1))
                                 : ((Total + RXALIGN - 1) & ~(RXALIGN - 1));
  if (Read > sizeof (Bcm->GlomBuf)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  Status = Cmd53 (Bcm, 2, 0, FALSE, Bcm->GlomBuf, Read);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  S = Bcm->GlomBuf;
  if (((UINT16)~((S[0] | (S[1] << 8)) ^ (S[2] | (S[3] << 8))) != 0) ||
      ((S[SDPCM_FRAMETAG_LEN + 1] & 0x0F) != BCM_CHANNEL_GLOM) || (S[SDPCM_FRAMETAG_LEN + 3] < SDPCM_HDRLEN))
  {
    DEBUG ((DEBUG_INFO, "Bcm: bad superframe header 0x%02x%02x%02x%02x ch 0x%02x\n", S[0], S[1], S[2], S[3],
            S[SDPCM_FRAMETAG_LEN + 1]));
    return EFI_CRC_ERROR;
  }
  if ((UINT8)(S[SDPCM_FRAMETAG_LEN + 5] - Bcm->TxSeq) <= 0x70) {
    Bcm->TxMax = S[SDPCM_FRAMETAG_LEN + 5];
  }
  Bcm->Gloms++;

  for (Start = S[SDPCM_FRAMETAG_LEN + 3], End = 0, I = 0; I < Count; I++, Start = End) {
    End += Bcm->GlomLen[I];
    if ((End > Total) || (Start + SDPCM_FRAMETAG_LEN + SDPCM_HDRLEN > End)) {
      break;
    }
    Sub = S[Start] | ((UINT32)S[Start + 1] << 8);
    if (((UINT16)~(Sub ^ (S[Start + 2] | ((UINT32)S[Start + 3] << 8))) != 0) || (Sub > End - Start) ||
        (Sub < SDPCM_FRAMETAG_LEN + SDPCM_HDRLEN))
    {
      DEBUG ((DEBUG_INFO, "Bcm: glom frame %u/%u bad tag\n", I, Count));
      break;
    }
    Dispatch (Bcm, S + Start, Sub);
  }
  return EFI_SUCCESS;
}

UINT8
BcmSdioIntPending (
  IN OUT BCM_SDIO  *Bcm
  )
{
  return Read8 (Bcm, 0, 0x05);
}

UINT32
BcmSdioPoll (
  IN OUT BCM_SDIO  *Bcm
  )
{
  UINT32      Int;
  UINT32      Mail;
  UINT32      Len;
  UINT32      Count;
  EFI_STATUS  Status;

  Int = Bp32 (Bcm, Bcm->Sdiod->Base + SDIOD_INTSTATUS);
  if ((Int == 0) || (Int == 0xFFFFFFFFU)) {
    return 0;
  }
  Bp32Write (Bcm, Bcm->Sdiod->Base + SDIOD_INTSTATUS, Int);

  if ((Int & I_HMB_HOST_INT) != 0) {
    Mail = Bp32 (Bcm, Bcm->Sdiod->Base + SDIOD_TOHOSTMAILBOXDATA);
    Bp32Write (Bcm, Bcm->Sdiod->Base + SDIOD_TOSBMAILBOX, SMB_INT_ACK);
    DEBUG ((DEBUG_INFO, "Bcm: mailbox 0x%08x%a%a%a\n", Mail,
            (Mail & HMB_DATA_DEVREADY) ? " devready" : "", (Mail & HMB_DATA_FWREADY) ? " fwready" : "",
            (Mail & HMB_DATA_FWHALT) ? " FIRMWARE HALTED" : ""));
  }

  Count = 0;
  if ((Int & I_HMB_FRAME_IND) != 0) {
    for (Count = 0; Count < 64; Count++) {
      if (Bcm->GlomCount != 0) {
        if (EFI_ERROR (ReadGlom (Bcm))) {
          RxFail (Bcm, TRUE);
          break;
        }
        continue;
      }
      Status = ReadFrame (Bcm, &Len);
      if (Status == EFI_NOT_FOUND) {
        break;
      }
      if (EFI_ERROR (Status)) {
        RxFail (Bcm, TRUE);
        break;
      }
      Dispatch (Bcm, Bcm->RxBuf, Len);
    }
  }
  return Count;
}

//
// ---------------------------------------------------------------------------
// BCDC control.
// ---------------------------------------------------------------------------
//

EFI_STATUS
BcmIoctl (
  IN OUT BCM_SDIO  *Bcm,
  IN     UINT32    Cmd,
  IN     BOOLEAN   Set,
  IN OUT VOID      *Buffer,
  IN     UINT32    Length,
  IN     UINT32    TimeoutMs
  )
{
  UINT8       *P;
  UINT32      Waited;
  UINTN       Try;
  EFI_STATUS  Status;

  if (Length > BCM_IOCTL_MAX) {
    return EFI_BAD_BUFFER_SIZE;
  }
  for (Try = 0; (Try < 20) && !HaveCredit (Bcm); Try++) {
    BcmSdioPoll (Bcm);
    gBS->Stall (500);
  }

  Bcm->IoctlId++;
  Bcm->IoctlDone   = FALSE;
  Bcm->IoctlFailed = FALSE;
  P = Bcm->TxBuf + SDPCM_HDRLEN;
  Put32Le (P + 0, Cmd);
  Put32Le (P + 4, Length);
  Put32Le (P + 8, ((UINT32)Bcm->IoctlId << CDCF_IOC_ID_SHIFT) | (Set ? CDCF_IOC_SET : 0));
  Put32Le (P + 12, 0);
  if (Length > 0) {
    CopyMem (P + CDC_HDRLEN, Buffer, Length);
  }
  Status = SendPrepared (Bcm, BCM_CHANNEL_CONTROL, CDC_HDRLEN + Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Waited = 0; Waited < TimeoutMs; Waited++) {
    BcmSdioPoll (Bcm);
    if (Bcm->IoctlDone) {
      break;
    }
    gBS->Stall (1000);
  }
  if (!Bcm->IoctlDone) {
    DEBUG ((DEBUG_ERROR, "Bcm: ioctl %u timed out\n", Cmd));
    return EFI_TIMEOUT;
  }
  if (Bcm->IoctlFailed) {
    DEBUG ((DEBUG_INFO, "Bcm: ioctl %u failed, BCME %d\n", Cmd, (INT32)Bcm->IoctlStatus));
    return EFI_DEVICE_ERROR;
  }
  if (!Set && (Length > 0)) {
    CopyMem (Buffer, Bcm->IoctlBuf, MIN (Length, Bcm->IoctlLength));
  }
  return EFI_SUCCESS;
}

EFI_STATUS
BcmIovarGet (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Name,
  OUT    VOID         *Value,
  IN     UINT32       Length
  )
{
  UINT8       Buf[BCM_IOCTL_MAX];
  UINT32      NameLen;
  EFI_STATUS  Status;

  NameLen = StrLen8 (Name) + 1;
  if (MAX (NameLen, Length) > sizeof (Buf)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  ZeroMem (Buf, MAX (NameLen, Length));
  CopyMem (Buf, Name, NameLen);
  Status = BcmIoctl (Bcm, WLC_GET_VAR, FALSE, Buf, MAX (NameLen, Length), 1000);
  if (!EFI_ERROR (Status)) {
    CopyMem (Value, Buf, Length);
  }
  return Status;
}

EFI_STATUS
BcmIovarSet (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Name,
  IN     CONST VOID   *Value,
  IN     UINT32       Length
  )
{
  UINT8   Buf[BCM_IOCTL_MAX];
  UINT32  NameLen;

  NameLen = StrLen8 (Name) + 1;
  if (NameLen + Length > sizeof (Buf)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  CopyMem (Buf, Name, NameLen);
  CopyMem (Buf + NameLen, Value, Length);
  return BcmIoctl (Bcm, WLC_SET_VAR, TRUE, Buf, NameLen + Length, 1000);
}

EFI_STATUS
BcmIovarSetInt (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Name,
  IN     UINT32       Value
  )
{
  UINT8  V[4];

  Put32Le (V, Value);
  return BcmIovarSet (Bcm, Name, V, 4);
}

EFI_STATUS
BcmIoctlSetInt (
  IN OUT BCM_SDIO  *Bcm,
  IN     UINT32    Cmd,
  IN     UINT32    Value
  )
{
  UINT8  V[4];

  Put32Le (V, Value);
  return BcmIoctl (Bcm, Cmd, TRUE, V, 4, 1000);
}

//
// SET_SSID, JOIN, AUTH, DEAUTH, DEAUTH_IND, ASSOC, DISASSOC, DISASSOC_IND,
// LINK, PSK_SUP.
//
STATIC CONST UINT8  mEvents[] = { 0, 1, 3, 5, 6, 7, 11, 12, 16, 46 };

EFI_STATUS
BcmJoin (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Ssid,
  IN     CONST CHAR8  *Passphrase  OPTIONAL
  )
{
  UINT8       SsidBuf[36];
  UINT8       Events[16];               // WL_EVENTING_MASK_LEN
  UINT32      Len;
  UINTN       I;
  EFI_STATUS  Status;

  //
  // No in-firmware supplicant ("idsup") in this firmware: it associates
  // with an RSN IE of its own making and hands the EAPOL frames to us.
  //
  Len = MIN (StrLen8 (Ssid), (UINT32)32);
  Bcm->WpaActive = FALSE;

  //
  // The firmware reports nothing until told which events the host wants
  // (dhd_preinit_ioctls() does the same with "event_msgs").
  //
  ZeroMem (Events, sizeof (Events));
  BcmIovarGet (Bcm, "event_msgs", Events, sizeof (Events));
  for (I = 0; I < ARRAY_SIZE (mEvents); I++) {
    Events[mEvents[I] / 8] |= (UINT8)(1u << (mEvents[I] % 8));
  }
  Status = BcmIovarSet (Bcm, "event_msgs", Events, sizeof (Events));
  DEBUG ((DEBUG_INFO, "Bcm: event mask set - %r\n", Status));

  //
  // One frame per read (glom descriptors are still handled if it does).
  //
  Status = BcmIovarSetInt (Bcm, "bus:rxglom", 0);
  DEBUG ((DEBUG_INFO, "Bcm: bus:rxglom 0 - %r\n", Status));

  BcmIoctlSetInt (Bcm, WLC_SET_INFRA, 1);
  BcmIoctlSetInt (Bcm, WLC_SET_AUTH, 0);
  if ((Passphrase != NULL) && (Passphrase[0] != '\0')) {
    Status = BcmWpaSetup (Bcm, Ssid, Len, Passphrase);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    BcmIoctlSetInt (Bcm, WLC_SET_WSEC, AES_ENABLED | TKIP_ENABLED);
    BcmIoctlSetInt (Bcm, WLC_SET_WPA_AUTH, WPA2_AUTH_PSK);
  } else {
    BcmIoctlSetInt (Bcm, WLC_SET_WSEC, 0);
    BcmIoctlSetInt (Bcm, WLC_SET_WPA_AUTH, 0);
  }
  ZeroMem (SsidBuf, sizeof (SsidBuf));
  Put32Le (SsidBuf, Len);
  CopyMem (&SsidBuf[4], Ssid, Len);
  return BcmIoctl (Bcm, WLC_SET_SSID, TRUE, SsidBuf, sizeof (SsidBuf), 1000);
}

//
// ---------------------------------------------------------------------------
// Card interrupt (in-band, DAT1).
// ---------------------------------------------------------------------------
//

VOID
BcmSdioEnableInterrupts (
  IN OUT BCM_SDIO  *Bcm
  )
{
  //
  // Card side: master enable + functions 1 and 2 (CCCR Int Enable).  Host
  // side: the SDIO interrupt into the controller's interrupt line.
  //
  Cmd52 (Bcm, 0, CCCR_INT_ENABLE, TRUE, B (0) | B (1) | B (2), NULL);
  MmioWrite32 (Bcm->Host.Base + DW_RINTSTS, RINT_SDIO);
  MmioOr32 (Bcm->Host.Base + DW_INTMASK, RINT_SDIO);
  DEBUG ((DEBUG_INFO, "Bcm: card interrupt enabled (CCCR IEN 0x%02x, INTMASK 0x%08x)\n",
          Read8 (Bcm, 0, CCCR_INT_ENABLE), MmioRead32 (Bcm->Host.Base + DW_INTMASK)));
}

BOOLEAN
BcmSdioInterruptIsr (
  IN UINT32  HostBase
  )
{
  //
  // ISR context: only controller registers.  Ours if the masked status has
  // the SDIO bit; mask it until the chip has been drained.
  //
  if ((MmioRead32 (HostBase + DW_MINTSTS) & RINT_SDIO) == 0) {
    return FALSE;
  }
  MmioAnd32 (HostBase + DW_INTMASK, ~RINT_SDIO);
  return TRUE;
}

VOID
BcmSdioInterruptDone (
  IN OUT BCM_SDIO  *Bcm
  )
{
  //
  // Acknowledge and unmask.  If the card still holds DAT1 low the bit
  // latches again and the interrupt comes straight back.
  //
  MmioWrite32 (Bcm->Host.Base + DW_RINTSTS, RINT_SDIO);
  MmioOr32 (Bcm->Host.Base + DW_INTMASK, RINT_SDIO);
}
