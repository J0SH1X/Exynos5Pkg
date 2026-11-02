/** @file
  Host side WPA2-PSK supplicant for the BCM4354 (BcmSdio.c), shared by UEFI
  (WlanTestApp) and Windows (exynoswlan) like the rest of the core.

  The firmware (no "idsup" in its feature list) associates, builds the RSN
  IE from wsec / wpa_auth and the target BSS, and passes the EAPOL frames
  (ethertype 0x888E) up the data channel, exactly as for wpa_supplicant on
  Linux / Android.  The 4-way and group key handshakes run here:

    BcmJoin            PMK = PBKDF2-SHA1 (passphrase, SSID, 4096), RNG seed
    Dispatch()         queues an EAPOL frame (poll context: no bus access)
    BcmWpaService()    answers it (message 1 -> 2, 3 -> 4, group 1 -> 2) and
                       installs the keys (WLC_SET_KEY)

  Pairwise CCMP only (key descriptor version 2: HMAC-SHA1 MIC, AES key
  wrap); the group cipher may be CCMP or TKIP.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "BcmSdio.h"

#define WLC_SET_KEY           45

#define CRYPTO_ALGO_TKIP      2
#define CRYPTO_ALGO_AES_CCM   4

#define KI_VERSION_MASK       0x0007
#define KI_PAIRWISE           0x0008
#define KI_INSTALL            0x0040
#define KI_ACK                0x0080
#define KI_MIC                0x0100
#define KI_SECURE             0x0200
#define KI_ENCRYPTED          0x1000

//
// EAPOL-Key descriptor offsets (from the descriptor type byte, which
// follows the 4-byte 802.1X header).
//
#define EK_INFO               1
#define EK_KEYLEN             3
#define EK_REPLAY             5
#define EK_NONCE              13
#define EK_RSC                61
#define EK_MIC                77
#define EK_DATALEN            93
#define EK_DATA               95

#define ETH_HDR               14

STATIC CONST UINT8  mRsnOui[3] = { 0x00, 0x0F, 0xAC };

//
// ---------------------------------------------------------------------------
// SHA-1 / HMAC-SHA1 / PBKDF2 / PRF.
// ---------------------------------------------------------------------------
//

typedef struct {
  UINT32  H[5];
  UINT64  Bytes;
  UINT8   Block[64];
  UINT32  Fill;
} SHA1_CTX;

#define ROL32(x, n)  (((x) << (n)) | ((x) >> (32 - (n))))

STATIC
VOID
Sha1Block (
  IN OUT SHA1_CTX     *C,
  IN     CONST UINT8  *P
  )
{
  UINT32  W[80];
  UINT32  A, B, D, E, F, K, T, Cc;
  UINTN   I;

  for (I = 0; I < 16; I++) {
    W[I] = ((UINT32)P[4 * I] << 24) | ((UINT32)P[4 * I + 1] << 16) | ((UINT32)P[4 * I + 2] << 8) | P[4 * I + 3];
  }
  for (I = 16; I < 80; I++) {
    W[I] = ROL32 (W[I - 3] ^ W[I - 8] ^ W[I - 14] ^ W[I - 16], 1);
  }
  A  = C->H[0];
  B  = C->H[1];
  Cc = C->H[2];
  D  = C->H[3];
  E  = C->H[4];
  for (I = 0; I < 80; I++) {
    if (I < 20) {
      F = (B & Cc) | (~B & D);
      K = 0x5A827999;
    } else if (I < 40) {
      F = B ^ Cc ^ D;
      K = 0x6ED9EBA1;
    } else if (I < 60) {
      F = (B & Cc) | (B & D) | (Cc & D);
      K = 0x8F1BBCDC;
    } else {
      F = B ^ Cc ^ D;
      K = 0xCA62C1D6;
    }
    T  = ROL32 (A, 5) + F + E + K + W[I];
    E  = D;
    D  = Cc;
    Cc = ROL32 (B, 30);
    B  = A;
    A  = T;
  }
  C->H[0] += A;
  C->H[1] += B;
  C->H[2] += Cc;
  C->H[3] += D;
  C->H[4] += E;
}

STATIC
VOID
Sha1Init (
  OUT SHA1_CTX  *C
  )
{
  C->H[0]  = 0x67452301;
  C->H[1]  = 0xEFCDAB89;
  C->H[2]  = 0x98BADCFE;
  C->H[3]  = 0x10325476;
  C->H[4]  = 0xC3D2E1F0;
  C->Bytes = 0;
  C->Fill  = 0;
}

STATIC
VOID
Sha1Update (
  IN OUT SHA1_CTX     *C,
  IN     CONST UINT8  *P,
  IN     UINTN        Len
  )
{
  C->Bytes += Len;
  while (Len > 0) {
    C->Block[C->Fill++] = *P++;
    Len--;
    if (C->Fill == 64) {
      Sha1Block (C, C->Block);
      C->Fill = 0;
    }
  }
}

STATIC
VOID
Sha1Final (
  IN OUT SHA1_CTX  *C,
  OUT    UINT8     Digest[20]
  )
{
  UINT64  Bits;
  UINT8   Pad;
  UINT8   L[8];
  UINTN   I;

  Bits = C->Bytes * 8;
  Pad  = 0x80;
  Sha1Update (C, &Pad, 1);
  Pad = 0;
  while (C->Fill != 56) {
    Sha1Update (C, &Pad, 1);
  }
  for (I = 0; I < 8; I++) {
    L[I] = (UINT8)(Bits >> (56 - 8 * I));
  }
  Sha1Update (C, L, 8);
  for (I = 0; I < 5; I++) {
    Digest[4 * I]     = (UINT8)(C->H[I] >> 24);
    Digest[4 * I + 1] = (UINT8)(C->H[I] >> 16);
    Digest[4 * I + 2] = (UINT8)(C->H[I] >> 8);
    Digest[4 * I + 3] = (UINT8)C->H[I];
  }
}

/**
  HMAC-SHA1 over the concatenation of up to three pieces.
**/
STATIC
VOID
HmacSha1 (
  IN  CONST UINT8  *Key,
  IN  UINTN        KeyLen,
  IN  CONST UINT8  *P1,
  IN  UINTN        L1,
  IN  CONST UINT8  *P2  OPTIONAL,
  IN  UINTN        L2,
  IN  CONST UINT8  *P3  OPTIONAL,
  IN  UINTN        L3,
  OUT UINT8        Mac[20]
  )
{
  SHA1_CTX  C;
  UINT8     K[64];
  UINT8     Pad[64];
  UINT8     Inner[20];
  UINTN     I;

  ZeroMem (K, sizeof (K));
  if (KeyLen > 64) {
    Sha1Init (&C);
    Sha1Update (&C, Key, KeyLen);
    Sha1Final (&C, K);
  } else {
    CopyMem (K, Key, KeyLen);
  }

  for (I = 0; I < 64; I++) {
    Pad[I] = K[I] ^ 0x36;
  }
  Sha1Init (&C);
  Sha1Update (&C, Pad, 64);
  Sha1Update (&C, P1, L1);
  if (P2 != NULL) {
    Sha1Update (&C, P2, L2);
  }
  if (P3 != NULL) {
    Sha1Update (&C, P3, L3);
  }
  Sha1Final (&C, Inner);

  for (I = 0; I < 64; I++) {
    Pad[I] = K[I] ^ 0x5C;
  }
  Sha1Init (&C);
  Sha1Update (&C, Pad, 64);
  Sha1Update (&C, Inner, 20);
  Sha1Final (&C, Mac);
}

