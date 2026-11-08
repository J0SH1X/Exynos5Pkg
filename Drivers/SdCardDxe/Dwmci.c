/** @file
  Synopsys DesignWare MMC ("dw_mmc") host backend for Exynos5420.

  Polled; data by the internal DMA (IDMAC, 32-bit chained descriptors)
  through a bounce buffer.  Derived from Exynos441xPkg's Mshci.c (same IP,
  older revision), with the Exynos5420 differences from the downstream
  kernel (drivers/mmc/host/dw_mmc.c, arch/arm/mach-exynos/dev-dwmci.c):

    - CPU PIO through the FIFO data port does not work on channel 0 (the
      reads never pop the FIFO: TBBCNT stays 0 with TCBCNT complete), so
      data always goes through the IDMAC, as in the kernel.
    - The bootloader leaves UHS_REG in DDR mode; it is cleared for SDR.
    - Samsung's CLKSEL (0x9C) holds the internal CIU clock divider and the
      sample/drive phases (pdata sdr_timing).
    - Commands set USE_HOLD_REG (the Exynos pdata has set_io_timing).
    - Channels 0/1 have an eMMCP security unit in front of the FIFO that
      must be put in bypass (exynos_dwmci_cfg_smu).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MmcHost.h"

//
// Register offsets (dw_mmc.h).
//
#define DWMCI_CTRL     0x00
#define DWMCI_PWREN    0x04
#define DWMCI_CLKDIV   0x08
#define DWMCI_CLKSRC   0x0C
#define DWMCI_CLKENA   0x10
#define DWMCI_TMOUT    0x14
#define DWMCI_CTYPE    0x18
#define DWMCI_BLKSIZ   0x1C
#define DWMCI_BYTCNT   0x20
#define DWMCI_INTMSK   0x24
#define DWMCI_CMDARG   0x28
#define DWMCI_CMD      0x2C
#define DWMCI_RESP0    0x30
#define DWMCI_MINTSTS  0x40
#define DWMCI_RINTSTS  0x44

//
// RINTSTS bit 16: the SDIO card's interrupt (DAT1), owned by an SDIO
// client's interrupt handling (exynoswlan).  The command / data paths clear
// their own status bits only, so a pending card interrupt survives them.
//
#define INT_SDIO0          (1U << 16)
#define RINTSTS_ACK(x)     ((x) & ~INT_SDIO0)
#define DWMCI_STATUS   0x48
#define DWMCI_FIFOTH   0x4C
#define DWMCI_TCBCNT   0x5C
#define DWMCI_TBBCNT   0x60
#define DWMCI_VERID    0x6C
#define DWMCI_UHS_REG  0x74
#define DWMCI_BMOD     0x80
#define DWMCI_PLDMND   0x84
#define DWMCI_DBADDR   0x88
#define DWMCI_IDSTS    0x8C
#define DWMCI_IDINTEN  0x90
#define DWMCI_CLKSEL   0x9C
#define DWMCI_CDTHRCTL 0x100

//
// Exynos5420 HS400 ("DDR200") block: read-strobe enable, async FIFO, delay
// line.  The kernel uses 0x110/0x114/0x118 + 0x70 on this SoC.
//
#define DWMCI_DDR200_RDDQS_EN     0x180
#define DWMCI_DDR200_ASYNC_FIFO   0x184
#define DWMCI_DDR200_DLINE_CTRL   0x188
#define RDDQS_TXDT_CRC_FASTLIMIT(x)  (((x) & 0xFF) << 16)
#define RDDQS_TXDT_CRC_INITVAL(x)    (((x) & 0xFF) << 8)
#define RDDQS_RESP_RCLK_MODE         (1U << 5)
#define RDDQS_RDDQS_EN               (1U << 0)
#define RDDQS_DEF  (RDDQS_TXDT_CRC_FASTLIMIT (0x12) | RDDQS_TXDT_CRC_INITVAL (0x14))
#define DLINE_FIFO_CLK_DELAY(x)      (((x) & 0x3) << 16)
#define DLINE_RD_DQS_DELAY(x)        ((x) & 0x3FF)
#define DLINE_DEFAULT   (DLINE_FIFO_CLK_DELAY (2) | DLINE_RD_DQS_DELAY (0x40))
#define DQS_DELAY_V1A   110      // board value (v1a: RD_DQS_DELAY_CTRL(110))

//
// CLKSEL (board-universal5420-mmc.c).
//
#define CLKSEL_SDR       0x03040000U
#define CLKSEL_DDR200    0x01020002U
#define CLKSEL_DDR52     0x03020000U
#define CLKSEL_HS200     0x03030000U       // sdr timing with clk_drv 3 (board pdata)
#define UHS_DDR_SLOT0    (1U << 16)

//
// IDMAC.
//
#define CTRL_USE_IDMAC     (1U << 25)
#define BMOD_SWRESET       (1U << 0)
#define BMOD_FB            (1U << 1)
#define BMOD_ENABLE        (1U << 7)
#define IDSTS_TI           (1U << 0)
#define IDSTS_RI           (1U << 1)
#define IDSTS_FBE          (1U << 2)
#define IDSTS_DU           (1U << 4)
#define IDSTS_CES          (1U << 5)
#define IDSTS_ERR          (IDSTS_FBE | IDSTS_DU | IDSTS_CES)
#define IDSTS_ALL          0x3FF

#define DES0_DIC           (1U << 1)
#define DES0_LD            (1U << 2)
#define DES0_FD            (1U << 3)
#define DES0_CH            (1U << 4)
#define DES0_OWN           (1U << 31)
#define DESC_WORDS         4
#define DESC_BUF_BYTES     4096
#define DESC_COUNT         (MMC_MAX_DMA_BYTES / DESC_BUF_BYTES)

//
// eMMCP security unit (channels 0/1).
//
#define DWMCI_MPSBEGIN0  0x1200
#define DWMCI_MPSEND0    0x1204
#define DWMCI_MPSCTRL0   0x120C
#define MPSCTRL_BYPASS   ((1U << 7) | (1U << 6) | (1U << 5) | (1U << 4) | (1U << 0))

//
// CTRL bits.
//
#define CTRL_RESET       (1U << 0)
#define CTRL_FIFO_RESET  (1U << 1)
#define CTRL_DMA_RESET   (1U << 2)
#define CTRL_ALL_RESET   (CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)
#define CTRL_INT_ENABLE  (1U << 4)

//
// CMD register bits.
//
#define CMD_RESP_EXP      (1U << 6)
#define CMD_RESP_LEN      (1U << 7)
#define CMD_CHECK_CRC     (1U << 8)
#define CMD_DATA_EXP      (1U << 9)
#define CMD_RW            (1U << 10)  // 1 = write (host->card)
#define CMD_WAIT_PRV_DAT  (1U << 13)
#define CMD_STOP_ABORT    (1U << 14)
#define CMD_SEND_INIT     (1U << 15)  // 80 init clocks before the command
#define CMD_SEND_CLK_ONLY (1U << 21)  // update-clock: no card command
#define CMD_USE_HOLD_REG  (1U << 29)
#define CMD_START         (1U << 31)

#define CMD_ONLY_CLK   (CMD_START | CMD_SEND_CLK_ONLY | CMD_WAIT_PRV_DAT)

//
// RINTSTS bits.
//
#define INT_RE     (1U << 1)
#define INT_CDONE  (1U << 2)
#define INT_DTO    (1U << 3)
#define INT_RCRC   (1U << 6)
#define INT_DCRC   (1U << 7)
#define INT_RTO    (1U << 8)
#define INT_DRTO   (1U << 9)
#define INT_HTO    (1U << 10)
#define INT_HLE    (1U << 12)
#define INT_SBE    (1U << 13)
#define INT_EBE    (1U << 15)

#define STATUS_DATA_BUSY  (1U << 9)   // card holds DAT0 low (busy)

#define INT_ERR    (INT_RE | INT_RCRC | INT_DCRC | INT_RTO | INT_DRTO | \
                    INT_HTO | INT_HLE | INT_SBE | INT_EBE)

//
// FIFO depth in 32-bit words (pdata fifo_depth = 0x40).
//
#define DWMCI_FIFO_DEPTH  0x40



UINT32
DwmciRead (
  IN MMC_HOST  *Host,
  IN UINT32    Offset
  )
{
  return MmioRead32 (Host->Base + Offset);
}

VOID
DwmciWrite (
  IN MMC_HOST  *Host,
  IN UINT32    Offset,
  IN UINT32    Value
  )
{
  MmioWrite32 (Host->Base + Offset, Value);
}

//
// Interrupts the command / data engine waits for (the SDIO card interrupt is
// not among them: an SDIO client owns it).
//
#define INT_WANTED  (INT_CDONE | INT_DTO | INT_ERR)

/**
  Interrupt service routine: latch what the controller reports into
  Host->IrqStatus (waiters look there), acknowledge it.  The SDIO card
  interrupt is left pending for its owner.

  @retval TRUE   The controller had something for us.
**/
BOOLEAN
DwmciIsr (
  IN MMC_HOST  *Host
  )
{
  UINT32  Mint;
  UINT32  IdSts;

  Mint  = MmioRead32 (Host->Base + DWMCI_MINTSTS) & INT_WANTED;
  IdSts = MmioRead32 (Host->Base + DWMCI_IDSTS) & IDSTS_ALL;
  if ((Mint == 0) && (IdSts == 0)) {
    return FALSE;
  }
  Host->IrqStatus |= Mint;
  Host->IrqIdSts  |= IdSts;
  Host->IrqCount++;
  if (Mint != 0) {
    MmioWrite32 (Host->Base + DWMCI_RINTSTS, Mint);
  }
  if (IdSts != 0) {
    MmioWrite32 (Host->Base + DWMCI_IDSTS, IdSts);
  }
  return TRUE;
}

