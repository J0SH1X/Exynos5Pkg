/** @file
  Broadcom BCM4354 (FullMAC, SDIO) host side, shared by UEFI (WlanTestApp)
  and Windows (ExynosWinDrivers/exynoswlan), built unchanged in both through
  the DesignWare host layer (../SdCardDxe/MmcHost.h, <MmcEnv.h>).

  The board (power, pins, clocks) is the caller's; this code starts at a
  powered chip and a clocked dw_mmc channel:

    BcmSdioAttach     SDIO enumeration, 4-bit 20 MHz, functions 1+2, block
                      sizes, ALP clock, chip id, core scan, Cortex-R4 RAM size
    BcmSdioDownload   firmware + NVRAM into RAM, start the Cortex-R4, wait for
                      the shared structure
    BcmSdioBusInit    F2 (frames) up, host interrupt mask, firmware ready
    BcmSdioSend / BcmSdioPoll    SDPCM frames
    BcmIoctl / BcmIovarGet / BcmIovarSet    BCDC control on channel 0

  Data is PIO through the controller's 64-bit FIFO (the IDMAC stalls on
  small SDIO transfers); frames are signalled by the card's in-band
  interrupt (BcmSdioEnableInterrupts).  One caller at a time, apart from
  BcmSdioInterruptIsr().

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __BCM_SDIO_H__
#define __BCM_SDIO_H__

#include "../SdCardDxe/MmcHost.h"

#define BCM_MAX_CORES       16
#define BCM_FRAME_MAX       4096       // largest SDPCM frame handled
#define BCM_IOCTL_MAX       1536       // ioctl payload (BCDC: ETHER_MAX_LEN)
#define BCM_EAPOL_MAX       512        // queued EAPOL frame (ether header on)
#define BCM_GLOM_MAX        32768      // largest superframe (glommed frames)
#define BCM_GLOM_FRAMES     32

//
// SDPCM channels.
//
#define BCM_CHANNEL_CONTROL 0
#define BCM_CHANNEL_EVENT   1
#define BCM_CHANNEL_DATA    2
#define BCM_CHANNEL_GLOM    3

//
// Selected WLC ioctls (wlioctl_defs.h) and constants.
//
#define WLC_UP              2
#define WLC_DOWN            3
#define WLC_SET_INFRA       20
#define WLC_SET_AUTH        22
#define WLC_GET_BSSID       23
#define WLC_SET_SSID        26
#define WLC_DISASSOC        52
#define WLC_SET_PM          86
#define WLC_GET_RSSI        127
#define WLC_SET_WSEC        134
#define WLC_SET_WPA_AUTH    165
#define WLC_GET_VAR         262
#define WLC_SET_VAR         263
#define WLC_SET_WSEC_PMK    268

#define TKIP_ENABLED        0x0002
#define AES_ENABLED         0x0004
#define WPA2_AUTH_PSK       0x0080
#define WSEC_PASSPHRASE     0x0001

//
// Events (bcmevent.h): ether header (14) + bcmeth header (10), then the
// big-endian wl_event_msg_t.
//
#define BCM_EVENT_MSG_OFFSET  24
#define WLC_E_SET_SSID      0
#define WLC_E_DEAUTH_IND    6
#define WLC_E_DISASSOC_IND  12
#define WLC_E_LINK          16
#define WLC_E_PSK_SUP       46
#define WLC_EVENT_MSG_LINK  0x01

typedef struct {
  UINT32  Id;
  UINT32  Rev;
  UINT32  Base;
  UINT32  Wrap;
} BCM_CORE;

typedef struct _BCM_SDIO BCM_SDIO;

/**
  Called for every frame BcmSdioPoll() takes off the bus that is not the
  answer to the ioctl being waited for: events (channel 1, the BDC payload
  starting at the ether header) and data (channel 2, the 802.3 frame).
**/
typedef
VOID
(*BCM_RX_HANDLER) (
  IN VOID    *Context,
  IN UINT8   Channel,
  IN UINT8   *Payload,
  IN UINT32  Length
  );