/**
  802.11i PSK -> PMK: PBKDF2-HMAC-SHA1 (passphrase, SSID, 4096, 32).
**/
STATIC
VOID
Pbkdf2Pmk (
  IN  CONST CHAR8  *Pass,
  IN  UINTN        PassLen,
  IN  CONST UINT8  *Ssid,
  IN  UINTN        SsidLen,
  OUT UINT8        Pmk[32]
  )
{
  UINT8   Count[4];
  UINT8   U[20];
  UINT8   T[20];
  UINT32  Block;
  UINTN   Iter;
  UINTN   I;

  for (Block = 1; Block <= 2; Block++) {
    Count[0] = 0;
    Count[1] = 0;
    Count[2] = 0;
    Count[3] = (UINT8)Block;
    HmacSha1 ((CONST UINT8 *)Pass, PassLen, Ssid, SsidLen, Count, 4, NULL, 0, U);
    CopyMem (T, U, 20);
    for (Iter = 1; Iter < 4096; Iter++) {
      HmacSha1 ((CONST UINT8 *)Pass, PassLen, U, 20, NULL, 0, NULL, 0, U);
      for (I = 0; I < 20; I++) {
        T[I] ^= U[I];
      }
    }
    CopyMem (Pmk + (Block - 1) * 20, T, (Block == 1) ? 20 : 12);
  }
}