VOID
DwmciIrqEnable (
  IN MMC_HOST  *Host,
  IN BOOLEAN   Enable
  )
{
  if (Enable) {
    Host->IrqStatus = 0;
    Host->IrqIdSts  = 0;
    MmioWrite32 (Host->Base + DWMCI_RINTSTS, RINTSTS_ACK (0xFFFFFFFFU));
    MmioWrite32 (Host->Base + DWMCI_IDSTS, IDSTS_ALL);
    MmioWrite32 (Host->Base + DWMCI_IDINTEN, 0x337);    // NI AI TI RI FBE DU CES
    MmioWrite32 (Host->Base + DWMCI_INTMSK, INT_WANTED);
    Host->IrqMode = TRUE;
  } else {
    Host->IrqMode = FALSE;
    MmioWrite32 (Host->Base + DWMCI_INTMSK, 0);
    MmioWrite32 (Host->Base + DWMCI_IDINTEN, 0);
  }
}

/**
  Wait until one of Mask shows up in the controller's raw status.  Polled
  hosts read RINTSTS (the caller acknowledges); interrupt-driven hosts wait
  for the ISR to latch it, with a safety poll in case an interrupt is lost
  (counted in IrqMissed).

  @param[out] Int   Status bits seen (RINTSTS layout).
  @retval TRUE      A wanted bit arrived within TimeoutUs.
**/
STATIC
BOOLEAN
WaitStatus (
  IN  MMC_HOST  *Host,
  IN  UINT32    Mask,
  IN  UINTN     TimeoutUs,
  OUT UINT32    *Int
  )
{
  UINTN   Waited;
  UINT32  Now;

  for (Waited = 0; Waited < TimeoutUs; ) {
    if (Host->IrqMode) {
      Now = Host->IrqStatus;
      if ((Now & Mask) != 0) {
        *Int = Now;
        return TRUE;
      }
      if (Host->WaitIrq != NULL) {
        Host->WaitIrq (Host, 100);
      } else {
        gBS->Stall (10);
      }
      Waited += 10;
      //
      // Lost-interrupt safety: after ~5 ms of silence look at the hardware.
      //
      if ((Waited % 5000) == 0) {
        if ((MmioRead32 (Host->Base + DWMCI_RINTSTS) & INT_WANTED & Mask) != 0) {
          Host->IrqMissed++;
          DwmciIsr (Host);
        }
      }
    } else {
      Now = MmioRead32 (Host->Base + DWMCI_RINTSTS);
      if ((Now & Mask) != 0) {
        *Int = Now;
        return TRUE;
      }
      gBS->Stall (10);
      Waited += 10;
    }
  }
  *Int = Host->IrqMode ? Host->IrqStatus : MmioRead32 (Host->Base + DWMCI_RINTSTS);
  return FALSE;
}

