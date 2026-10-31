/** @file
  Synopsys DWC3 device-mode backend for UsbFnDxe (Exynos5420 USB3.0 DRD0).

  Core bring-up follows the downstream kernel's split:
    exynos-drd.c     exynos_drd_init(), exynos_drd_phy_set(),
                     exynos_drd_config(), exynos_drd_change_mode()
    exynos_ss_udc.c  exynos_ss_udc_corereset(), exynos_ss_udc_init(),
                     exynos_ss_udc_ep0_activate(), the EP0 state machine
                     (process_control / xfer_complete / xfer_notready)

  One TRB (two for an EP0 IN reply that needs a ZLP) per physical endpoint;
  every transfer goes through a bounce buffer in the DMA pool.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UsbFnDxe.h"

#define REG(x)  (DWC3_BASE + (x))

#define SUSPEND_CLK_HZ  24000000          // sclk_usbdrd30 = XXTI (Board.c)

//
// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------
//

STATIC
UINT32
PtrToDma (
  IN CONST VOID  *Ptr
  )
{
  return (UINT32)(UINTN)Ptr;   // identity mapped, DRAM below 4 GiB
}

STATIC
BOOLEAN
PollClear (
  IN UINTN   Address,
  IN UINT32  Mask,
  IN UINTN   TimeoutUs
  )
{
  while (MmioRead32 (Address) & Mask) {
    if (TimeoutUs-- == 0) {
      return FALSE;
    }
    gBS->Stall (1);
  }
  return TRUE;
}

STATIC
BOOLEAN
PollSet (
  IN UINTN   Address,
  IN UINT32  Mask,
  IN UINTN   TimeoutUs
  )
{
  while (!(MmioRead32 (Address) & Mask)) {
    if (TimeoutUs-- == 0) {
      return FALSE;
    }
    gBS->Stall (1);
  }
  return TRUE;
}

/**
  Issue an endpoint command and wait for it.  Returns the DEPCMD register
  after completion (XferRscIdx for StartXfer), or MAX_UINT32 on timeout.
**/
STATIC
UINT32
DepCmd (
  IN UINT8   Phys,
  IN UINT32  Cmd,
  IN UINT32  Param0,
  IN UINT32  Param1,
  IN UINT32  Param2
  )
{
  UINT32  Reg;

  MmioWrite32 (REG (DEPCMDPAR2 (Phys)), Param2);
  MmioWrite32 (REG (DEPCMDPAR1 (Phys)), Param1);
  MmioWrite32 (REG (DEPCMDPAR0 (Phys)), Param0);
  MmioWrite32 (REG (DEPCMD (Phys)), Cmd | DEPCMD_CMDACT);

  if (!PollClear (REG (DEPCMD (Phys)), DEPCMD_CMDACT, 1000)) {
    DEBUG ((DEBUG_ERROR, "UsbFn: DEPCMD %x on ep%u timed out\n", Cmd, Phys));
    return MAX_UINT32;
  }

  Reg = MmioRead32 (REG (DEPCMD (Phys)));
  if (DEPCMD_STATUS (Reg) != 0) {
    DEBUG ((DEBUG_ERROR, "UsbFn: DEPCMD %x on ep%u status %x\n",
            Cmd, Phys, DEPCMD_STATUS (Reg)));
  }
  return Reg;
}

STATIC
USBFN_XFER *
XferOf (
  IN USBFN_DEV  *Dev,
  IN UINT8      Phys
  )
{
  return (Phys & 1) ? &Dev->In[Phys >> 1] : &Dev->Out[Phys >> 1];
}

/**
  Fill the endpoint's TRB(s) and issue StartXfer.
**/
STATIC
EFI_STATUS
StartTrb (
  IN USBFN_DEV  *Dev,
  IN UINT8      Phys,
  IN UINT8      *Buffer,
  IN UINTN      Length,
  IN UINT32     TrbCtl,
  IN BOOLEAN    Zlp
  )
{
  DWC3_TRB    *Trb;
  USBFN_XFER  *X;
  UINT32      Ctrl;
  UINT32      Reg;

  Trb  = Dev->Trb[Phys];
  X    = XferOf (Dev, Phys);
  Ctrl = TRB_CTRL_TRBCTL (TrbCtl) | TRB_CTRL_HWO;
  if (!(Phys & 1)) {
    Ctrl |= TRB_CTRL_ISP_IMI;   // OUT: a short packet ends the transfer
  }

  Trb[0].BufLo = PtrToDma (Buffer);
  Trb[0].BufHi = 0;
  Trb[0].Size  = (UINT32)Length & TRB_SIZE_MASK;
  if (Zlp) {
    Trb[0].Ctrl  = Ctrl | TRB_CTRL_CHN;
    Trb[1].BufLo = PtrToDma (Buffer);
    Trb[1].BufHi = 0;
    Trb[1].Size  = 0;
    Trb[1].Ctrl  = Ctrl | TRB_CTRL_LST | TRB_CTRL_IOC;
  } else {
    Trb[0].Ctrl = Ctrl | TRB_CTRL_LST | TRB_CTRL_IOC;
  }
  WriteBackDataCacheRange (Trb, DWC3_TRB_SLOT);

  Reg = DepCmd (Phys, DEPCMD_DEPSTRTXFER, 0, PtrToDma (Trb), 0);
  if ((Reg == MAX_UINT32) || (DEPCMD_STATUS (Reg) != 0)) {
    Trb[0].Ctrl = 0;
    WriteBackDataCacheRange (Trb, DWC3_TRB_SLOT);
    return EFI_DEVICE_ERROR;
  }

  X->Resource   = (UINT8)DEPCMD_XFERRSCIDX (Reg);
  X->Programmed = Length;
  X->Started    = TRUE;
  return EFI_SUCCESS;
}