struct _BCM_SDIO {
  MMC_HOST        Host;
  UINT16          Rca;
  UINT32          Window;
  UINT32          ChipId;
  UINT32          ChipRev;
  BCM_CORE        Cores[BCM_MAX_CORES];
  UINT32          CoreCount;
  UINT32          EromPtr;
  BCM_CORE        *Cr4;
  BCM_CORE        *Sdiod;
  UINT32          RamBase;
  UINT32          RamSize;
  UINT32          Shared;          // firmware's sdpcm_shared address

  UINT8           TxSeq;
  UINT8           TxMax;           // credit from the dongle
  UINT16          IoctlId;
  UINT32          Frames;          // received, for the logs
  UINT32          Errors;
  UINT32          RxFails;         // frames dropped with a terminate (resync)
  UINT32          Gloms;           // superframes received

  //
  // Glom descriptor (channel 3): the lengths of the frames of the next read.
  //
  UINT32          GlomCount;
  UINT16          GlomLen[BCM_GLOM_FRAMES];

  BCM_RX_HANDLER  RxHandler;
  VOID            *RxContext;

  //
  // Answer of the ioctl being waited for (BcmIoctl).
  //
  BOOLEAN         IoctlDone;
  BOOLEAN         IoctlFailed;
  UINT32          IoctlStatus;
  UINT32          IoctlLength;
  UINT8           IoctlBuf[BCM_IOCTL_MAX];

  //
  // Host side WPA2-PSK supplicant (BcmWpa.c).  Dispatch() queues one EAPOL
  // frame; BcmWpaService() answers it.
  //
  BOOLEAN         WpaActive;
  BOOLEAN         WpaDone;         // keys installed
  UINT8           WpaMac[6];
  UINT8           WpaBssid[6];
  UINT8           WpaPmk[32];
  UINT8           WpaPtk[48];      // KCK | KEK | TK
  UINT8           WpaANonce[32];
  UINT8           WpaSNonce[32];
  UINT8           WpaSeed[32];
  UINT32          WpaNonceCount;
  UINT8           WpaRsnIe[64];
  UINT32          WpaRsnIeLen;
  UINT32          EapolLength;
  UINT8           Eapol[BCM_EAPOL_MAX];

  UINT8           TxBuf[BCM_FRAME_MAX];
  UINT8           RxBuf[BCM_FRAME_MAX];
  UINT8           GlomBuf[BCM_GLOM_MAX];
};

EFI_STATUS
BcmSdioAttach (
  IN OUT BCM_SDIO  *Bcm,
  IN     UINT32    HostBase,
  IN     UINT32    CiuClockHz
  );

EFI_STATUS
BcmSdioDownload (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST UINT8  *Firmware,
  IN     UINT32       FirmwareSize,
  IN     CONST CHAR8  *NvramText,
  IN     UINT32       NvramTextSize
  );

EFI_STATUS
BcmSdioBusInit (
  IN OUT BCM_SDIO  *Bcm
  );

/**
  One frame on Channel: SDPCM header + Payload (BDC header included for
  data frames: the caller builds it).
**/
EFI_STATUS
BcmSdioSend (
  IN OUT BCM_SDIO     *Bcm,
  IN     UINT8        Channel,
  IN     CONST UINT8  *Payload,
  IN     UINT32       Length
  );

/**
  Take every pending frame off the bus and dispatch it (ioctl answers to
  BcmIoctl, the rest to the RX handler).  Returns the number of frames.
**/
UINT32
BcmSdioPoll (
  IN OUT BCM_SDIO  *Bcm
  );

/**
  BCDC ioctl: Buffer holds the request and receives the answer (up to
  Length bytes).  Waits up to TimeoutMs, dispatching other frames.
**/
EFI_STATUS
BcmIoctl (
  IN OUT BCM_SDIO  *Bcm,
  IN     UINT32    Cmd,
  IN     BOOLEAN   Set,
  IN OUT VOID      *Buffer,
  IN     UINT32    Length,
  IN     UINT32    TimeoutMs
  );