/**
  802.11 PRF-n (SHA1): Out = HMAC (Key, Label || 0 || Data || i), i = 0, 1 ...
**/
STATIC
VOID
PrfSha1 (
  IN  CONST UINT8  *Key,
  IN  UINTN        KeyLen,
  IN  CONST CHAR8  *Label,
  IN  CONST UINT8  *Data,
  IN  UINTN        DataLen,
  OUT UINT8        *Out,
  IN  UINTN        OutLen
  )
{
  UINT8   Buf[128];
  UINT8   Mac[20];
  UINTN   LabelLen;
  UINTN   Pos;
  UINT8   I;

  for (LabelLen = 0; Label[LabelLen] != '\0'; LabelLen++) {
  }
  CopyMem (Buf, Label, LabelLen + 1);       // with the 0 separator
  CopyMem (Buf + LabelLen + 1, Data, DataLen);
  for (Pos = 0, I = 0; Pos < OutLen; Pos += 20, I++) {
    HmacSha1 (Key, KeyLen, Buf, LabelLen + 1 + DataLen, &I, 1, NULL, 0, Mac);
    CopyMem (Out + Pos, Mac, MIN ((UINTN)20, OutLen - Pos));
  }
}

//
// ---------------------------------------------------------------------------
// AES-128 decryption, for the RFC 3394 key unwrap of the GTK.
// ---------------------------------------------------------------------------
//

STATIC UINT8    mSbox[256];
STATIC UINT8    mInvSbox[256];
STATIC BOOLEAN  mSboxReady;

STATIC
UINT8
GfMul (
  IN UINT8  A,
  IN UINT8  B
  )
{
  UINT8  R;

  for (R = 0; B != 0; B >>= 1) {
    if ((B & 1) != 0) {
      R ^= A;
    }
    A = (UINT8)((A << 1) ^ (((A & 0x80) != 0) ? 0x1B : 0));
  }
  return R;
}

STATIC
VOID
AesTables (
  VOID
  )
{
  UINT8  P;
  UINT8  Q;
  UINT8  X;

  if (mSboxReady) {
    return;
  }

  //
  // p walks the field by multiplying with 3, q by dividing by 3 (so q is
  // p's inverse); the S-box is the affine transform of the inverse.
  //
  P = 1;
  Q = 1;
  do {
    P = (UINT8)(P ^ (P << 1) ^ (((P & 0x80) != 0) ? 0x1B : 0));
    Q ^= (UINT8)(Q << 1);
    Q ^= (UINT8)(Q << 2);
    Q ^= (UINT8)(Q << 4);
    if ((Q & 0x80) != 0) {
      Q ^= 0x09;
    }
    X = (UINT8)(Q ^ ((Q << 1) | (Q >> 7)) ^ ((Q << 2) | (Q >> 6)) ^ ((Q << 3) | (Q >> 5)) ^ ((Q << 4) | (Q >> 4)));
    mSbox[P] = X ^ 0x63;
  } while (P != 1);
  mSbox[0] = 0x63;
  for (X = 0; ; X++) {
    mInvSbox[mSbox[X]] = X;
    if (X == 255) {
      break;
    }
  }
  mSboxReady = TRUE;
}

STATIC
VOID
AesExpandKey (
  IN  CONST UINT8  Key[16],
  OUT UINT8        Rk[176]
  )
{
  UINT8  T[4];
  UINT8  Rcon;
  UINTN  I;
  UINT8  Tmp;

  AesTables ();
  CopyMem (Rk, Key, 16);
  Rcon = 1;
  for (I = 16; I < 176; I += 4) {
    CopyMem (T, Rk + I - 4, 4);
    if ((I % 16) == 0) {
      Tmp  = T[0];
      T[0] = (UINT8)(mSbox[T[1]] ^ Rcon);
      T[1] = mSbox[T[2]];
      T[2] = mSbox[T[3]];
      T[3] = mSbox[Tmp];
      Rcon = GfMul (Rcon, 2);
    }
    Rk[I]     = Rk[I - 16] ^ T[0];
    Rk[I + 1] = Rk[I - 15] ^ T[1];
    Rk[I + 2] = Rk[I - 14] ^ T[2];
    Rk[I + 3] = Rk[I - 13] ^ T[3];
  }
}

STATIC
VOID
AesDecryptBlock (
  IN     CONST UINT8  Rk[176],
  IN OUT UINT8        S[16]
  )
{
  UINT8  T[16];
  UINTN  Round;
  UINTN  I;
  UINTN  C;

  for (I = 0; I < 16; I++) {
    S[I] ^= Rk[160 + I];
  }
  for (Round = 9; ; Round--) {
    //
    // InvShiftRows + InvSubBytes (state column major: S[4c + r]).
    //
    for (C = 0; C < 4; C++) {
      for (I = 0; I < 4; I++) {
        T[4 * ((C + I) % 4) + I] = mInvSbox[S[4 * C + I]];
      }
    }
    for (I = 0; I < 16; I++) {
      S[I] = T[I] ^ Rk[16 * Round + I];
    }
    if (Round == 0) {
      break;
    }
    for (C = 0; C < 4; C++) {
      T[0] = S[4 * C];
      T[1] = S[4 * C + 1];
      T[2] = S[4 * C + 2];
      T[3] = S[4 * C + 3];
      S[4 * C]     = GfMul (T[0], 14) ^ GfMul (T[1], 11) ^ GfMul (T[2], 13) ^ GfMul (T[3], 9);
      S[4 * C + 1] = GfMul (T[0], 9) ^ GfMul (T[1], 14) ^ GfMul (T[2], 11) ^ GfMul (T[3], 13);
      S[4 * C + 2] = GfMul (T[0], 13) ^ GfMul (T[1], 9) ^ GfMul (T[2], 14) ^ GfMul (T[3], 11);
      S[4 * C + 3] = GfMul (T[0], 11) ^ GfMul (T[1], 13) ^ GfMul (T[2], 9) ^ GfMul (T[3], 14);
    }
  }
}