STATIC
VOID
AckStatus (
  IN MMC_HOST  *Host,
  IN UINT32    Bits
  )
{
  if (Host->IrqMode) {
    Host->IrqStatus &= ~Bits;
  } else {
    MmioWrite32 (Host->Base + DWMCI_RINTSTS, RINTSTS_ACK (Bits));
  }
}

STATIC
BOOLEAN
WaitCmdIdle (
  IN MMC_HOST  *Host,
  IN UINTN     TimeoutMs
  )
{
  UINTN  Retry;

  for (Retry = 0; Retry < TimeoutMs * 10; Retry++) {
    if (!(MmioRead32 (Host->Base + DWMCI_CMD) & CMD_START)) {
      return TRUE;
    }
    gBS->Stall (100);
  }
  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
DwmciSendCmd (
  IN MMC_HOST  *Host,
  IN UINT32    Cmd,
  IN UINT32    Arg,
  IN UINT32    Flags,
  OUT UINT32   *Resp
  )
{
  UINTN   Retry;
  UINT32  CmdReg;
  UINT32  Int;

  if (!WaitCmdIdle (Host, 1000)) {
    DEBUG ((DEBUG_ERROR, "Dwmci %x: CMD_START never released\n", Host->Base));
    return EFI_TIMEOUT;
  }

  //
  // After a write or an R1b command the card keeps DAT0 low while it
  // programs; anything but CMD12/CMD13 sent then is illegal for it and
  // simply not answered.  Wait it out (up to 1 s).
  //
  if ((Cmd != 0) && (Cmd != 12) && (Cmd != 13)) {
    for (Retry = 0; Retry < 10000; Retry++) {
      if (!(MmioRead32 (Host->Base + DWMCI_STATUS) & STATUS_DATA_BUSY)) {
        break;
      }
      gBS->Stall (100);
    }
  }

  MmioWrite32 (Host->Base + DWMCI_RINTSTS, RINTSTS_ACK (0xFFFFFFFFU));
  Host->IrqStatus = 0;
  MmioWrite32 (Host->Base + DWMCI_CMDARG, Arg);

  //
  // WAIT_PRV_DAT only on data commands; CMD0 gets the 80 init clocks, CMD12
  // aborts the data transfer in flight.  CRC check only when the response
  // carries a valid CRC (not R3: CMD1 / ACMD41).
  //
  CmdReg = Cmd | CMD_START | CMD_USE_HOLD_REG;
  if (Cmd == 0) {
    CmdReg |= CMD_SEND_INIT;
  } else if (Cmd == 12) {
    CmdReg |= CMD_STOP_ABORT;
  } else if (Flags & HOST_CMD_DATA) {
    CmdReg |= CMD_WAIT_PRV_DAT;
  }
  if (!(Flags & HOST_CMD_NO_CRC)) {
    CmdReg |= CMD_CHECK_CRC;
  }
  if (Flags & HOST_CMD_DATA) {
    CmdReg |= CMD_DATA_EXP;
    if (Flags & HOST_CMD_WRITE) {
      CmdReg |= CMD_RW;
    }
  }
  if (Flags & HOST_CMD_RESP_LONG) {
    CmdReg |= CMD_RESP_EXP | CMD_RESP_LEN;
  } else if (Flags & (HOST_CMD_RESP_SHORT | HOST_CMD_RESP_BUSY)) {
    CmdReg |= CMD_RESP_EXP;
  }

  MmioWrite32 (Host->Base + DWMCI_CMD, CmdReg);

  //
  // Command done or error (~1 s).  A missing card answers with RTO quickly.
  //
  WaitStatus (Host, INT_CDONE | INT_ERR, 1000000, &Int);
  if (Int & INT_ERR) {
    AckStatus (Host, Int & (INT_ERR | INT_CDONE));
    //
    // Identification probes time out by design (CMD8 on MMC / SD v1,
    // ACMD41 / CMD55 on an empty slot); data and stop commands must not.
    //
    if ((Flags & HOST_CMD_DATA) || (Cmd == 12) || (Cmd == 13)) {
      DEBUG ((DEBUG_ERROR, "Dwmci %x: cmd %u arg 0x%x INT 0x%x STATUS 0x%x\n",
              Host->Base, Cmd, Arg, Int, MmioRead32 (Host->Base + DWMCI_STATUS)));
    }
    return (Int & INT_RTO) ? EFI_TIMEOUT : EFI_DEVICE_ERROR;
  }
  if (!(Int & INT_CDONE)) {
    DEBUG ((DEBUG_ERROR, "Dwmci %x: cmd %u no CDONE, RINTSTS 0x%x STATUS 0x%x\n",
            Host->Base, Cmd, Int, MmioRead32 (Host->Base + DWMCI_STATUS)));
    return EFI_TIMEOUT;
  }
  AckStatus (Host, INT_CDONE);

  if (Resp != NULL) {
    Resp[0] = MmioRead32 (Host->Base + DWMCI_RESP0);
    Resp[1] = MmioRead32 (Host->Base + DWMCI_RESP0 + 0x4);
    Resp[2] = MmioRead32 (Host->Base + DWMCI_RESP0 + 0x8);
    Resp[3] = MmioRead32 (Host->Base + DWMCI_RESP0 + 0xC);
  }

  return EFI_SUCCESS;
}

/**
  Push the clock registers into the CIU with the update-clock pseudo command.
  The normal form waits for the previous data transfer; a card wedged in a
  failed fast mode holds DAT0 busy for good and the command never completes.
  Then reset the controller (CTRL/FIFO/DMA) and push without that wait.
**/
STATIC
BOOLEAN
ClockUpdate (
  IN MMC_HOST  *Host
  )
{
  UINTN  Retry;

  MmioWrite32 (Host->Base + DWMCI_CMD, CMD_ONLY_CLK);
  if (WaitCmdIdle (Host, 500)) {
    return TRUE;
  }
  MmioWrite32 (Host->Base + DWMCI_CTRL, MmioRead32 (Host->Base + DWMCI_CTRL) | CTRL_ALL_RESET);
  for (Retry = 0; Retry < 1000; Retry++) {
    if (!(MmioRead32 (Host->Base + DWMCI_CTRL) & CTRL_ALL_RESET)) {
      break;
    }
    gBS->Stall (10);
  }
  MmioOr32 (Host->Base + DWMCI_CTRL, CTRL_INT_ENABLE);
  MmioWrite32 (Host->Base + DWMCI_CMD, CMD_START | CMD_SEND_CLK_ONLY);
  return WaitCmdIdle (Host, 500);
}

/**
  Card clock = CIU clock / (2 * CLKDIV), CLKDIV 0 = bypass.  Gate, program,
  push with the update-clock pseudo-command, ungate, push again.
**/
STATIC
EFI_STATUS
EFIAPI
DwmciSetClock (
  IN MMC_HOST  *Host,
  IN UINT32    ClockHz
  )
{
  UINT32  Div;

  Host->ClockHz = ClockHz;
  WaitCmdIdle (Host, 500);

  MmioWrite32 (Host->Base + DWMCI_CLKENA, 0);
  if (!ClockUpdate (Host)) {
    DEBUG ((DEBUG_ERROR, "Dwmci %x: clock gate update timed out\n", Host->Base));
    return EFI_TIMEOUT;
  }

  if (ClockHz == 0) {
    return EFI_SUCCESS;
  }

  //
  // Round up so the card clock never exceeds ClockHz.
  //
  if (ClockHz >= Host->MaxClockHz) {
    Div = 0;
  } else {
    Div = (Host->MaxClockHz + (2 * ClockHz) - 1) / (2 * ClockHz);
  }
  Div = MIN (Div, (UINT32)0xFF);

  MmioWrite32 (Host->Base + DWMCI_CLKSRC, 0);
  MmioWrite32 (Host->Base + DWMCI_CLKDIV, Div);
  if (!ClockUpdate (Host)) {
    DEBUG ((DEBUG_ERROR, "Dwmci %x: clock divider update timed out\n", Host->Base));
    return EFI_TIMEOUT;
  }

  MmioWrite32 (Host->Base + DWMCI_CLKENA, 1);
  if (!ClockUpdate (Host)) {
    DEBUG ((DEBUG_ERROR, "Dwmci %x: clock enable update timed out\n", Host->Base));
    return EFI_TIMEOUT;
  }
  return EFI_SUCCESS;
}


/**
  Program the controller for a speed mode: CLKSEL phases / divider, UHS DDR
  select, and for HS400 the read-strobe block.  The card clock is gated; the
  caller picks the card clock with SetClock afterwards.  The CMU is the
  environment's business (Host->SetSourceClock).
**/
STATIC
EFI_STATUS
EFIAPI
DwmciSetSpeed (
  IN MMC_HOST   *Host,
  IN MMC_SPEED  Speed
  )
{
  UINT32      ClkSel;
  UINT32      Uhs;
  UINT32      Rddqs;
  UINT32      Dline;
  UINT32      Thr;
  UINT32      Ciu;
  EFI_STATUS  Status;

  ClkSel = CLKSEL_SDR;
  Uhs    = 0;
  Rddqs  = RDDQS_DEF;
  Dline  = DLINE_DEFAULT;
  Thr    = 0;
  if (Speed == MmcSpeedHs200) {
    ClkSel = (CLKSEL_HS200 & ~7U) | ((Host->SamplePhase <= 7) ? Host->SamplePhase : 0);
    Uhs    = 1;                                // 1.8 V signalling
  } else if (Speed == MmcSpeedDdr52) {
    ClkSel = (CLKSEL_DDR52 & ~7U) | ((Host->SamplePhase <= 7) ? Host->SamplePhase : (CLKSEL_DDR52 & 7));
    Uhs    = UHS_DDR_SLOT0 | (Host->Uhs1V8 & 1);
    if (Host->ThrOverride != 0xFFFFFFFFU) {
      Thr = Host->ThrOverride;
    }
  } else if (Speed == MmcSpeedHs400Es) {
    if (Host->SetSourceClock == NULL) {
      return EFI_UNSUPPORTED;
    }
    ClkSel = (CLKSEL_DDR200 & ~7U) | ((Host->SamplePhase <= 7) ? Host->SamplePhase : (CLKSEL_DDR200 & 7));
    Uhs    = UHS_DDR_SLOT0 | (Host->Uhs1V8 & 1);
    Rddqs  = RDDQS_DEF | RDDQS_RDDQS_EN | (Host->Strobe ? RDDQS_RESP_RCLK_MODE : 0);
    Dline  = DLINE_FIFO_CLK_DELAY (2) | DLINE_RD_DQS_DELAY (Host->DqsDelay);
    Thr    = (Host->ThrOverride != 0xFFFFFFFFU) ? Host->ThrOverride : ((512U << 16) | 1);   // card read threshold (dw_mci_set_ios)
  }

  if ((Host->DrivePhase <= 7) && ((Speed == MmcSpeedDdr52) || (Speed == MmcSpeedHs400Es) || (Speed == MmcSpeedHs200))) {
    ClkSel = (ClkSel & ~(7U << 16)) | (Host->DrivePhase << 16);
  }
  Status = DwmciSetClock (Host, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Host->SetSourceClock != NULL) {
    Status = Host->SetSourceClock (Host, Speed, ClkSel, &Ciu);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Host->MaxClockHz = Ciu;
  }
  MmioWrite32 (Host->Base + DWMCI_UHS_REG, Uhs);
  MmioWrite32 (Host->Base + DWMCI_CLKSEL, ClkSel);
  MmioWrite32 (Host->Base + DWMCI_CDTHRCTL, Thr);
  MmioWrite32 (Host->Base + DWMCI_DDR200_RDDQS_EN, Rddqs);
  MmioWrite32 (Host->Base + DWMCI_DDR200_DLINE_CTRL, Dline);
  Host->Speed = Speed;
  return EFI_SUCCESS;
}

/**
  CTYPE: bit 0 = 4-bit, bit 16 = 8-bit (slot 0).
**/
STATIC
EFI_STATUS
EFIAPI
DwmciSetBusWidth (
  IN MMC_HOST  *Host,
  IN UINT32    Width
  )
{
  UINT32  Ctype;

  switch (Width) {
    case 8:
      Ctype = 1U << 16;
      break;
    case 4:
      Ctype = 1U << 0;
      break;
    default:
      Ctype = 0;
      break;
  }
  MmioWrite32 (Host->Base + DWMCI_CTYPE, Ctype);
  return EFI_SUCCESS;
}

/**
  Block size and total bytes of the next data phase (MMC_HOST.BlockSize /
  ByteCount override the 512-byte memory block default).
**/
STATIC
UINT32
DataBlockSize (
  IN MMC_HOST  *Host
  )
{
  return (Host->BlockSize != 0) ? Host->BlockSize : BLOCK_SIZE;
}

STATIC
UINT32
DataBytes (
  IN MMC_HOST  *Host,
  IN UINT32    BlockCount
  )
{
  return (Host->ByteCount != 0) ? Host->ByteCount : BlockCount * DataBlockSize (Host);
}

/**
  Reset the FIFO and the IDMAC, build the descriptor chain over the bounce
  buffer and arm the DMA; the data command that follows starts it.
**/
STATIC
EFI_STATUS
EFIAPI
DwmciPrepareData (
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount,
  IN VOID      *Buffer
  )
{
  UINT32  Bytes;
  UINT32  Left;
  UINT32  Index;
  UINT32  Count;
  UINT32  *Desc;
  UINTN   Retry;

  Bytes = DataBytes (Host, BlockCount);
  if ((Bytes == 0) || (Bytes > MMC_MAX_DMA_BYTES)) {
    return EFI_INVALID_PARAMETER;
  }

  MmioOr32 (Host->Base + DWMCI_CTRL, CTRL_FIFO_RESET | CTRL_DMA_RESET);
  for (Retry = 0; Retry < 1000; Retry++) {
    if (!(MmioRead32 (Host->Base + DWMCI_CTRL) & (CTRL_FIFO_RESET | CTRL_DMA_RESET))) {
      break;
    }
    gBS->Stall (10);
  }
  MmioWrite32 (Host->Base + DWMCI_BMOD, BMOD_SWRESET);

  if (Read) {
    InvalidateDataCacheRange (Host->DmaBounce, Bytes);
  } else {
    CopyMem (Host->DmaBounce, Buffer, Bytes);
    WriteBackDataCacheRange (Host->DmaBounce, Bytes);
  }

  Count = (Bytes + DESC_BUF_BYTES - 1) / DESC_BUF_BYTES;
  Left  = Bytes;
  for (Index = 0; Index < Count; Index++) {
    Desc    = Host->DmaDesc + Index * DESC_WORDS;
    Desc[0] = DES0_OWN | DES0_DIC | DES0_CH | ((Index == 0) ? DES0_FD : 0);
    Desc[1] = MIN (Left, (UINT32)DESC_BUF_BYTES);
    Desc[2] = Host->DmaBouncePhys + Index * DESC_BUF_BYTES;
    Desc[3] = Host->DmaDescPhys + (Index + 1) * DESC_WORDS * sizeof (UINT32);
    Left   -= Desc[1];
  }
  Desc     = Host->DmaDesc + (Count - 1) * DESC_WORDS;
  Desc[0] &= ~(DES0_CH | DES0_DIC);
  Desc[0] |= DES0_LD;
  Desc[3]  = 0;
  WriteBackDataCacheRange (Host->DmaDesc, Count * DESC_WORDS * sizeof (UINT32));

  MmioWrite32 (Host->Base + DWMCI_IDSTS, IDSTS_ALL);
  MmioWrite32 (Host->Base + DWMCI_IDINTEN, Host->IrqMode ? 0x337 : 0);
  Host->IrqIdSts = 0;
  MmioWrite32 (Host->Base + DWMCI_DBADDR, Host->DmaDescPhys);
  MmioWrite32 (Host->Base + DWMCI_TMOUT, 0xFFFFFFFFU);
  MmioWrite32 (Host->Base + DWMCI_BLKSIZ, DataBlockSize (Host));

  //
  // FIFO thresholds: the init value (8-entry bursts, RX watermark at half
  // the FIFO) suits 512-byte blocks.  A small SDIO block would never reach
  // it; like dw_mci_submit_data() for SDIO cards, use single bursts and an
  // RX watermark of 1 for blocks under half the FIFO.
  //
  if (Host->BlockSize != 0) {
    MmioWrite32 (Host->Base + DWMCI_FIFOTH, (0U << 28) | (1U << 16) | (DWMCI_FIFO_DEPTH / 2));
  } else {
    MmioWrite32 (Host->Base + DWMCI_FIFOTH, (2U << 28) |
                 ((DWMCI_FIFO_DEPTH / 2 - 1) << 16) | (DWMCI_FIFO_DEPTH / 2));
  }
  MmioWrite32 (Host->Base + DWMCI_BYTCNT, Bytes);

  MmioOr32 (Host->Base + DWMCI_CTRL, CTRL_USE_IDMAC);
  MmioWrite32 (Host->Base + DWMCI_BMOD, BMOD_ENABLE | BMOD_FB);
  MmioWrite32 (Host->Base + DWMCI_PLDMND, 1);
  return EFI_SUCCESS;
}

STATIC
VOID
DwmciStopDma (
  IN MMC_HOST  *Host
  )
{
  MmioAnd32 (Host->Base + DWMCI_CTRL, ~CTRL_USE_IDMAC);
  MmioWrite32 (Host->Base + DWMCI_BMOD, 0);
  MmioWrite32 (Host->Base + DWMCI_IDSTS, IDSTS_ALL);
}

/**
  Wait for the data phase started by the data command: data-transfer-over
  from the CIU and receive/transmit-done from the IDMAC.  Reads are copied
  out of the bounce buffer.
**/
STATIC
EFI_STATUS
EFIAPI
DwmciXferBlock (
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount,
  OUT VOID     *Buffer
  )
{
  UINT32           Int;
  UINT32           IdSts;
  UINT32           Done;
  UINTN            Retry;
  BOOLEAN          DmaDone;
  volatile UINT32  *LastDesc;

  Int      = 0;
  IdSts    = 0;
  DmaDone  = FALSE;
  Done     = Read ? IDSTS_RI : IDSTS_TI;
  LastDesc = Host->DmaDesc +
             ((DataBytes (Host, BlockCount) + DESC_BUF_BYTES - 1) / DESC_BUF_BYTES - 1) * DESC_WORDS;

  //
  // Up to ~5 s: 512 KiB at the 1-bit identification clock is ~10 s worth
  // of bits, but only EXT_CSD (one block) is read that slowly.
  //
  if (Host->IrqMode) {
    //
    // Interrupt driven: sleep until the ISR reports data-transfer-over or
    // an error, then give the IDMAC a moment to hand the last descriptor
    // back (OWN cleared).
    //
    WaitStatus (Host, INT_DTO | INT_ERR, 5000000, &Int);
    IdSts = Host->IrqIdSts;
    if ((Int & INT_DTO) && !(Int & INT_ERR) && !(IdSts & IDSTS_ERR)) {
      for (Retry = 0; Retry < 10000; Retry++) {
        InvalidateDataCacheRange ((VOID *)LastDesc, DESC_WORDS * sizeof (UINT32));
        DmaDone = ((Host->IrqIdSts & Done) != 0) || ((LastDesc[0] & DES0_OWN) == 0);
        if (DmaDone) {
          break;
        }
        gBS->Stall (10);
      }
      IdSts = Host->IrqIdSts;
    }
    AckStatus (Host, Int);
  } else {
    for (Retry = 0; Retry < 500000; Retry++) {
      Int   = MmioRead32 (Host->Base + DWMCI_RINTSTS);
      IdSts = MmioRead32 (Host->Base + DWMCI_IDSTS);
      if ((Int & INT_ERR) || (IdSts & IDSTS_ERR)) {
        break;
      }
      if (Int & INT_DTO) {
        //
        // The IDMAC hands each descriptor back (OWN cleared) once its data
        // is in memory; RI/TI in IDSTS does not latch with IDINTEN masked.
        //
        InvalidateDataCacheRange ((VOID *)LastDesc, DESC_WORDS * sizeof (UINT32));
        DmaDone = ((IdSts & Done) != 0) || ((LastDesc[0] & DES0_OWN) == 0);
        if (DmaDone) {
          break;
        }
      }
      gBS->Stall (10);
    }
    MmioWrite32 (Host->Base + DWMCI_RINTSTS, RINTSTS_ACK (Int));
  }
  DwmciStopDma (Host);

  if ((Int & INT_ERR) || (IdSts & IDSTS_ERR) || !(Int & INT_DTO) || !DmaDone) {
    DEBUG ((DEBUG_ERROR, "Dwmci %x: %a %u blocks failed, RINTSTS 0x%x IDSTS 0x%x "
            "DES0 0x%x TCBCNT %u TBBCNT %u\n", Host->Base, Read ? "read" : "write",
            BlockCount, Int, IdSts, LastDesc[0], MmioRead32 (Host->Base + DWMCI_TCBCNT),
            MmioRead32 (Host->Base + DWMCI_TBBCNT)));
    return ((Int & INT_ERR) || (IdSts & IDSTS_ERR)) ? EFI_DEVICE_ERROR : EFI_TIMEOUT;
  }

  if (Read) {
    InvalidateDataCacheRange (Host->DmaBounce, DataBytes (Host, BlockCount));
    CopyMem (Buffer, Host->DmaBounce, DataBytes (Host, BlockCount));
  }
  return EFI_SUCCESS;
}

/**
  Reset the controller and prepare it for card identification at 400 kHz.

  @param[out] Host         Initialised host (ops set, Card zeroed).
  @param[in]  Base         MMIO base.
  @param[in]  CiuClockHz   Controller clock after the CLKSEL divider.
  @param[in]  MaxBusWidth  Data lines wired to the slot.
  @param[in]  ClkSel       CLKSEL value (divider + sample/drive phases).
  @param[in]  SmuBypass    Put the eMMCP security unit in bypass (ch0/1).
**/
EFI_STATUS
DwmciInit (
  IN MMC_HOST  *Host,
  IN UINT32    Base,
  IN UINT32    CiuClockHz,
  IN UINT32    MaxBusWidth,
  IN UINT32    ClkSel,
  IN BOOLEAN   SmuBypass
  )
{
  UINTN   Retry;
  UINT32  VerId;
  UINT8   *Pool;

  ZeroMem (Host, sizeof (*Host));
  Host->Base        = Base;
  Host->MaxClockHz  = CiuClockHz;
  Host->MaxBusWidth = MaxBusWidth;
  Host->SendCmd     = DwmciSendCmd;
  Host->SetClock    = DwmciSetClock;
  Host->PrepareData = DwmciPrepareData;
  Host->XferBlock   = DwmciXferBlock;
  Host->SetBusWidth = DwmciSetBusWidth;
  Host->SetSpeed    = DwmciSetSpeed;
  Host->DqsDelay    = DQS_DELAY_V1A;
  Host->Uhs1V8      = 1;
  Host->DrivePhase  = 0xFF;
  Host->ThrOverride = 0xFFFFFFFFU;
  Host->SamplePhase = 0xFF;                 // unset: each mode uses the board default
  Host->Speed       = MmcSpeedDefault;

  VerId = MmioRead32 (Base + DWMCI_VERID) & 0xFFFF;

  //
  // One page of descriptors + the bounce buffer, below 4 GiB (32-bit DMA).
  //
  Pool = DmaAllocatePages (EFI_SIZE_TO_PAGES (EFI_PAGE_SIZE + MMC_MAX_DMA_BYTES));
  if (Pool == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Host->DmaDesc       = (UINT32 *)Pool;
  Host->DmaBounce     = Pool + EFI_PAGE_SIZE;
  Host->DmaDescPhys   = DMA_ADDRESS (Host->DmaDesc);
  Host->DmaBouncePhys = DMA_ADDRESS (Host->DmaBounce);
  ASSERT (DESC_COUNT * DESC_WORDS * sizeof (UINT32) <= EFI_PAGE_SIZE);

  //
  // Controller, FIFO and DMA reset (the bootloader leaves IDMAC state and
  // FIFO contents behind).
  //
  MmioWrite32 (Base + DWMCI_CTRL, CTRL_ALL_RESET);
  for (Retry = 0; Retry < 1000; Retry++) {
    if (!(MmioRead32 (Base + DWMCI_CTRL) & CTRL_ALL_RESET)) {
      break;
    }
    gBS->Stall (100);
  }
  if (MmioRead32 (Base + DWMCI_CTRL) & CTRL_ALL_RESET) {
    DEBUG ((DEBUG_ERROR, "Dwmci %x: reset stuck, CTRL 0x%x\n", Base,
            MmioRead32 (Base + DWMCI_CTRL)));
    return EFI_TIMEOUT;
  }

  //
  // IDMAC reset and off until a data command arms it.
  //
  MmioWrite32 (Base + DWMCI_BMOD, BMOD_SWRESET);
  MmioWrite32 (Base + DWMCI_BMOD, 0);

  if (SmuBypass) {
    MmioWrite32 (Base + DWMCI_MPSBEGIN0, 0);
    MmioWrite32 (Base + DWMCI_MPSEND0, 0xFFFFFFFFU);
    MmioWrite32 (Base + DWMCI_MPSCTRL0, MPSCTRL_BYPASS);
  }

  //
  // SDR: the bootloader runs the eMMC in DDR and leaves UHS_REG's DDR bit
  // set (0x00010000), which survives the controller reset.  With it, 1-bit
  // still works but every 4/8-bit read gets half the data and an end-bit
  // error.
  //
  MmioWrite32 (Base + DWMCI_UHS_REG, 0);
  MmioWrite32 (Base + DWMCI_CLKSEL, ClkSel);
  MmioWrite32 (Base + DWMCI_FIFOTH, (2U << 28) |
               ((DWMCI_FIFO_DEPTH / 2 - 1) << 16) | (DWMCI_FIFO_DEPTH / 2));
  MmioWrite32 (Base + DWMCI_PWREN, 1);
  MmioWrite32 (Base + DWMCI_CTYPE, 0);    // 1-bit for identification
  MmioWrite32 (Base + DWMCI_TMOUT, 0xFFFFFFFFU);
  MmioWrite32 (Base + DWMCI_INTMSK, 0);
  MmioWrite32 (Base + DWMCI_RINTSTS, 0xFFFFFFFFU);

  //
  // Polled: the interrupt engine runs (RINTSTS), nothing goes to the GIC.
  //
  MmioWrite32 (Base + DWMCI_CTRL, CTRL_INT_ENABLE);

  DEBUG ((DEBUG_INFO, "Dwmci %x: VERID %x, CIU %u Hz, CLKSEL %08x\n",
          Base, VerId, CiuClockHz, ClkSel));

  return DwmciSetClock (Host, 400000);
}