EFI_STATUS
BcmIovarGet (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Name,
  OUT    VOID         *Value,
  IN     UINT32       Length
  );

EFI_STATUS
BcmIovarSet (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Name,
  IN     CONST VOID   *Value,
  IN     UINT32       Length
  );

EFI_STATUS
BcmIovarSetInt (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Name,
  IN     UINT32       Value
  );

EFI_STATUS
BcmIoctlSetInt (
  IN OUT BCM_SDIO  *Bcm,
  IN     UINT32    Cmd,
  IN     UINT32    Value
  );

/**
  Join a WPA2-PSK network (pairwise CCMP; group CCMP or TKIP), or an open
  one with Passphrase NULL.  The firmware associates; the handshake runs in
  BcmWpaService() (BcmWpa.c) and the link state comes as events.
**/
EFI_STATUS
BcmJoin (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Ssid,
  IN     CONST CHAR8  *Passphrase  OPTIONAL
  );

/**
  WPA2-PSK (BcmWpa.c), used by BcmJoin(): PMK, MAC and RNG seed, then the
  handshake is answered from BcmWpaService().
**/
EFI_STATUS
BcmWpaSetup (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Ssid,
  IN     UINT32       SsidLen,
  IN     CONST CHAR8  *Passphrase
  );

/**
  Answer the queued EAPOL frames (handshake messages, group rekeys).  Call
  after BcmSdioPoll(), never from the RX handler.  TRUE when the keys have
  just been installed (the link carries data from now on).
**/
BOOLEAN
BcmWpaService (
  IN OUT BCM_SDIO  *Bcm
  );

/**
  Link lost: the next association does a new handshake.  Safe from the RX
  handler.
**/
VOID
BcmWpaLinkDown (
  IN OUT BCM_SDIO  *Bcm
  );

/**
  In-band card interrupt (DAT1) through the dw_mmc controller's line:
  enable it after BcmSdioBusInit().  The ISR half needs only the register
  base and masks the source; BcmSdioInterruptDone() acknowledges and
  unmasks once BcmSdioPoll() has drained the chip.
**/
VOID
BcmSdioEnableInterrupts (
  IN OUT BCM_SDIO  *Bcm
  );

BOOLEAN
BcmSdioInterruptIsr (
  IN UINT32  HostBase
  );

VOID
BcmSdioInterruptDone (
  IN OUT BCM_SDIO  *Bcm
  );

/**
  Install (Length > 0) or delete (Length 0) a key in the firmware
  (WLC_SET_KEY, wl_wsec_key_t): pairwise with Station set, group / default
  key Index otherwise; Rsc (6 bytes, little endian) primes the receive
  replay counter.  For a host-side supplicant that is not BcmWpa.c (the
  Windows Native Wi-Fi driver gets its keys from the OS).
**/
#define BCM_CRYPTO_ALGO_OFF      0
#define BCM_CRYPTO_ALGO_WEP1     1
#define BCM_CRYPTO_ALGO_TKIP     2
#define BCM_CRYPTO_ALGO_WEP128   3
#define BCM_CRYPTO_ALGO_AES_CCM  4

EFI_STATUS
BcmSetKey (
  IN OUT BCM_SDIO     *Bcm,
  IN     UINT32       Index,
  IN     CONST UINT8  *Key      OPTIONAL,
  IN     UINT32       Length,
  IN     UINT32       Algorithm,
  IN     CONST UINT8  *Rsc      OPTIONAL,
  IN     CONST UINT8  *Station  OPTIONAL
  );

/**
  CCCR interrupt pending (bit 1 = function 1, bit 2 = function 2), for
  diagnostics of the in-band interrupt.
**/
UINT8
BcmSdioIntPending (
  IN OUT BCM_SDIO  *Bcm
  );

/**
  The firmware's console ring, logged line by line (for diagnostics).
**/
VOID
BcmLogConsole (
  IN OUT BCM_SDIO  *Bcm
  );

#endif // __BCM_SDIO_H__