/**
  RFC 3394 unwrap: In is Len bytes (8 * (n + 1)), Out gets Len - 8.
**/
STATIC
BOOLEAN
AesUnwrap (
  IN  CONST UINT8  Kek[16],
  IN  CONST UINT8  *In,
  IN  UINTN        Len,
  OUT UINT8        *Out
  )
{
  UINT8   Rk[176];
  UINT8   A[8];
  UINT8   B[16];
  UINTN   N;
  UINTN   I;
  INTN    J;
  UINTN   K;
  UINT64  T;

  if ((Len < 24) || ((Len % 8) != 0)) {
    return FALSE;
  }
  N = Len / 8 - 1;
  AesExpandKey (Kek, Rk);
  CopyMem (A, In, 8);
  CopyMem (Out, In + 8, Len - 8);
  for (J = 5; J >= 0; J--) {
    for (I = N; I >= 1; I--) {
      T = (UINT64)N * (UINT64)J + I;
      CopyMem (B, A, 8);
      for (K = 0; K < 8; K++) {
        B[7 - K] ^= (UINT8)(T >> (8 * K));
      }
      CopyMem (B + 8, Out + 8 * (I - 1), 8);
      AesDecryptBlock (Rk, B);
      CopyMem (A, B, 8);
      CopyMem (Out + 8 * (I - 1), B + 8, 8);
    }
  }
  for (K = 0; K < 8; K++) {
    if (A[K] != 0xA6) {
      return FALSE;
    }
  }
  return TRUE;
}

//
// ---------------------------------------------------------------------------
// Handshake.
// ---------------------------------------------------------------------------
//

STATIC
UINT16
Be16 (
  IN CONST UINT8  *P
  )
{
  return (UINT16)((P[0] << 8) | P[1]);
}

STATIC
VOID
PutBe16 (
  OUT UINT8   *P,
  IN  UINT16  V
  )
{
  P[0] = (UINT8)(V >> 8);
  P[1] = (UINT8)V;
}

STATIC
VOID
Put32 (
  OUT UINT8   *P,
  IN  UINT32  V
  )
{
  P[0] = (UINT8)V;
  P[1] = (UINT8)(V >> 8);
  P[2] = (UINT8)(V >> 16);
  P[3] = (UINT8)(V >> 24);
}

/**
  A nonce: SHA1 over the seed from the chip's RNG, a counter, the timer and
  the authenticator's nonce.
**/
STATIC
VOID
MakeNonce (
  IN OUT BCM_SDIO  *Bcm,
  OUT    UINT8     Nonce[32]
  )
{
  SHA1_CTX  C;
  UINT64    Ticks;
  UINT8     Half;

  for (Half = 0; Half < 2; Half++) {
    Ticks = GetPerformanceCounter ();
    Bcm->WpaNonceCount++;
    Sha1Init (&C);
    Sha1Update (&C, Bcm->WpaSeed, sizeof (Bcm->WpaSeed));
    Sha1Update (&C, (UINT8 *)&Bcm->WpaNonceCount, sizeof (Bcm->WpaNonceCount));
    Sha1Update (&C, (UINT8 *)&Ticks, sizeof (Ticks));
    Sha1Update (&C, &Half, 1);
    Sha1Update (&C, Bcm->WpaANonce, 32);
    Sha1Final (&C, (Half == 0) ? Nonce : Bcm->WpaSeed);
    if (Half == 0) {
      CopyMem (Bcm->WpaSeed + 20, Nonce, 12);    // stir
    } else {
      CopyMem (Nonce + 20, Bcm->WpaSeed, 12);
    }
  }
}