/**
  Bytes the core moved for the TRB of a completed transfer.
**/
STATIC
UINTN
TrbDone (
  IN USBFN_DEV  *Dev,
  IN UINT8      Phys
  )
{
  DWC3_TRB    *Trb;
  USBFN_XFER  *X;
  UINTN       Left;

  Trb = Dev->Trb[Phys];
  X   = XferOf (Dev, Phys);
  InvalidateDataCacheRange (Trb, DWC3_TRB_SLOT);
  Left = Trb[0].Size & TRB_SIZE_MASK;
  return (Left <= X->Programmed) ? (X->Programmed - Left) : 0;
}

/**
  EndXfer (forced) on a physical endpoint with a transfer in flight.
**/
STATIC
VOID
EndXfer (
  IN USBFN_DEV  *Dev,
  IN UINT8      Phys
  )
{
  USBFN_XFER  *X;

  X = XferOf (Dev, Phys);
  if (!X->Started) {
    return;
  }

  DepCmd (Phys, DEPCMD_DEPENDXFER | DEPCMD_PARAM (X->Resource) |
          DEPCMD_HIPRI_FORCERM, 0, 0, 0);
  //
  // The core needs time to retire the transfer resource before the
  // endpoint can be restarted (dwc3 databook; Linux waits 100 us too).
  //
  gBS->Stall (100);
  X->Started = FALSE;
}

//
// ---------------------------------------------------------------------------
// DMA pool.
// ---------------------------------------------------------------------------
//

