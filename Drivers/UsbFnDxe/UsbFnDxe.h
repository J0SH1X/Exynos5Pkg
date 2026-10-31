/** @file
  Exynos5420 USB peripheral driver: EFI_USBFN_IO_PROTOCOL on USB3.0 DRD
  channel 0 (Synopsys DWC3 at 0x12000000, PHY at 0x12100000).

  v1awifi wires the micro-USB data lines to DRD0 (the kernel's defconfig:
  CONFIG_USB_EXYNOS5_USB3_DRD_CH0=y, gadget = exynos-ss-udc).  The 5420 also
  has a DWC2 HSOTG block at 0x12140000, but nothing is connected to it.

  Peripheral mode only, forced to high speed (the descriptors UsbLogDxe
  publishes are USB 2.0; no SuperSpeed companion descriptors).

  Interrupt driven: the controller interrupt drains the event buffer, runs
  the EP0 control state machine, answers the standard requests the driver
  owns and queues UsbFn messages; EventHandler() only dequeues them.

  Data moves by DMA through TRBs.  Every TRB, the event buffer and one
  bounce buffer per endpoint/direction live in a DMA pool that is mapped
  uncached when the CPU driver allows it; cache maintenance is done anyway,
  so a cached pool is still correct.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __USBFN_DXE_H__
#define __USBFN_DXE_H__

#include <Uefi.h>
#include <IndustryStandard/Usb.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/HardwareInterrupt.h>
#include <Protocol/UsbFunctionIo.h>

//
// Controller base and interrupt (map.h EXYNOS5_PA_USB3_DRD0 = 0x12000000,
// irqs.h EXYNOS5_IRQ_USB3_DRD0 = SPI 72 -> GIC INTID 104).
//
#define DWC3_BASE      0x12000000
#define DWC3_PHY_BASE  0x12100000
#define DWC3_IRQ       (32 + 72)

//
// DWC3 global registers (exynos-drd.h).
//
#define GSBUSCFG0             0xC100
#define GSBUSCFG1             0xC104
#define GRXTHRCFG             0xC10C
#define GCTL                  0xC110
#define GSNPSID               0xC120
#define GUCTL                 0xC12C
#define GUSB2PHYCFG           0xC200
#define GUSB3PIPECTL          0xC2C0
#define GEVNTADRLO            0xC400
#define GEVNTADRHI            0xC404
#define GEVNTSIZ              0xC408
#define GEVNTCOUNT            0xC40C

#define GSBUSCFG0_INCR16      (1U << 3)
#define GSBUSCFG0_INCR        (1U << 0)
#define GSBUSCFG1_BREQLIMIT(x) ((x) << 8)

#define GRXTHRCFG_PKTCNTSEL   (1U << 29)
#define GRXTHRCFG_PKTCNT(x)   ((x) << 24)
#define GRXTHRCFG_PKTCNT_MASK (0xFU << 24)
#define GRXTHRCFG_MAXBURST(x) ((x) << 19)
#define GRXTHRCFG_MAXBURST_MASK (0x1FU << 19)

#define GCTL_PWRDNSCALE(x)    ((x) << 19)
#define GCTL_PWRDNSCALE_MASK  (0x1FFFU << 19)
#define GCTL_U2RSTECN         (1U << 16)
#define GCTL_PRTCAPDIR_MASK   (0x3U << 12)
#define GCTL_PRTCAPDIR_DEVICE (0x2U << 12)
#define GCTL_CORESOFTRESET    (1U << 11)
#define GCTL_RAMCLKSEL_MASK   (0x3U << 6)
#define GCTL_DISSCRAMBLE      (1U << 3)

#define GUCTL_HSTINAUTORETRY  (1U << 14)

#define GUSB2PHYCFG_PHYSOFTRST (1U << 31)
#define GUSB2PHYCFG_TRDTIM(x)  ((x) << 10)
#define GUSB2PHYCFG_TRDTIM_MASK (0xFU << 10)
#define GUSB2PHYCFG_ENBLSLPM   (1U << 8)
#define GUSB2PHYCFG_SUSPHY     (1U << 6)
#define GUSB2PHYCFG_PHYIF      (1U << 3)

#define GUSB3PIPECTL_PHYSOFTRST (1U << 31)
#define GUSB3PIPECTL_DELAYP1P2P3(x) ((x) << 19)
#define GUSB3PIPECTL_DELAYP1P2P3_MASK (0x7U << 19)
#define GUSB3PIPECTL_SUSPSSPHY  (1U << 17)

#define GEVNTSIZ_INTMASK      (1U << 31)

//
// DWC3 device registers (exynos_ss_udc.h).
//
#define DCFG                  0xC700
#define DCTL                  0xC704
#define DEVTEN                0xC708
#define DSTS                  0xC70C
#define DGCMDPAR              0xC710
#define DGCMD                 0xC714
#define DALEPENA              0xC720
#define DEPCMDPAR2(n)         (0xC800 + (n) * 0x10)
#define DEPCMDPAR1(n)         (0xC804 + (n) * 0x10)
#define DEPCMDPAR0(n)         (0xC808 + (n) * 0x10)
#define DEPCMD(n)             (0xC80C + (n) * 0x10)

#define DCFG_NUMP(x)          ((x) << 17)
#define DCFG_PERFRINT(x)      ((x) << 10)
#define DCFG_DEVADDR_MASK     (0x7FU << 3)
#define DCFG_DEVADDR(x)       ((x) << 3)
#define DCFG_DEVSPD_HS        0
#define DCFG_DEVSPD_MASK      0x7U

#define DCTL_RUNSTOP          (1U << 31)
#define DCTL_CSFTRST          (1U << 30)
#define DCTL_TSTCTL_MASK      (0xFU << 1)

#define DEVTEN_ULSTCNG        (1U << 3)
#define DEVTEN_CONNECTDONE    (1U << 2)
#define DEVTEN_USBRST         (1U << 1)
#define DEVTEN_DISCONN        (1U << 0)

#define DSTS_DEVCTRLHLT       (1U << 22)
#define DSTS_CONNECTSPD(x)    ((x) & 0x7)
#define CONNECTSPD_HS         0
#define CONNECTSPD_FS         1
#define CONNECTSPD_LS         2
#define CONNECTSPD_FS48       3
#define CONNECTSPD_SS         4

#define DGCMD_CMDACT          (1U << 10)
#define DGCMD_ALLFIFOFLUSH    0x0A

#define DEPCMD_PARAM(x)       ((x) << 16)
#define DEPCMD_XFERRSCIDX(x)  (((x) >> 16) & 0x7F)
#define DEPCMD_STATUS(x)      (((x) >> 12) & 0xF)
#define DEPCMD_HIPRI_FORCERM  (1U << 11)
#define DEPCMD_CMDACT         (1U << 10)
#define DEPCMD_CMDIOC         (1U << 8)
#define DEPCMD_DEPCFG         0x1
#define DEPCMD_DEPXFERCFG     0x2
#define DEPCMD_DEPSSTALL      0x4
#define DEPCMD_DEPCSTALL      0x5
#define DEPCMD_DEPSTRTXFER    0x6
#define DEPCMD_DEPENDXFER     0x8
#define DEPCMD_DEPSTARTCFG    0x9

#define DEPCFG0_CFGACTION(x)  ((x) << 30)
#define DEPCFG0_FIFONUM(x)    ((x) << 17)
#define DEPCFG0_MPS(x)        ((x) << 3)
#define DEPCFG0_EPTYPE(x)     ((x) << 1)
#define DEPCFG1_EPNUM(x)      ((x) << 26)
#define DEPCFG1_EPDIR         (1U << 25)
#define DEPCFG1_XFERNRDYEN    (1U << 10)
#define DEPCFG1_XFERCMPLEN    (1U << 8)
#define DEPXFERCFG0_NUMXFERRES(x) (x)

//
// TRB (16 bytes, 16-byte aligned).
//
typedef struct {
  UINT32  BufLo;
  UINT32  BufHi;
  UINT32  Size;     // BUFSIZ[23:0], TRBSTS[31:28]
  UINT32  Ctrl;
} DWC3_TRB;

#define TRB_SIZE_MASK         0x00FFFFFFU
#define TRB_CTRL_IOC          (1U << 11)
#define TRB_CTRL_ISP_IMI      (1U << 10)
#define TRB_CTRL_TRBCTL(x)    ((x) << 4)
#define TRB_CTRL_CHN          (1U << 2)
#define TRB_CTRL_LST          (1U << 1)
#define TRB_CTRL_HWO          (1U << 0)

#define TRBCTL_NORMAL         1
#define TRBCTL_CONTROL_SETUP  2
#define TRBCTL_CONTROL_STATUS2 3
#define TRBCTL_CONTROL_STATUS3 4
#define TRBCTL_CONTROL_DATA   5

//
// Events.
//
#define EVT_IS_DEVICE(e)      (((e) & 1) != 0)
#define EVT_DEVT_TYPE(e)      (((e) >> 1) & 0x7F)   // 0 = device event

#define DEPEVT_EPNUM(e)       (((e) >> 1) & 0x1F)
#define DEPEVT_TYPE(e)        (((e) >> 6) & 0xF)
#define DEPEVT_XFERCOMPLETE   1
#define DEPEVT_XFERNOTREADY   3
#define DEPEVT_CMDCMPLT       7
#define DEPEVT_STATUS(e)      (((e) >> 12) & 0xF)
#define DEPEVT_CTL_STATUS(e)  (((e) >> 12) & 0x3)
#define CTL_STATUS_SETUP      0
#define CTL_STATUS_DATA       1
#define CTL_STATUS_STATUS     2

#define DEVT_TYPE(e)          (((e) >> 8) & 0xF)
#define DEVT_DISCONN          0
#define DEVT_USBRST           1
#define DEVT_CONNECTDONE      2
#define DEVT_ULSTCHNG         3
#define DEVT_WKUP             4
#define DEVT_EVNTOVERFLOW     11
#define DEVT_LINKSTATE(e)     (((e) >> 16) & 0xF)
#define LINKSTATE_U3          3     // HS: suspend (L2)
#define LINKSTATE_U0          0     // HS: on (L0)

//
// Bring-up self test (SelfTest.c): the driver starts itself and runs a bulk
// loopback device.  Off: UsbLogDxe (and later KDNET) is the client.
//
#ifndef USBFN_SELFTEST
#define USBFN_SELFTEST         0
#endif

#define USBFN_MAX_EPS          4      // EP0 + bulk endpoint numbers 1..3
#define USBFN_EP0_MPS          64
#define USBFN_MAX_TRANSFER     (64 * 1024)
#define USBFN_EVENT_QUEUE      32
#define USBFN_EP0_BUFFER       512

//
// Identity reported by GetVendorIdProductId / GetDeviceInfo.  pid.codes test
// VID/PID: development only, the class driver may override them in its own
// device descriptor.
//
#define USBFN_VID              0x1209
#define USBFN_PID              0x0001

//
// DMA pool layout (one allocation, page aligned).  Each TRB pair gets its own
// 64-byte cache line so a clean of one never touches another the core owns.
//
#define DWC3_EVENT_BUFFER_SIZE 4096
#define DWC3_TRB_SLOT          64
#define DWC3_PHYS_EPS          (USBFN_MAX_EPS * 2)
#define DWC3_BOUNCE_SIZE       USBFN_MAX_TRANSFER

//
// EP0 control transfer state.
//
typedef enum {
  Ep0StateIdle = 0,         // no SETUP armed (before start / after stop)
  Ep0StateSetup,            // SETUP TRB armed on physical EP0
  Ep0StateData,             // data stage expected / in flight
  Ep0StateWaitStatus,       // waiting for XferNotReady(Status)
  Ep0StateStatus            // status TRB in flight
} EP0_STATE;

//
// One transfer per endpoint and direction.
//
typedef struct {
  BOOLEAN    Active;      // client transfer owned by this endpoint
  BOOLEAN    Internal;    // EP0 data stage owned by the driver, not reported
  BOOLEAN    SendZlp;     // EP0 IN: terminate a max-packet-multiple reply
  BOOLEAN    Started;     // TRB handed to the core (StartXfer issued)
  UINT8      *Buffer;     // client buffer
  UINTN      Length;      // client length
  UINTN      Programmed;  // bytes in the TRB (OUT: rounded up to MPS)
  UINTN      Done;        // bytes actually moved
  UINT8      Resource;    // XferRscIdx from StartXfer, for EndXfer
} USBFN_XFER;

typedef struct {
  EFI_USBFN_MESSAGE          Message;
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;
} USBFN_EVENT;

typedef struct {
  UINT32                    Signature;
  EFI_USBFN_IO_PROTOCOL     UsbFnIo;
  EFI_HANDLE                Handle;
  EFI_HARDWARE_INTERRUPT_PROTOCOL *Interrupt;
  EFI_EVENT                 ExitBootServices;

  BOOLEAN                   Started;
  EFI_USB_BUS_SPEED         Speed;
  UINT8                     Configuration;
  BOOLEAN                   EpsConfigured;    // DEPCFG done for EP1..3
  UINT16                    Release;          // GSNPSID release (e.g. 0x210a)

  EFI_USB_DEVICE_INFO       *DeviceInfo;
  UINT8                     *ConfigBlob;      // full configuration descriptor
  UINT16                    ConfigBlobLength;

  USBFN_XFER                In[USBFN_MAX_EPS];
  USBFN_XFER                Out[USBFN_MAX_EPS];
  UINT8                     EpType[USBFN_MAX_EPS];   // USB_ENDPOINT_* per number
  UINT16                    EpMps[USBFN_MAX_EPS];
  UINT8                     EpDirs[USBFN_MAX_EPS];   // EP_DIR_OUT | EP_DIR_IN
  BOOLEAN                   Halted[DWC3_PHYS_EPS];

  //
  // EP0 control state machine.
  //
  EP0_STATE                 Ep0State;
  BOOLEAN                   Ep0ThreeStage;    // wLength != 0
  BOOLEAN                   Ep0DirIn;         // data stage direction
  BOOLEAN                   Ep0StatusReady;   // status may be started
  BOOLEAN                   Ep0StatusPending; // XferNotReady(Status) seen
  UINT8                     Ep0StatusPhys;    // physical EP of that event
  UINT8                     Ep0Buffer[USBFN_EP0_BUFFER];   // driver replies

  //
  // DMA pool.
  //
  UINT8                     *Pool;
  UINTN                     PoolPages;
  UINT32                    *EventBuffer;
  UINTN                     EventIndex;       // next event, in bytes
  DWC3_TRB                  *Trb[DWC3_PHYS_EPS];      // 2 TRBs per slot
  UINT8                     *Bounce[DWC3_PHYS_EPS];   // EP0: USBFN_EP0_BUFFER
  UINT8                     *SetupBuffer;             // 8-byte SETUP target

  USBFN_EVENT               Queue[USBFN_EVENT_QUEUE];
  UINTN                     QueueHead;
  UINTN                     QueueTail;
  UINTN                     QueueDropped;

  UINTN                     IrqCount;         // controller interrupts taken
  UINTN                     PollCount;        // serviced from EventHandler
} USBFN_DEV;

#define USBFN_SIGNATURE    SIGNATURE_32 ('U', 'F', 'N', 'D')
#define USBFN_FROM_THIS(a) CR (a, USBFN_DEV, UsbFnIo, USBFN_SIGNATURE)

#define PHYS_EP(Ep, In)    ((UINT8)(((Ep) << 1) | ((In) ? 1 : 0)))
#define EP_DIR_OUT         (1U << 0)
#define EP_DIR_IN          (1U << 1)

//
// Board.c: clocks + DRD0 PHY.
//
EFI_STATUS
UsbBoardClockInit (
  VOID
  );

VOID
UsbBoardPhyInit (
  VOID
  );

VOID
UsbBoardPhyTune (
  VOID
  );

VOID
UsbBoardPhyCrWrite (
  IN UINT16  Address,
  IN UINT16  Data
  );

VOID
UsbBoardPhyExit (
  VOID
  );

//
// Dwc3.c
//
EFI_STATUS
Dwc3AllocatePool (
  IN USBFN_DEV  *Dev
  );

EFI_STATUS
Dwc3CoreInit (
  IN USBFN_DEV  *Dev
  );

VOID
Dwc3Connect (
  IN USBFN_DEV  *Dev,
  IN BOOLEAN    Connect
  );

VOID
Dwc3Shutdown (
  IN USBFN_DEV  *Dev
  );

BOOLEAN
Dwc3EventPending (
  VOID
  );

VOID
Dwc3ServiceEvents (
  IN USBFN_DEV  *Dev
  );

VOID
Dwc3SetAddress (
  IN UINT8  Address
  );

VOID
Dwc3ActivateEndpoints (
  IN USBFN_DEV  *Dev
  );

EFI_STATUS
Dwc3StartTransfer (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In
  );

VOID
Dwc3SetStall (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In,
  IN BOOLEAN    Stall
  );

BOOLEAN
Dwc3GetStall (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In
  );

VOID
Dwc3Ep0Stall (
  IN USBFN_DEV  *Dev
  );

VOID
Dwc3Ep0StatusReady (
  IN USBFN_DEV  *Dev
  );

VOID
Dwc3AbortEp (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In
  );

//
// UsbFnDxe.c: callbacks from the event handler.
//
VOID
UsbFnQueueEvent (
  IN USBFN_DEV                        *Dev,
  IN EFI_USBFN_MESSAGE                Message,
  IN CONST EFI_USBFN_MESSAGE_PAYLOAD  *Payload OPTIONAL
  );

VOID
UsbFnQueueTransfer (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In
  );

VOID
UsbFnEp0Setup (
  IN USBFN_DEV                     *Dev,
  IN CONST EFI_USB_DEVICE_REQUEST  *Req
  );

#endif // __USBFN_DXE_H__