/**
  The RSN IE the firmware put into our (re)association request: the one
  message 2 must carry.
**/
STATIC
BOOLEAN
FetchRsnIe (
  IN OUT BCM_SDIO  *Bcm
  )
{
  UINT8   Info[64];
  UINT8   Ies[512];
  UINT32  ReqLen;
  UINT32  Start;
  UINT32  Pos;

  ZeroMem (Info, sizeof (Info));
  ZeroMem (Ies, sizeof (Ies));
  if (EFI_ERROR (BcmIovarGet (Bcm, "assoc_info", Info, sizeof (Info)))) {
    return FALSE;
  }
  ReqLen = (UINT32)Info[0] | ((UINT32)Info[1] << 8) | ((UINT32)Info[2] << 16) | ((UINT32)Info[3] << 24);
  ReqLen = MIN (ReqLen, (UINT32)sizeof (Ies));
  if ((ReqLen == 0) || EFI_ERROR (BcmIovarGet (Bcm, "assoc_req_ies", Ies, sizeof (Ies)))) {
    return FALSE;
  }

  //
  // IEs only (as brcmfmac hands them to cfg80211); in case the fixed
  // fields come first, also try after capability + listen interval
  // (+ current AP for a reassociation).
  //
  for (Start = 0; Start <= 10; Start += (Start == 0) ? 4 : 6) {
    for (Pos = Start; Pos + 2 <= ReqLen; Pos += 2 + Ies[Pos + 1]) {
      if (Pos + 2 + Ies[Pos + 1] > ReqLen) {
        break;
      }
      if ((Ies[Pos] == 0x30) && (Ies[Pos + 1] >= 18) && (Ies[Pos + 1] + 2 <= sizeof (Bcm->WpaRsnIe))) {
        Bcm->WpaRsnIeLen = Ies[Pos + 1] + 2u;
        CopyMem (Bcm->WpaRsnIe, Ies + Pos, Bcm->WpaRsnIeLen);
        return TRUE;
      }
    }
  }
  return FALSE;
}

/**
  An EAPOL-Key frame to the AP: Key info, replay counter, nonce, key data;
  MIC with the KCK when Mic.
**/
STATIC
EFI_STATUS
SendKeyFrame (
  IN OUT BCM_SDIO     *Bcm,
  IN     UINT8        EapolVersion,
  IN     UINT16       KeyInfo,
  IN     CONST UINT8  *Replay,
  IN     CONST UINT8  *Nonce  OPTIONAL,
  IN     CONST UINT8  *Data   OPTIONAL,
  IN     UINT32       DataLen
  )
{
  UINT8   Frame[4 + ETH_HDR + 4 + EK_DATA + 64];
  UINT8   *Eth;
  UINT8   *X;
  UINT8   *K;
  UINT8   Mic[20];
  UINT32  BodyLen;

  if (DataLen > 64) {
    return EFI_BAD_BUFFER_SIZE;
  }
  ZeroMem (Frame, sizeof (Frame));
  Frame[0] = 2 << 4;                    // BDC version 2, priority 0, no offset
  Eth = Frame + 4;
  CopyMem (Eth, Bcm->WpaBssid, 6);
  CopyMem (Eth + 6, Bcm->WpaMac, 6);
  Eth[12] = 0x88;
  Eth[13] = 0x8E;
  X       = Eth + ETH_HDR;
  BodyLen = EK_DATA + DataLen;
  X[0]    = EapolVersion;
  X[1]    = 3;                          // EAPOL-Key
  PutBe16 (X + 2, (UINT16)BodyLen);
  K       = X + 4;
  K[0]    = 2;                          // RSN key descriptor
  PutBe16 (K + EK_INFO, KeyInfo);
  CopyMem (K + EK_REPLAY, Replay, 8);
  if (Nonce != NULL) {
    CopyMem (K + EK_NONCE, Nonce, 32);
  }
  PutBe16 (K + EK_DATALEN, (UINT16)DataLen);
  if (DataLen > 0) {
    CopyMem (K + EK_DATA, Data, DataLen);
  }
  HmacSha1 (Bcm->WpaPtk, 16, X, 4 + BodyLen, NULL, 0, NULL, 0, Mic);
  CopyMem (K + EK_MIC, Mic, 16);
  return BcmSdioSend (Bcm, BCM_CHANNEL_DATA, Frame, 4 + ETH_HDR + 4 + BodyLen);
}