EFI_STATUS
Dwc3AllocatePool (
  IN USBFN_DEV  *Dev
  )
{
  UINTN       Size;
  UINT8       *Ptr;
  UINTN       Phys;
  EFI_STATUS  Status;

  //
  // [event buffer 4K][TRB slots + SETUP][EP0 bounce x2][bulk bounce x6]
  //
  Size = DWC3_EVENT_BUFFER_SIZE + EFI_PAGE_SIZE + EFI_PAGE_SIZE +
         (DWC3_PHYS_EPS - 2) * DWC3_BOUNCE_SIZE;
  Dev->PoolPages = EFI_SIZE_TO_PAGES (Size);
  Dev->Pool      = AllocateAlignedPages (Dev->PoolPages, EFI_PAGE_SIZE);
  if (Dev->Pool == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  ZeroMem (Dev->Pool, EFI_PAGES_TO_SIZE (Dev->PoolPages));
  WriteBackInvalidateDataCacheRange (Dev->Pool, EFI_PAGES_TO_SIZE (Dev->PoolPages));

  //
  // Uncached if the CPU driver lets us; the cache maintenance below keeps
  // it correct either way.
  //
  Status = gDS->SetMemorySpaceAttributes ((UINTN)Dev->Pool,
                                          EFI_PAGES_TO_SIZE (Dev->PoolPages),
                                          EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbFn: DMA pool stays cached (%r)\n", Status));
  }

  Ptr              = Dev->Pool;
  Dev->EventBuffer = (UINT32 *)Ptr;
  Ptr             += DWC3_EVENT_BUFFER_SIZE;

  for (Phys = 0; Phys < DWC3_PHYS_EPS; Phys++) {
    Dev->Trb[Phys] = (DWC3_TRB *)(Ptr + Phys * DWC3_TRB_SLOT);
  }
  Dev->SetupBuffer = Ptr + DWC3_PHYS_EPS * DWC3_TRB_SLOT;
  Ptr             += EFI_PAGE_SIZE;

  Dev->Bounce[0] = Ptr;
  Dev->Bounce[1] = Ptr + USBFN_EP0_BUFFER;
  Ptr           += EFI_PAGE_SIZE;

  for (Phys = 2; Phys < DWC3_PHYS_EPS; Phys++) {
    Dev->Bounce[Phys] = Ptr;
    Ptr              += DWC3_BOUNCE_SIZE;
  }

  DEBUG ((DEBUG_ERROR, "UsbFn: DMA pool %p, %u pages\n", Dev->Pool, Dev->PoolPages));
  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// EP0 control state machine.
// ---------------------------------------------------------------------------
//

STATIC
VOID
Ep0ArmSetup (
  IN USBFN_DEV  *Dev
  )
{
  Dev->Ep0State         = Ep0StateSetup;
  Dev->Ep0StatusReady   = FALSE;
  Dev->Ep0StatusPending = FALSE;
  InvalidateDataCacheRange (Dev->SetupBuffer, 64);
  if (EFI_ERROR (StartTrb (Dev, 0, Dev->SetupBuffer, 8, TRBCTL_CONTROL_SETUP, FALSE))) {
    DEBUG ((DEBUG_ERROR, "UsbFn: cannot arm SETUP\n"));
  }
}

STATIC
VOID
Ep0StartStatus (
  IN USBFN_DEV  *Dev
  )
{
  UINT8  Phys;

  Phys          = Dev->Ep0StatusPhys;
  Dev->Ep0State = Ep0StateStatus;
  StartTrb (Dev, Phys, Dev->Bounce[Phys], 0,
            Dev->Ep0ThreeStage ? TRBCTL_CONTROL_STATUS3 : TRBCTL_CONTROL_STATUS2,
            FALSE);
}

/**
  Protocol stall of the current control transfer: stall physical EP0 and
  wait for the next SETUP (the core clears the stall when it arrives).
**/
VOID
Dwc3Ep0Stall (
  IN USBFN_DEV  *Dev
  )
{
  EndXfer (Dev, 0);
  EndXfer (Dev, 1);
  ZeroMem (&Dev->In[0], sizeof (Dev->In[0]));
  ZeroMem (&Dev->Out[0], sizeof (Dev->Out[0]));
  DepCmd (0, DEPCMD_DEPSSTALL, 0, 0, 0);
  Ep0ArmSetup (Dev);
}

/**
  The status stage of the current request may go (no-data request handled,
  or the class driver acknowledged it).
**/
VOID
Dwc3Ep0StatusReady (
  IN USBFN_DEV  *Dev
  )
{
  Dev->Ep0StatusReady = TRUE;
  if ((Dev->Ep0State == Ep0StateWaitStatus) && Dev->Ep0StatusPending) {
    Ep0StartStatus (Dev);
  }
}

STATIC
VOID
Ep0XferComplete (
  IN USBFN_DEV  *Dev,
  IN UINT8      Phys
  )
{
  EFI_USB_DEVICE_REQUEST  Req;
  USBFN_XFER              *X;
  UINTN                   Done;

  X          = XferOf (Dev, Phys);
  X->Started = FALSE;

  switch (Dev->Ep0State) {
    case Ep0StateSetup:
      InvalidateDataCacheRange (Dev->SetupBuffer, 64);
      CopyMem (&Req, Dev->SetupBuffer, sizeof (Req));

      ZeroMem (&Dev->In[0], sizeof (Dev->In[0]));
      ZeroMem (&Dev->Out[0], sizeof (Dev->Out[0]));
      Dev->Ep0ThreeStage    = (Req.Length != 0);
      Dev->Ep0DirIn         = (Req.RequestType & USB_ENDPOINT_DIR_IN) != 0;
      Dev->Ep0StatusReady   = FALSE;
      Dev->Ep0StatusPending = FALSE;
      Dev->Ep0State         = Dev->Ep0ThreeStage ? Ep0StateData : Ep0StateWaitStatus;

      UsbFnEp0Setup (Dev, &Req);
      break;

    case Ep0StateData:
      X    = Dev->Ep0DirIn ? &Dev->In[0] : &Dev->Out[0];
      Done = TrbDone (Dev, Phys);
      if (!Dev->Ep0DirIn) {
        Done = MIN (Done, X->Length);
        InvalidateDataCacheRange (Dev->Bounce[Phys], USBFN_EP0_BUFFER);
        CopyMem (X->Buffer, Dev->Bounce[Phys], Done);
      }
      X->Done   = Done;
      X->Active = FALSE;
      if (!X->Internal) {
        UsbFnQueueTransfer (Dev, 0, Dev->Ep0DirIn);
      }
      Dev->Ep0State = Ep0StateWaitStatus;
      Dwc3Ep0StatusReady (Dev);   // data done: status follows on its own
      break;

    case Ep0StateStatus:
      Ep0ArmSetup (Dev);
      break;

    default:
      Ep0ArmSetup (Dev);
      break;
  }
}

STATIC
VOID
Ep0XferNotReady (
  IN USBFN_DEV  *Dev,
  IN UINT8      Phys,
  IN UINT32     Event
  )
{
  UINT32  Stage;

  Stage = DEPEVT_CTL_STATUS (Event);

  //
  // A new SETUP while the previous request is unfinished (its owner never
  // answered and the host gave up): abandon that request and take the new
  // one.  Without a SETUP TRB the core would hold it forever.
  //
  if ((Stage == CTL_STATUS_SETUP) && (Dev->Ep0State != Ep0StateSetup)) {
    DEBUG ((DEBUG_ERROR, "UsbFn: EP0 request abandoned by the host\n"));
    EndXfer (Dev, 0);
    EndXfer (Dev, 1);
    ZeroMem (&Dev->In[0], sizeof (Dev->In[0]));
    ZeroMem (&Dev->Out[0], sizeof (Dev->Out[0]));
    Ep0ArmSetup (Dev);
    return;
  }

  switch (Dev->Ep0State) {
    case Ep0StateSetup:
      if (Stage != CTL_STATUS_SETUP) {
        DEBUG ((DEBUG_ERROR, "UsbFn: EP0 NotReady stage %u in SETUP\n", Stage));
        Dwc3Ep0Stall (Dev);
      }
      break;

    case Ep0StateData:
      if ((Stage == CTL_STATUS_DATA) && (((Phys & 1) != 0) != Dev->Ep0DirIn)) {
        DEBUG ((DEBUG_ERROR, "UsbFn: EP0 data in the wrong direction\n"));
        Dwc3Ep0Stall (Dev);
      }
      break;

    case Ep0StateWaitStatus:
      if (Stage == CTL_STATUS_DATA) {
        //
        // The host wants more data than the request carried, or a data
        // stage the request does not have.
        //
        Dwc3Ep0Stall (Dev);
        break;
      }
      if (Stage == CTL_STATUS_STATUS) {
        Dev->Ep0StatusPending = TRUE;
        Dev->Ep0StatusPhys    = Phys;
        if (Dev->Ep0StatusReady) {
          Ep0StartStatus (Dev);
        }
      }
      break;

    default:
      break;
  }
}

//
// ---------------------------------------------------------------------------
// Transfers.
// ---------------------------------------------------------------------------
//

/**
  Start the transfer described by Dev->In/Out[Ep] (Active, Buffer, Length,
  Internal and SendZlp already set by the caller).
**/
EFI_STATUS
Dwc3StartTransfer (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In
  )
{
  USBFN_XFER  *X;
  UINT8       Phys;
  UINTN       Length;
  UINT16      Mps;
  EFI_STATUS  Status;

  X    = In ? &Dev->In[Ep] : &Dev->Out[Ep];
  Phys = PHYS_EP (Ep, In);

  if (X->Started) {
    return EFI_NOT_READY;
  }

  if (Ep == 0) {
    //
    // Status acknowledgement of a two-stage request by its owner.
    //
    if ((Dev->Ep0State == Ep0StateWaitStatus) && !Dev->Ep0ThreeStage && In &&
        (X->Length == 0)) {
      X->Active = FALSE;
      X->Done   = 0;
      if (!X->Internal) {
        UsbFnQueueTransfer (Dev, 0, TRUE);
      }
      Dwc3Ep0StatusReady (Dev);
      return EFI_SUCCESS;
    }

    if ((Dev->Ep0State != Ep0StateData) || (In != Dev->Ep0DirIn)) {
      return EFI_NOT_READY;
    }

    Length = MIN (X->Length, (UINTN)USBFN_EP0_BUFFER);
    if (In) {
      CopyMem (Dev->Bounce[Phys], X->Buffer, Length);
      WriteBackDataCacheRange (Dev->Bounce[Phys], USBFN_EP0_BUFFER);
    } else {
      //
      // OUT buffer sizes must be a multiple of the max packet size.
      //
      Length = ALIGN_VALUE (MAX (Length, (UINTN)1), USBFN_EP0_MPS);
      Length = MIN (Length, (UINTN)USBFN_EP0_BUFFER);
      InvalidateDataCacheRange (Dev->Bounce[Phys], USBFN_EP0_BUFFER);
    }
    return StartTrb (Dev, Phys, Dev->Bounce[Phys], Length,
                     TRBCTL_CONTROL_DATA, In && X->SendZlp);
  }

  if (!Dev->EpsConfigured) {
    return EFI_NOT_READY;
  }

  Mps    = (Dev->EpMps[Ep] != 0) ? Dev->EpMps[Ep] : 512;
  Length = MIN (X->Length, (UINTN)DWC3_BOUNCE_SIZE);
  if (In) {
    CopyMem (Dev->Bounce[Phys], X->Buffer, Length);
    WriteBackDataCacheRange (Dev->Bounce[Phys], MAX (Length, (UINTN)1));
  } else {
    Length = ALIGN_VALUE (MAX (Length, (UINTN)1), Mps);
    Length = MIN (Length, (UINTN)DWC3_BOUNCE_SIZE);
    InvalidateDataCacheRange (Dev->Bounce[Phys], Length);
  }

  Status = StartTrb (Dev, Phys, Dev->Bounce[Phys], Length, TRBCTL_NORMAL, FALSE);
  return Status;
}

STATIC
VOID
EpXferComplete (
  IN USBFN_DEV  *Dev,
  IN UINT8      Phys
  )
{
  USBFN_XFER  *X;
  UINTN       Done;
  BOOLEAN     In;

  X  = XferOf (Dev, Phys);
  In = (Phys & 1) != 0;
  if (!X->Started) {
    return;
  }
  X->Started = FALSE;

  Done = TrbDone (Dev, Phys);
  if (!In) {
    Done = MIN (Done, X->Length);
    InvalidateDataCacheRange (Dev->Bounce[Phys], X->Programmed);
    CopyMem (X->Buffer, Dev->Bounce[Phys], Done);
  }
  X->Done = Done;

  if (X->Active) {
    X->Active = FALSE;
    UsbFnQueueTransfer (Dev, Phys >> 1, In);
  }
}

VOID
Dwc3AbortEp (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In
  )
{
  EndXfer (Dev, PHYS_EP (Ep, In));
}

VOID
Dwc3SetStall (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In,
  IN BOOLEAN    Stall
  )
{
  UINT8  Phys;

  if (Ep == 0) {
    if (Stall) {
      Dwc3Ep0Stall (Dev);
    }
    return;
  }

  Phys = PHYS_EP (Ep, In);
  DepCmd (Phys, Stall ? DEPCMD_DEPSSTALL : DEPCMD_DEPCSTALL, 0, 0, 0);
  Dev->Halted[Phys] = Stall;
}

BOOLEAN
Dwc3GetStall (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In
  )
{
  return (Ep != 0) && Dev->Halted[PHYS_EP (Ep, In)];
}

VOID
Dwc3SetAddress (
  IN UINT8  Address
  )
{
  //
  // DWC3 wants the new address in DCFG before the status stage.
  //
  MmioAndThenOr32 (REG (DCFG), ~DCFG_DEVADDR_MASK, DCFG_DEVADDR (Address));
}

/**
  SET_CONFIGURATION: configure and enable the endpoints of the
  configuration (exynos_ss_udc_ep_activate).
**/
VOID
Dwc3ActivateEndpoints (
  IN USBFN_DEV  *Dev
  )
{
  UINT8    Ep;
  UINT8    Dir;
  UINT8    Phys;
  BOOLEAN  In;

  if (Dev->EpsConfigured) {
    return;
  }

  //
  // Start a new configuration: transfer resources from index 2 on
  // (0 and 1 belong to EP0).
  //
  DepCmd (0, DEPCMD_DEPSTARTCFG | DEPCMD_PARAM (2), 0, 0, 0);

  for (Ep = 1; Ep < USBFN_MAX_EPS; Ep++) {
    for (Dir = 0; Dir < 2; Dir++) {
      In = (Dir == 1);
      if (!(Dev->EpDirs[Ep] & (In ? EP_DIR_IN : EP_DIR_OUT))) {
        continue;
      }
      Phys = PHYS_EP (Ep, In);

      DepCmd (Phys, DEPCMD_DEPCFG,
              DEPCFG0_EPTYPE (Dev->EpType[Ep]) | DEPCFG0_MPS (Dev->EpMps[Ep]) |
              (In ? DEPCFG0_FIFONUM (Ep) : 0),
              DEPCFG1_EPNUM (Ep) | (In ? DEPCFG1_EPDIR : 0) | DEPCFG1_XFERCMPLEN,
              0);
      DepCmd (Phys, DEPCMD_DEPXFERCFG, DEPXFERCFG0_NUMXFERRES (1), 0, 0);
      MmioOr32 (REG (DALEPENA), 1U << Phys);
      Dev->Halted[Phys] = FALSE;
    }
  }

  Dev->EpsConfigured = TRUE;
}

/**
  Drop every endpoint but EP0 (bus reset / disconnect).
**/
STATIC
VOID
DeactivateEndpoints (
  IN USBFN_DEV  *Dev
  )
{
  UINT8  Phys;

  for (Phys = 2; Phys < DWC3_PHYS_EPS; Phys++) {
    EndXfer (Dev, Phys);
    if (Dev->Halted[Phys]) {
      DepCmd (Phys, DEPCMD_DEPCSTALL, 0, 0, 0);
      Dev->Halted[Phys] = FALSE;
    }
  }
  MmioWrite32 (REG (DALEPENA), 0x3);

  ZeroMem (&Dev->In[1], sizeof (Dev->In[0]) * (USBFN_MAX_EPS - 1));
  ZeroMem (&Dev->Out[1], sizeof (Dev->Out[0]) * (USBFN_MAX_EPS - 1));
  Dev->EpsConfigured = FALSE;
  Dev->Configuration = 0;
}

//
// ---------------------------------------------------------------------------
// Events.
// ---------------------------------------------------------------------------
//

STATIC
VOID
OnBusReset (
  IN USBFN_DEV  *Dev
  )
{
  MmioAnd32 (REG (DCTL), ~DCTL_TSTCTL_MASK);
  MmioAnd32 (REG (GUSB2PHYCFG), ~GUSB2PHYCFG_SUSPHY);
  MmioAnd32 (REG (GUSB3PIPECTL), ~GUSB3PIPECTL_SUSPSSPHY);

  DeactivateEndpoints (Dev);

  //
  // Keep a SETUP that is still armed; anything else on EP0 is abandoned.
  //
  if (!((Dev->Ep0State == Ep0StateSetup) && Dev->Out[0].Started)) {
    EndXfer (Dev, 0);
    EndXfer (Dev, 1);
    ZeroMem (&Dev->In[0], sizeof (Dev->In[0]));
    ZeroMem (&Dev->Out[0], sizeof (Dev->Out[0]));
    Ep0ArmSetup (Dev);
  }

  MmioAnd32 (REG (DCFG), ~DCFG_DEVADDR_MASK);
  UsbFnQueueEvent (Dev, EfiUsbMsgBusEventReset, NULL);
}

STATIC
VOID
OnConnectDone (
  IN USBFN_DEV  *Dev
  )
{
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;
  UINT32                     Speed;

  Speed = DSTS_CONNECTSPD (MmioRead32 (REG (DSTS)));
  switch (Speed) {
    case CONNECTSPD_HS:
      Dev->Speed = UsbBusSpeedHigh;
      break;
    case CONNECTSPD_FS:
    case CONNECTSPD_FS48:
      Dev->Speed = UsbBusSpeedFull;
      break;
    default:
      DEBUG ((DEBUG_ERROR, "UsbFn: unexpected connect speed %u\n", Speed));
      Dev->Speed = UsbBusSpeedHigh;
      break;
  }
  DEBUG ((DEBUG_ERROR, "UsbFn: connected, %a speed\n",
          (Dev->Speed == UsbBusSpeedHigh) ? "high" : "full"));

  //
  // USB 2.0 link: the SuperSpeed PHY is idle (exynos_ss_udc_irq_connectdone,
  // including WORKAROUND TD 7.23: TX_EN override off).
  //
  MmioOr32 (REG (GUSB3PIPECTL), GUSB3PIPECTL_SUSPSSPHY);
  UsbBoardPhyCrWrite (0x1000, 0x0);

  DepCmd (0, DEPCMD_DEPCFG, DEPCFG0_MPS (USBFN_EP0_MPS) | DEPCFG0_CFGACTION (2),
          DEPCFG1_XFERNRDYEN | DEPCFG1_XFERCMPLEN, 0);
  DepCmd (1, DEPCMD_DEPCFG, DEPCFG0_MPS (USBFN_EP0_MPS) | DEPCFG0_CFGACTION (2),
          DEPCFG1_EPDIR | DEPCFG1_XFERNRDYEN | DEPCFG1_XFERCMPLEN, 0);

  ZeroMem (&Payload, sizeof (Payload));
  Payload.ubs = Dev->Speed;
  UsbFnQueueEvent (Dev, EfiUsbMsgBusEventSpeed, &Payload);
}

STATIC
VOID
HandleDeviceEvent (
  IN USBFN_DEV  *Dev,
  IN UINT32     Event
  )
{
  switch (DEVT_TYPE (Event)) {
    case DEVT_USBRST:
      OnBusReset (Dev);
      break;

    case DEVT_CONNECTDONE:
      OnConnectDone (Dev);
      break;

    case DEVT_DISCONN:
      DEBUG ((DEBUG_ERROR, "UsbFn: disconnected\n"));
      DeactivateEndpoints (Dev);
      UsbFnQueueEvent (Dev, EfiUsbMsgBusEventDetach, NULL);
      break;

    case DEVT_ULSTCHNG:
      if (DEVT_LINKSTATE (Event) == LINKSTATE_U3) {
        UsbFnQueueEvent (Dev, EfiUsbMsgBusEventSuspend, NULL);
      } else if (DEVT_LINKSTATE (Event) == LINKSTATE_U0) {
        UsbFnQueueEvent (Dev, EfiUsbMsgBusEventResume, NULL);
      }
      break;

    case DEVT_EVNTOVERFLOW:
      DEBUG ((DEBUG_ERROR, "UsbFn: event buffer overflow\n"));
      break;

    default:
      break;
  }
}

STATIC
VOID
HandleEpEvent (
  IN USBFN_DEV  *Dev,
  IN UINT32     Event
  )
{
  UINT8  Phys;

  Phys = (UINT8)DEPEVT_EPNUM (Event);
  if (Phys >= DWC3_PHYS_EPS) {
    return;
  }

  switch (DEPEVT_TYPE (Event)) {
    case DEPEVT_XFERCOMPLETE:
      if (Phys < 2) {
        Ep0XferComplete (Dev, Phys);
      } else {
        EpXferComplete (Dev, Phys);
      }
      break;

    case DEPEVT_XFERNOTREADY:
      if (Phys < 2) {
        Ep0XferNotReady (Dev, Phys, Event);
      }
      break;

    default:
      break;
  }
}

BOOLEAN
Dwc3EventPending (
  VOID
  )
{
  return (MmioRead32 (REG (GEVNTCOUNT)) & 0xFFFC) != 0;
}

VOID
Dwc3ServiceEvents (
  IN USBFN_DEV  *Dev
  )
{
  UINT32  Count;
  UINT32  Event;
  UINTN   Rounds;

  for (Rounds = 0; Rounds < 8; Rounds++) {
    Count = MmioRead32 (REG (GEVNTCOUNT)) & 0xFFFC;
    if (Count == 0) {
      return;
    }

    InvalidateDataCacheRange (Dev->EventBuffer, DWC3_EVENT_BUFFER_SIZE);
    while (Count != 0) {
      Event = Dev->EventBuffer[Dev->EventIndex / 4];
      Dev->EventIndex = (Dev->EventIndex + 4) % DWC3_EVENT_BUFFER_SIZE;

      if (!EVT_IS_DEVICE (Event)) {
        HandleEpEvent (Dev, Event);
      } else if (EVT_DEVT_TYPE (Event) == 0) {
        HandleDeviceEvent (Dev, Event);
      }

      MmioWrite32 (REG (GEVNTCOUNT), 4);
      Count -= 4;
    }
  }
}

//
// ---------------------------------------------------------------------------
// Bring-up.
// ---------------------------------------------------------------------------
//

/**
  exynos_drd_init() + exynos_drd_phy_set().
**/
STATIC
EFI_STATUS
DrdInit (
  IN USBFN_DEV  *Dev
  )
{
  UINT32  Id;
  UINT32  Reg;

  Id = MmioRead32 (REG (GSNPSID));
  if ((Id >> 16) != 0x5533) {
    DEBUG ((DEBUG_ERROR, "UsbFn: no DWC3 at %x (GSNPSID %08x)\n", DWC3_BASE, Id));
    return EFI_NOT_FOUND;
  }
  Dev->Release = (UINT16)Id;
  if (Dev->Release == 0x131a) {
    Dev->Release = 0x180a;   // 1.80a reports a wrong release number
  }
  DEBUG ((DEBUG_ERROR, "UsbFn: DWC3 release %x\n", Dev->Release));

  Reg  = MmioRead32 (REG (GCTL));
  Reg &= ~(GCTL_PWRDNSCALE_MASK | GCTL_DISSCRAMBLE | GCTL_RAMCLKSEL_MASK);
  Reg |= GCTL_PWRDNSCALE (SUSPEND_CLK_HZ / 16000) | GCTL_U2RSTECN;
  MmioWrite32 (REG (GCTL), Reg);

  //
  // PHY interface reset around the PHY init.
  //
  MmioOr32 (REG (GCTL), GCTL_CORESOFTRESET);
  MmioOr32 (REG (GUSB2PHYCFG), GUSB2PHYCFG_PHYSOFTRST);
  MmioOr32 (REG (GUSB3PIPECTL), GUSB3PIPECTL_PHYSOFTRST);

  UsbBoardPhyInit ();
  UsbBoardPhyTune ();

  MmioAnd32 (REG (GUSB2PHYCFG), ~GUSB2PHYCFG_PHYSOFTRST);
  MmioAnd32 (REG (GUSB3PIPECTL), ~GUSB3PIPECTL_PHYSOFTRST);
  MmioAnd32 (REG (GCTL), ~GCTL_CORESOFTRESET);

  MmioAndThenOr32 (REG (GUSB2PHYCFG),
                   ~(GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM | GUSB2PHYCFG_TRDTIM_MASK),
                   GUSB2PHYCFG_TRDTIM (9));
  MmioAnd32 (REG (GUSB3PIPECTL), ~GUSB3PIPECTL_SUSPSSPHY);
  return EFI_SUCCESS;
}

/**
  exynos_drd_config(): bus setup, release workarounds, PHY tuning.
**/
STATIC
VOID
DrdConfig (
  IN USBFN_DEV  *Dev
  )
{
  UINT32  Reg;

  MmioWrite32 (REG (GSBUSCFG0), GSBUSCFG0_INCR16 | GSBUSCFG0_INCR);
  MmioWrite32 (REG (GSBUSCFG1), GSBUSCFG1_BREQLIMIT (3));

  if ((Dev->Release >= 0x190a) && (Dev->Release <= 0x210a)) {
    MmioOr32 (REG (GUCTL), GUCTL_HSTINAUTORETRY);
    Reg  = MmioRead32 (REG (GRXTHRCFG));
    Reg &= ~(GRXTHRCFG_PKTCNT_MASK | GRXTHRCFG_MAXBURST_MASK);
    Reg |= GRXTHRCFG_PKTCNTSEL | GRXTHRCFG_PKTCNT (3) | GRXTHRCFG_MAXBURST (3);
    MmioWrite32 (REG (GRXTHRCFG), Reg);
  }

  if (Dev->Release <= 0x210a) {
    MmioAndThenOr32 (REG (GUSB3PIPECTL), ~GUSB3PIPECTL_DELAYP1P2P3_MASK,
                     GUSB3PIPECTL_DELAYP1P2P3 (1));
    MmioOr32 (REG (GUSB2PHYCFG), GUSB2PHYCFG_PHYIF);
  }

  //
  // los_bias, LOW_VBOOST quirk, rx eq 4 (CONFIG_V1A).
  //
  UsbBoardPhyCrWrite (0x15, 0xA409);
  UsbBoardPhyCrWrite (0x12, 0x8000);
  UsbBoardPhyCrWrite (0x1006, 0xC80);
}

/**
  Clocks, PHY, core reset and device-mode setup; EP0 armed, not connected.
**/
EFI_STATUS
Dwc3CoreInit (
  IN USBFN_DEV  *Dev
  )
{
  EFI_STATUS  Status;

  Status = UsbBoardClockInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Whatever the bootloader left running: stop it first.
  //
  MmioAnd32 (REG (DCTL), ~DCTL_RUNSTOP);
  PollSet (REG (DSTS), DSTS_DEVCTRLHLT, 10000);

  Status = DrdInit (Dev);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Device role, then device core soft reset (exynos_ss_udc_enable).
  //
  MmioAndThenOr32 (REG (GCTL), ~GCTL_PRTCAPDIR_MASK, GCTL_PRTCAPDIR_DEVICE);
  MmioOr32 (REG (DCTL), DCTL_CSFTRST);
  if (!PollClear (REG (DCTL), DCTL_CSFTRST, 100000)) {
    DEBUG ((DEBUG_ERROR, "UsbFn: DCTL.CSftRst stuck\n"));
    return EFI_DEVICE_ERROR;
  }

  DrdConfig (Dev);

  //
  // High speed only (USB 2.0 descriptors), no LPM.
  //
  MmioWrite32 (REG (DCFG), DCFG_NUMP (3) | DCFG_PERFRINT (2) | DCFG_DEVSPD_HS);

  //
  // Event buffer 0.
  //
  Dev->EventIndex = 0;
  MmioWrite32 (REG (GEVNTADRHI), 0);
  MmioWrite32 (REG (GEVNTADRLO), PtrToDma (Dev->EventBuffer));
  MmioWrite32 (REG (GEVNTSIZ), GEVNTSIZ_INTMASK | DWC3_EVENT_BUFFER_SIZE);
  MmioWrite32 (REG (GEVNTCOUNT), MmioRead32 (REG (GEVNTCOUNT)));
  MmioWrite32 (REG (GEVNTSIZ), DWC3_EVENT_BUFFER_SIZE);

  MmioWrite32 (REG (DEVTEN), DEVTEN_ULSTCNG | DEVTEN_CONNECTDONE |
                             DEVTEN_USBRST | DEVTEN_DISCONN);

  //
  // EP0 (physical 0 = OUT, 1 = IN).
  //
  DepCmd (0, DEPCMD_DEPSTARTCFG, 0, 0, 0);
  DepCmd (0, DEPCMD_DEPCFG, DEPCFG0_MPS (USBFN_EP0_MPS),
          DEPCFG1_XFERNRDYEN | DEPCFG1_XFERCMPLEN, 0);
  DepCmd (1, DEPCMD_DEPCFG, DEPCFG0_MPS (USBFN_EP0_MPS),
          DEPCFG1_EPDIR | DEPCFG1_XFERNRDYEN | DEPCFG1_XFERCMPLEN, 0);
  DepCmd (0, DEPCMD_DEPXFERCFG, DEPXFERCFG0_NUMXFERRES (1), 0, 0);
  DepCmd (1, DEPCMD_DEPXFERCFG, DEPXFERCFG0_NUMXFERRES (1), 0, 0);
  MmioWrite32 (REG (DALEPENA), 0x3);

  ZeroMem (Dev->In, sizeof (Dev->In));
  ZeroMem (Dev->Out, sizeof (Dev->Out));
  ZeroMem (Dev->Halted, sizeof (Dev->Halted));
  Dev->EpsConfigured = FALSE;
  Dev->Configuration = 0;
  Ep0ArmSetup (Dev);

  DEBUG ((DEBUG_ERROR, "UsbFn: core up, GCTL %08x DCFG %08x GUSB2PHYCFG %08x\n",
          MmioRead32 (REG (GCTL)), MmioRead32 (REG (DCFG)),
          MmioRead32 (REG (GUSB2PHYCFG))));
  return EFI_SUCCESS;
}

/**
  Soft connect / disconnect (DCTL.RunStop).
**/
VOID
Dwc3Connect (
  IN USBFN_DEV  *Dev,
  IN BOOLEAN    Connect
  )
{
  UINTN  Try;

  if (Connect) {
    MmioOr32 (REG (DCTL), DCTL_RUNSTOP);
    if (!PollClear (REG (DSTS), DSTS_DEVCTRLHLT, 1000)) {
      DEBUG ((DEBUG_ERROR, "UsbFn: controller did not start\n"));
    }
    return;
  }

  MmioAnd32 (REG (DCTL), ~DCTL_RUNSTOP);
  for (Try = 0; Try < 10; Try++) {
    //
    // The core only halts with an empty event buffer.
    //
    MmioWrite32 (REG (GEVNTCOUNT), MmioRead32 (REG (GEVNTCOUNT)) & 0xFFFC);
    if (PollSet (REG (DSTS), DSTS_DEVCTRLHLT, 100)) {
      break;
    }
  }
}

/**
  ExitBootServices / StopController: disconnect and stop all DMA into the
  pool (which the OS is about to reclaim).
**/
VOID
Dwc3Shutdown (
  IN USBFN_DEV  *Dev
  )
{
  Dwc3Connect (Dev, FALSE);
  MmioWrite32 (REG (DEVTEN), 0);
  MmioWrite32 (REG (GEVNTSIZ), GEVNTSIZ_INTMASK);
  MmioWrite32 (REG (GEVNTCOUNT), MmioRead32 (REG (GEVNTCOUNT)) & 0xFFFC);
  MmioWrite32 (REG (GEVNTADRLO), 0);
  Dev->Ep0State = Ep0StateIdle;
}