STATIC
EFI_STATUS
SetKey (
  IN OUT BCM_SDIO     *Bcm,
  IN     UINT32       Index,
  IN     CONST UINT8  *Key,
  IN     UINT32       Len,
  IN     UINT32       Algo,
  IN     CONST UINT8  *Rsc       OPTIONAL,
  IN     CONST UINT8  *Station   OPTIONAL
  )
{
  UINT8  K[164];                        // wl_wsec_key_t
  UINT8  Tmp[8];

  ZeroMem (K, sizeof (K));
  Put32 (K + 0, Index);
  Put32 (K + 4, Len);
  CopyMem (K + 8, Key, Len);
  if ((Algo == CRYPTO_ALGO_TKIP) && (Len == 32)) {
    //
    // Station side: the authenticator's Tx MIC key is our Rx MIC key.
    //
    CopyMem (Tmp, K + 8 + 24, 8);
    CopyMem (K + 8 + 24, K + 8 + 16, 8);
    CopyMem (K + 8 + 16, Tmp, 8);
  }
  Put32 (K + 112, Algo);
  if (Rsc != NULL) {
    Put32 (K + 132, 1);                 // iv_initialized
    Put32 (K + 140, (UINT32)Rsc[2] | ((UINT32)Rsc[3] << 8) | ((UINT32)Rsc[4] << 16) | ((UINT32)Rsc[5] << 24));
    K[144] = Rsc[0];
    K[145] = Rsc[1];
  }
  if (Station != NULL) {
    CopyMem (K + 156, Station, 6);
  }
  return BcmIoctl (Bcm, WLC_SET_KEY, TRUE, K, sizeof (K), 1000);
}

EFI_STATUS
BcmSetKey (
  IN OUT BCM_SDIO     *Bcm,
  IN     UINT32       Index,
  IN     CONST UINT8  *Key      OPTIONAL,
  IN     UINT32       Length,
  IN     UINT32       Algorithm,
  IN     CONST UINT8  *Rsc      OPTIONAL,
  IN     CONST UINT8  *Station  OPTIONAL
  )
{
  STATIC CONST UINT8  NoKey[1] = { 0 };

  if ((Length > 32) || ((Length > 0) && (Key == NULL))) {
    return EFI_INVALID_PARAMETER;
  }
  return SetKey (Bcm, Index, (Key != NULL) ? Key : NoKey, Length, (Length != 0) ? Algorithm : BCM_CRYPTO_ALGO_OFF, Rsc,
                 Station);
}

STATIC
UINT32
CipherAlgo (
  IN CONST UINT8  *Suite
  )
{
  if (CompareMem (Suite, mRsnOui, 3) != 0) {
    return 0;
  }
  return (Suite[3] == 4) ? CRYPTO_ALGO_AES_CCM : (Suite[3] == 2) ? CRYPTO_ALGO_TKIP : 0;
}

/**
  The GTK KDE of decrypted key data: install it.
**/
STATIC
EFI_STATUS
InstallGtk (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST UINT8  *Data,
  IN     UINT32       Len,
  IN     CONST UINT8  *Rsc
  )
{
  UINT32  Pos;
  UINT32  Algo;
  UINT32  GtkLen;

  for (Pos = 0; Pos + 2 <= Len; Pos += 2 + Data[Pos + 1]) {
    if ((Data[Pos] == 0xDD) && (Data[Pos + 1] == 0)) {
      break;                            // padding
    }
    if (Pos + 2 + Data[Pos + 1] > Len) {
      break;
    }
    if ((Data[Pos] == 0xDD) && (Data[Pos + 1] >= 6 + 16) && (CompareMem (Data + Pos + 2, mRsnOui, 3) == 0) &&
        (Data[Pos + 5] == 1)) {
      GtkLen = Data[Pos + 1] - 6u;
      Algo   = (GtkLen == 32) ? CRYPTO_ALGO_TKIP : CRYPTO_ALGO_AES_CCM;
      if ((Bcm->WpaRsnIeLen >= 8) && (CipherAlgo (Bcm->WpaRsnIe + 4) != 0)) {
        Algo = CipherAlgo (Bcm->WpaRsnIe + 4);
      }
      DEBUG ((DEBUG_INFO, "Bcm: WPA group key %u (%a, %u bytes)\n", Data[Pos + 6] & 3,
              (Algo == CRYPTO_ALGO_TKIP) ? "TKIP" : "CCMP", GtkLen));
      return SetKey (Bcm, Data[Pos + 6] & 3, Data + Pos + 8, GtkLen, Algo, Rsc, NULL);
    }
  }
  DEBUG ((DEBUG_INFO, "Bcm: WPA no group key in the key data\n"));
  return EFI_NOT_FOUND;
}

/**
  Check the MIC of a received EAPOL-Key frame X (802.1X header first,
  Len bytes) with the current KCK.
**/
STATIC
BOOLEAN
MicOk (
  IN BCM_SDIO  *Bcm,
  IN UINT8     *X,
  IN UINT32    Len
  )
{
  UINT8  Got[16];
  UINT8  Mic[20];

  CopyMem (Got, X + 4 + EK_MIC, 16);
  ZeroMem (X + 4 + EK_MIC, 16);
  HmacSha1 (Bcm->WpaPtk, 16, X, Len, NULL, 0, NULL, 0, Mic);
  CopyMem (X + 4 + EK_MIC, Got, 16);
  return CompareMem (Got, Mic, 16) == 0;
}

/**
  Decrypt the key data of X in place; returns its plain length (0: failed).
**/
STATIC
UINT32
KeyData (
  IN     BCM_SDIO  *Bcm,
  IN OUT UINT8     *K,
  IN     UINT32    DataLen,
  OUT    UINT8     *Plain
  )
{
  if ((Be16 (K + EK_INFO) & KI_ENCRYPTED) == 0) {
    CopyMem (Plain, K + EK_DATA, DataLen);
    return DataLen;
  }
  if (!AesUnwrap (Bcm->WpaPtk + 16, K + EK_DATA, DataLen, Plain)) {
    DEBUG ((DEBUG_INFO, "Bcm: WPA key data unwrap failed\n"));
    return 0;
  }
  return DataLen - 8;
}

STATIC
VOID
HandleKeyFrame (
  IN OUT BCM_SDIO  *Bcm,
  IN     UINT8     *Frame,
  IN     UINT32    Length
  )
{
  UINT8       *X;
  UINT8       *K;
  UINT32      XLen;
  UINT16      Info;
  UINT32      DataLen;
  UINT32      PlainLen;
  UINT8       Plain[256];
  UINT8       B[76];
  UINT32      Algo;
  EFI_STATUS  Status;

  X = Frame + ETH_HDR;
  if ((Length < ETH_HDR + 4 + EK_DATA) || (X[1] != 3)) {
    return;
  }
  XLen = 4u + Be16 (X + 2);
  K    = X + 4;
  if ((XLen > Length - ETH_HDR) || (XLen < 4 + EK_DATA) || (K[0] != 2)) {
    return;
  }
  Info    = Be16 (K + EK_INFO);
  DataLen = Be16 (K + EK_DATALEN);
  if ((EK_DATA + DataLen > XLen - 4) || (DataLen > sizeof (Plain))) {
    return;
  }
  if ((Info & KI_VERSION_MASK) != 2) {
    DEBUG ((DEBUG_INFO, "Bcm: WPA key descriptor version %u not supported (TKIP pairwise?)\n", Info & KI_VERSION_MASK));
    return;
  }

  if ((Info & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_PAIRWISE | KI_ACK)) {
    //
    // Message 1: ANonce -> PTK, answer with SNonce + our RSN IE.
    //
    CopyMem (Bcm->WpaBssid, Frame + 6, 6);
    if ((Bcm->WpaRsnIeLen == 0) && !FetchRsnIe (Bcm)) {
      DEBUG ((DEBUG_INFO, "Bcm: WPA no RSN IE in the association request\n"));
      return;
    }
    CopyMem (Bcm->WpaANonce, K + EK_NONCE, 32);
    MakeNonce (Bcm, Bcm->WpaSNonce);
    if (CompareMem (Bcm->WpaMac, Bcm->WpaBssid, 6) < 0) {
      CopyMem (B, Bcm->WpaMac, 6);
      CopyMem (B + 6, Bcm->WpaBssid, 6);
    } else {
      CopyMem (B, Bcm->WpaBssid, 6);
      CopyMem (B + 6, Bcm->WpaMac, 6);
    }
    if (CompareMem (Bcm->WpaANonce, Bcm->WpaSNonce, 32) < 0) {
      CopyMem (B + 12, Bcm->WpaANonce, 32);
      CopyMem (B + 44, Bcm->WpaSNonce, 32);
    } else {
      CopyMem (B + 12, Bcm->WpaSNonce, 32);
      CopyMem (B + 44, Bcm->WpaANonce, 32);
    }
    PrfSha1 (Bcm->WpaPmk, 32, "Pairwise key expansion", B, sizeof (B), Bcm->WpaPtk, 48);
    Status = SendKeyFrame (Bcm, X[0], 2 | KI_PAIRWISE | KI_MIC, K + EK_REPLAY, Bcm->WpaSNonce,
                           Bcm->WpaRsnIe, Bcm->WpaRsnIeLen);
    DEBUG ((DEBUG_INFO, "Bcm: WPA 1/4 from %02x:%02x:%02x:%02x:%02x:%02x, sent 2/4 - %r\n", Bcm->WpaBssid[0],
            Bcm->WpaBssid[1], Bcm->WpaBssid[2], Bcm->WpaBssid[3], Bcm->WpaBssid[4], Bcm->WpaBssid[5], Status));
    return;
  }

  if (!MicOk (Bcm, X, XLen)) {
    DEBUG ((DEBUG_INFO, "Bcm: WPA MIC mismatch (key info 0x%04x) - wrong passphrase?\n", Info));
    return;
  }
  PlainLen = KeyData (Bcm, K, DataLen, Plain);

  if ((Info & (KI_PAIRWISE | KI_ACK | KI_MIC | KI_INSTALL)) == (KI_PAIRWISE | KI_ACK | KI_MIC | KI_INSTALL)) {
    //
    // Message 3: answer, then (once 4/4 is on its way out unencrypted) the
    // pairwise and group keys.
    //
    if (CompareMem (K + EK_NONCE, Bcm->WpaANonce, 32) != 0) {
      DEBUG ((DEBUG_INFO, "Bcm: WPA 3/4 with another ANonce, ignored\n"));
      return;
    }
    Status = SendKeyFrame (Bcm, X[0], 2 | KI_PAIRWISE | KI_MIC | KI_SECURE, K + EK_REPLAY, NULL, NULL, 0);
    DEBUG ((DEBUG_INFO, "Bcm: WPA 3/4, sent 4/4 - %r\n", Status));
    gBS->Stall (50 * 1000);
    Algo   = CipherAlgo (Bcm->WpaRsnIe + 10);
    Status = (Algo == CRYPTO_ALGO_AES_CCM) ? SetKey (Bcm, 0, Bcm->WpaPtk + 32, 16, Algo, NULL, Bcm->WpaBssid)
                                           : EFI_UNSUPPORTED;
    DEBUG ((DEBUG_INFO, "Bcm: WPA pairwise key - %r\n", Status));
    if (!EFI_ERROR (Status) && (PlainLen > 0)) {
      Status = InstallGtk (Bcm, Plain, PlainLen, K + EK_RSC);
    }
    Bcm->WpaDone = !EFI_ERROR (Status);
    DEBUG ((DEBUG_INFO, "Bcm: WPA handshake %a\n", Bcm->WpaDone ? "complete" : "FAILED"));
    return;
  }

  if ((Info & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_ACK | KI_MIC)) {
    //
    // Group key handshake 1/2 (rekey).
    //
    Status = (PlainLen > 0) ? InstallGtk (Bcm, Plain, PlainLen, K + EK_RSC) : EFI_DEVICE_ERROR;
    SendKeyFrame (Bcm, X[0], 2 | KI_MIC | KI_SECURE, K + EK_REPLAY, NULL, NULL, 0);
    DEBUG ((DEBUG_INFO, "Bcm: WPA group rekey - %r\n", Status));
  }
}

//
// ---------------------------------------------------------------------------
// Interface.
// ---------------------------------------------------------------------------
//

EFI_STATUS
BcmWpaSetup (
  IN OUT BCM_SDIO     *Bcm,
  IN     CONST CHAR8  *Ssid,
  IN     UINT32       SsidLen,
  IN     CONST CHAR8  *Passphrase
  )
{
  UINT32      Pass;
  EFI_STATUS  Status;

  for (Pass = 0; Passphrase[Pass] != '\0'; Pass++) {
  }
  if ((Pass < 8) || (Pass > 63)) {
    DEBUG ((DEBUG_INFO, "Bcm: WPA passphrase must be 8..63 characters (got %u)\n", Pass));
    return EFI_INVALID_PARAMETER;
  }
  Status = BcmIovarGet (Bcm, "cur_etheraddr", Bcm->WpaMac, 6);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Seed: no "rand" iovar in this firmware (BCME_UNSUPPORTED); the timer
  // here plus the arrival time of every frame (Dispatch()) are folded in.
  //
  *(UINT64 *)(VOID *)Bcm->WpaSeed ^= GetPerformanceCounter ();

  Pbkdf2Pmk (Passphrase, Pass, (CONST UINT8 *)Ssid, SsidLen, Bcm->WpaPmk);
  Bcm->WpaRsnIeLen = 0;
  Bcm->WpaDone     = FALSE;
  Bcm->EapolLength = 0;
  Bcm->WpaActive   = TRUE;
  return EFI_SUCCESS;
}

VOID
BcmWpaLinkDown (
  IN OUT BCM_SDIO  *Bcm
  )
{
  Bcm->WpaDone     = FALSE;
  Bcm->WpaRsnIeLen = 0;
}

BOOLEAN
BcmWpaService (
  IN OUT BCM_SDIO  *Bcm
  )
{
  UINT8    Frame[BCM_EAPOL_MAX];
  UINT32   Length;
  BOOLEAN  Was;

  Was = Bcm->WpaDone;
  while (Bcm->WpaActive && (Bcm->EapolLength != 0)) {
    //
    // A copy: answering polls the bus, which may queue the next frame.
    //
    Length = Bcm->EapolLength;
    CopyMem (Frame, Bcm->Eapol, Length);
    Bcm->EapolLength = 0;
    HandleKeyFrame (Bcm, Frame, Length);
  }
  return !Was && Bcm->WpaDone;
}
