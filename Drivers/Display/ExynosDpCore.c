/** @file
  Exynos5420 DP1 transmitter: video PHY, AUX channel (DPCD), software link
  training and slave-mode video (timing and pixel clock from FIMD1).

  Ported from the v1awifi kernel's Samsung DP driver (drivers/video/
  s5p-dp-core.c, s5p-dp-reg.c, s5p-dp-reg.h; the non-PSR path of
  s5p_dp_enable()), board data from board-v1-display.c (4 lanes,
  2.7 Gbps, RGB 8 bpc VESA, no analog_param).  Every wait has a bound.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "ExynosDisplay.h"

#define DP_TX_SW_RESET          0x014
#define DP_FUNC_EN_1            0x018
#define DP_FUNC_EN_2            0x01C
#define DP_VIDEO_CTL_1          0x020
#define DP_VIDEO_CTL_2          0x024
#define DP_VIDEO_CTL_3          0x028
#define DP_VIDEO_CTL_8          0x03C
#define DP_VIDEO_CTL_10         0x044
#define DP_LANE_MAP             0x35C
#define DP_ANALOG_CTL_1         0x370
#define DP_ANALOG_CTL_2         0x374
#define DP_ANALOG_CTL_3         0x378
#define DP_PLL_FILTER_CTL_1     0x37C
#define DP_TX_AMP_TUNING_CTL    0x380
#define DP_AUX_HW_RETRY_CTL     0x390
#define DP_COMMON_INT_STA_1     0x3C4
#define DP_COMMON_INT_STA_2     0x3C8
#define DP_COMMON_INT_STA_3     0x3CC
#define DP_COMMON_INT_STA_4     0x3D0
#define DP_INT_STA              0x3DC
#define DP_COMMON_INT_MASK_1    0x3E0
#define DP_COMMON_INT_MASK_2    0x3E4
#define DP_COMMON_INT_MASK_3    0x3E8
#define DP_COMMON_INT_MASK_4    0x3EC
#define DP_INT_STA_MASK         0x3F8
#define DP_INT_CTL              0x3FC
#define DP_SYS_CTL_1            0x600
#define DP_SYS_CTL_2            0x604
#define DP_SYS_CTL_3            0x608
#define DP_SYS_CTL_4            0x60C
#define DP_PKT_SEND_CTL         0x640
#define DP_HDCP_CTL             0x648
#define DP_LINK_BW_SET          0x680
#define DP_LANE_COUNT_SET       0x684
#define DP_TRAINING_PTN_SET     0x688
#define DP_LN_LINK_TRAINING_CTL(n)  (0x68C + (n) * 4)
#define DP_DEBUG_CTL            0x6C0
#define DP_HPD_DEGLITCH_L       0x6C4
#define DP_HPD_DEGLITCH_H       0x6C8
#define DP_LINK_DEBUG_CTL       0x6E0
#define DP_N_VID_0              0x70C
#define DP_N_VID_1              0x710
#define DP_N_VID_2              0x714
#define DP_PLL_CTL              0x71C
#define DP_PHY_PD               0x720
#define DP_PHY_TEST             0x724
#define DP_VIDEO_FIFO_THRD      0x730
#define DP_AUDIO_MARGIN         0x73C
#define DP_M_VID_GEN_FILTER_TH  0x764
#define DP_M_AUD_GEN_FILTER_TH  0x778
#define DP_AUX_CH_STA           0x780
#define DP_AUX_CH_DEFER_CTL     0x788
#define DP_BUFFER_DATA_CTL      0x790
#define DP_AUX_CH_CTL_1         0x794
#define DP_AUX_ADDR_7_0         0x798
#define DP_AUX_ADDR_15_8        0x79C
#define DP_AUX_ADDR_19_16       0x7A0
#define DP_AUX_CH_CTL_2         0x7A4
#define DP_BUF_DATA_0           0x7C0
#define DP_SOC_GENERAL_CTL      0x800

// FUNC_EN_1 / 2
#define MASTER_VID_FUNC_EN_N    (1u << 7)
#define SLAVE_VID_FUNC_EN_N     (1u << 5)
#define AUD_FIFO_FUNC_EN_N      (1u << 4)
#define AUD_FUNC_EN_N           (1u << 3)
#define HDCP_FUNC_EN_N          (1u << 2)
#define SW_FUNC_EN_N            (1u << 0)
#define SSC_FUNC_EN_N           (1u << 7)
#define AUX_FUNC_EN_N           (1u << 2)
#define SERDES_FIFO_FUNC_EN_N   (1u << 1)
#define LS_CLK_DOMAIN_FUNC_EN_N (1u << 0)
// VIDEO_CTL
#define VIDEO_EN                (1u << 7)
#define HDCP_VIDEO_MUTE         (1u << 6)
#define IN_BPC_8_BITS           (1u << 4)
#define VID_HRES_TH(x)          (((x) & 0xF) << 4)
#define VID_VRES_TH(x)          ((x) & 0xF)
#define FORMAT_SEL              (1u << 4)
#define INTERACE_SCAN_CFG       (1u << 2)
#define VSYNC_POLARITY_CFG      (1u << 1)
#define HSYNC_POLARITY_CFG      (1u << 0)
// analog
#define TX_TERMINAL_CTRL_50_OHM (1u << 4)
#define SEL_24M                 (1u << 3)
#define TX_DVDD_BIT_1_0625V     (4u << 0)
#define DRIVE_DVDD_BIT_1_0625V  (4u << 5)
#define VCO_BIT_600_MICRO       (5u << 0)
#define PD_RING_OSC             (1u << 6)
#define AUX_TERMINAL_CTRL_50_OHM (2u << 4)
#define TX_CUR1_2X              (1u << 2)
#define TX_CUR_16_MA            (3u << 0)
#define CH_AMP_500_MV           0x08080808u     // CONFIG_S5P_DP_PSR is set on v1awifi
// AUX
#define AUX_BIT_PERIOD_EXPECTED_DELAY(x) (((x) & 7) << 8)
#define AUX_HW_RETRY_INTERVAL_600_US     (0u << 3)
#define AUX_HW_RETRY_COUNT_SEL(x)        ((x) & 7)
#define DEFER_CTRL_EN           (1u << 7)
#define DEFER_COUNT(x)          ((x) & 0x7F)
#define BUF_CLR                 (1u << 7)
#define AUX_LENGTH(x)           ((((x) - 1) & 0xF) << 4)
#define AUX_TX_COMM_DP          (1u << 3)
#define AUX_TX_COMM_READ        (1u << 0)
#define AUX_EN                  (1u << 0)
#define AUX_STATUS_MASK         0xF
// interrupt status
#define PLL_LOCK_CHG            (1u << 6)
#define VSYNC_DET               (1u << 7)
#define VID_FORMAT_CHG          (1u << 3)
#define VID_CLK_CHG             (1u << 1)
#define HOTPLUG_CHG             (1u << 2)
#define HPD_LOST                (1u << 1)
#define PLUG                    (1u << 0)
#define INT_HPD                 (1u << 6)
#define RPLY_RECEIV             (1u << 1)
#define AUX_ERR                 (1u << 0)
#define INT_POL                 (1u << 0)
// SYS_CTL
#define DET_STA                 (1u << 2)
#define CHA_CRI(x)              (((x) & 0xF) << 4)
#define CHA_STA                 (1u << 2)
#define CHA_CTRL                (1u << 0)
#define HPD_STATUS              (1u << 6)
#define F_HPD                   (1u << 5)
#define HPD_CTRL                (1u << 4)
#define STRM_VALID              (1u << 2)
#define ENHANCED                (1u << 3)
#define FIX_M_VID               (1u << 2)
// training
#define SCRAMBLING_DISABLE      (1u << 5)
#define SW_TRAINING_PATTERN_PTN1 1u
#define SW_TRAINING_PATTERN_PTN2 2u
#define PRE_EMPHASIS_SET_SHIFT  3
#define PLL_LOCK                (1u << 4)
#define F_PLL_LOCK              (1u << 3)
#define PLL_LOCK_CTRL           (1u << 2)
#define DP_PLL_PD               (1u << 7)
#define DP_PHY_PD_ALL           0x3F           // DP_PHY_PD | AUX_PD | CH3..CH0_PD
#define MACRO_RST               (1u << 5)
// SOC_GENERAL_CTL
#define AUDIO_MODE_SPDIF_MODE   (1u << 8)
#define VIDEO_MASTER_MODE_EN    (1u << 1)
#define VIDEO_MODE_MASK         (1u << 0)
#define VIDEO_MODE_SLAVE_MODE   (1u << 0)

// DPCD
#define DPCD_REV                0x0000
#define DPCD_MAX_LINK_RATE      0x0001
#define DPCD_MAX_LANE_COUNT     0x0002
#define DPCD_LINK_BW_SET        0x0100
#define DPCD_LANE_COUNT_SET     0x0101
#define DPCD_TRAINING_PATTERN_SET 0x0102
#define DPCD_TRAINING_LANE0_SET 0x0103
#define DPCD_CONFIGURATION_SET  0x010A
#define DPCD_LANE0_1_STATUS     0x0202
#define DPCD_LANE_ALIGN_STATUS  0x0204
#define DPCD_ADJUST_REQUEST_LANE0_1 0x0206
#define DPCD_SINK_POWER_STATE   0x0600
#define DPCD_SCRAMBLING_DISABLED (1u << 5)
#define DPCD_TRAINING_PATTERN_1 1u
#define DPCD_TRAINING_PATTERN_2 2u
#define DPCD_MAX_PRE_EMPHASIS_REACHED (1u << 5)
#define DPCD_MAX_SWING_REACHED  (1u << 2)
#define DPCD_LANE_CR_DONE       (1u << 0)
#define DPCD_CHANNEL_EQ_BITS    0x07
#define DPCD_INTERLANE_ALIGN_DONE (1u << 0)
#define DPCD_POWER_D0           1u

#define DP_PMU_DPTX_PHY_CONTROL 0x728
#define DP_PHY_ENABLE           (1u << 0)

#define LINK_RATE_2_70GBPS      0x0A
#define LINK_RATE_1_62GBPS      0x06
#define LANES_MAX               4
#define MAX_CR_LOOP             5
#define MAX_EQ_LOOP             4
#define TIMEOUT_LOOPS           300

#define RD(o)        MmioRead32 (Disp->Dp + (o))
#define WR(o, v)     MmioWrite32 (Disp->Dp + (o), (v))
#define SETB(o, b)   WR (o, RD (o) | (b))
#define CLRB(o, b)   WR (o, RD (o) & ~(b))

VOID
DpPhyPower (
  IN EXYNOS_DISPLAY  *Disp,
  IN BOOLEAN         On
  )
{
  //
  // PMU DPTX_PHY_CONTROL (single, known register; s5p_dp_phy_init/exit).
  //
  if (On) {
    MmioOr32 (Disp->Pmu + DP_PMU_DPTX_PHY_CONTROL, DP_PHY_ENABLE);
  } else {
    MmioAnd32 (Disp->Pmu + DP_PMU_DPTX_PHY_CONTROL, ~DP_PHY_ENABLE);
  }
}

//
// s5p_dp_reset() with init_analog_param() and init_interrupt().
//
STATIC
VOID
DpReset (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  WR (DP_TX_SW_RESET, 1);
  CLRB (DP_VIDEO_CTL_1, VIDEO_EN);
  CLRB (DP_VIDEO_CTL_1, HDCP_VIDEO_MUTE);
  WR (DP_FUNC_EN_1, MASTER_VID_FUNC_EN_N | SLAVE_VID_FUNC_EN_N | AUD_FIFO_FUNC_EN_N | AUD_FUNC_EN_N |
      HDCP_FUNC_EN_N | SW_FUNC_EN_N);
  WR (DP_FUNC_EN_2, SSC_FUNC_EN_N | AUX_FUNC_EN_N | SERDES_FIFO_FUNC_EN_N | LS_CLK_DOMAIN_FUNC_EN_N);
  MicroSecondDelay (20);
  WR (DP_LANE_MAP, (3u << 6) | (2u << 4) | (1u << 2) | 0u);          // no lane swap
  WR (DP_SYS_CTL_1, 0);
  WR (DP_SYS_CTL_2, 0x40);
  WR (DP_SYS_CTL_3, 0);
  WR (DP_SYS_CTL_4, 0);
  WR (DP_PKT_SEND_CTL, 0);
  WR (DP_HDCP_CTL, 0);
  WR (DP_HPD_DEGLITCH_L, 0x5E);
  WR (DP_HPD_DEGLITCH_H, 0x1A);
  WR (DP_LINK_DEBUG_CTL, 0x10);
  WR (DP_PHY_TEST, 0);
  WR (DP_VIDEO_FIFO_THRD, 0);
  WR (DP_AUDIO_MARGIN, 0x20);
  WR (DP_M_VID_GEN_FILTER_TH, 0x4);
  WR (DP_M_AUD_GEN_FILTER_TH, 0x2);
  WR (DP_SOC_GENERAL_CTL, 0x00000101);

  WR (DP_ANALOG_CTL_1, TX_TERMINAL_CTRL_50_OHM);
  WR (DP_ANALOG_CTL_2, SEL_24M | TX_DVDD_BIT_1_0625V);
  WR (DP_ANALOG_CTL_3, DRIVE_DVDD_BIT_1_0625V | VCO_BIT_600_MICRO);
  WR (DP_PLL_FILTER_CTL_1, PD_RING_OSC | AUX_TERMINAL_CTRL_50_OHM | TX_CUR1_2X | TX_CUR_16_MA);
  WR (DP_TX_AMP_TUNING_CTL, CH_AMP_500_MV);

  WR (DP_INT_CTL, INT_POL);
  WR (DP_COMMON_INT_STA_1, 0xFF);
  WR (DP_COMMON_INT_STA_2, 0x4F);
  WR (DP_COMMON_INT_STA_3, 0xE0);
  WR (DP_COMMON_INT_STA_4, 0x27);
  WR (DP_INT_STA, 0x63);
  WR (DP_COMMON_INT_MASK_1, 0);
  WR (DP_COMMON_INT_MASK_2, 0);
  WR (DP_COMMON_INT_MASK_3, 0);
  WR (DP_COMMON_INT_MASK_4, 0);
  WR (DP_INT_STA_MASK, 0);
}

//
// s5p_dp_init_dp(): reset, SW function, analog (PLL), HPD, AUX.  The
// interrupt masks stay closed (polled).
//
STATIC
INTN
DpInit (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  UINTN  Wait;

  DpReset (Disp);
  CLRB (DP_FUNC_EN_1, SW_FUNC_EN_N);

  // init_analog_func: everything powered, PLL up and locked.
  WR (DP_PHY_PD, 0);
  WR (DP_COMMON_INT_STA_1, PLL_LOCK_CHG);
  CLRB (DP_DEBUG_CTL, F_PLL_LOCK | PLL_LOCK_CTRL);
  if ((RD (DP_DEBUG_CTL) & PLL_LOCK) == 0) {
    CLRB (DP_PLL_CTL, DP_PLL_PD);
  }
  for (Wait = 0; ((RD (DP_DEBUG_CTL) & PLL_LOCK) == 0) && (Wait < 100); Wait++) {
    MicroSecondDelay (100);
  }
  if ((RD (DP_DEBUG_CTL) & PLL_LOCK) == 0) {
    DEBUG ((DEBUG_ERROR, "Display: DP PLL does not lock (DEBUG_CTL 0x%x)\n", RD (DP_DEBUG_CTL)));
    return -1;
  }
  CLRB (DP_FUNC_EN_2, SERDES_FIFO_FUNC_EN_N | LS_CLK_DOMAIN_FUNC_EN_N | AUX_FUNC_EN_N);

  // init_hpd
  WR (DP_COMMON_INT_STA_4, HOTPLUG_CHG | HPD_LOST | PLUG);
  WR (DP_INT_STA, INT_HPD);
  CLRB (DP_SYS_CTL_3, F_HPD | HPD_CTRL);

  // init_aux: no hardware retry, defer count 1
  WR (DP_INT_STA, RPLY_RECEIV | AUX_ERR);
  SETB (DP_FUNC_EN_2, AUX_FUNC_EN_N);
  WR (DP_AUX_HW_RETRY_CTL, AUX_BIT_PERIOD_EXPECTED_DELAY (3) | AUX_HW_RETRY_COUNT_SEL (0) |
      AUX_HW_RETRY_INTERVAL_600_US);
  WR (DP_AUX_CH_DEFER_CTL, DEFER_CTRL_EN | DEFER_COUNT (1));
  CLRB (DP_FUNC_EN_2, AUX_FUNC_EN_N);
  return 0;
}

STATIC
INTN
AuxTransaction (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  UINTN   Wait;
  UINT32  Sta;

  SETB (DP_AUX_CH_CTL_2, AUX_EN);
  for (Wait = 0; (RD (DP_AUX_CH_CTL_2) & AUX_EN) != 0; Wait++) {
    if (Wait > TIMEOUT_LOOPS * 10) {
      return -2;
    }
    MicroSecondDelay (100);
  }
  for (Wait = 0; (RD (DP_INT_STA) & RPLY_RECEIV) == 0; Wait++) {
    if (Wait > TIMEOUT_LOOPS) {
      return -3;
    }
    MicroSecondDelay (10);
  }
  WR (DP_INT_STA, RPLY_RECEIV);
  if ((RD (DP_INT_STA) & AUX_ERR) != 0) {
    WR (DP_INT_STA, AUX_ERR);
    return -4;
  }
  Sta = RD (DP_AUX_CH_STA);
  return ((Sta & AUX_STATUS_MASK) != 0) ? -5 : 0;
}

STATIC
VOID
AuxAddress (
  IN EXYNOS_DISPLAY  *Disp,
  IN UINT32          Addr
  )
{
  WR (DP_AUX_ADDR_7_0, Addr & 0xFF);
  WR (DP_AUX_ADDR_15_8, (Addr >> 8) & 0xFF);
  WR (DP_AUX_ADDR_19_16, (Addr >> 16) & 0x0F);
}

INTN
DpcdRead (
  IN  EXYNOS_DISPLAY  *Disp,
  IN  UINT32          Addr,
  OUT UINT8           *Data,
  IN  UINT32          Count
  )
{
  UINT32  Done;
  UINT32  Chunk;
  UINT32  Index;
  UINTN   Try;
  INTN    Result = 0;

  WR (DP_BUFFER_DATA_CTL, BUF_CLR);
  for (Done = 0; Done < Count; Done += Chunk) {
    Chunk = MIN (Count - Done, 16u);
    for (Try = 0; Try < 10; Try++) {
      AuxAddress (Disp, Addr + Done);
      WR (DP_AUX_CH_CTL_1, AUX_LENGTH (Chunk) | AUX_TX_COMM_DP | AUX_TX_COMM_READ);
      Result = AuxTransaction (Disp);
      if (Result == 0) {
        break;
      }
    }
    if (Result != 0) {
      return Result;
    }
    for (Index = 0; Index < Chunk; Index++) {
      Data[Done + Index] = (UINT8)RD (DP_BUF_DATA_0 + 4 * Index);
    }
  }
  return 0;
}

INTN
DpcdWrite (
  IN EXYNOS_DISPLAY  *Disp,
  IN UINT32          Addr,
  IN CONST UINT8     *Data,
  IN UINT32          Count
  )
{
  UINT32  Done;
  UINT32  Chunk;
  UINT32  Index;
  UINTN   Try;
  INTN    Result = 0;

  WR (DP_BUFFER_DATA_CTL, BUF_CLR);
  for (Done = 0; Done < Count; Done += Chunk) {
    Chunk = MIN (Count - Done, 16u);
    for (Try = 0; Try < 10; Try++) {
      AuxAddress (Disp, Addr + Done);
      for (Index = 0; Index < Chunk; Index++) {
        WR (DP_BUF_DATA_0 + 4 * Index, Data[Done + Index]);
      }
      WR (DP_AUX_CH_CTL_1, AUX_LENGTH (Chunk) | AUX_TX_COMM_DP);
      Result = AuxTransaction (Disp);
      if (Result == 0) {
        break;
      }
    }
    if (Result != 0) {
      return Result;
    }
  }
  return 0;
}

STATIC
INTN
DpcdWriteByte (
  IN EXYNOS_DISPLAY  *Disp,
  IN UINT32          Addr,
  IN UINT8           Value
  )
{
  return DpcdWrite (Disp, Addr, &Value, 1);
}

STATIC
VOID
SetTrainingPattern (
  IN EXYNOS_DISPLAY  *Disp,
  IN UINT32          Pattern       // 0 normal (scrambled), 1, 2
  )
{
  WR (DP_TRAINING_PTN_SET, (Pattern == 0) ? 0 : (SCRAMBLING_DISABLE | Pattern));
}

STATIC
UINT8
LaneStatus (
  IN UINT8  *Status,
  IN UINTN  Lane
  )
{
  return (Status[Lane >> 1] >> ((Lane & 1) * 4)) & 0xF;
}

STATIC
UINT8
TrainingValue (
  IN UINT8  *Adjust,
  IN UINTN  Lane
  )
{
  UINT8  Value  = (Adjust[Lane >> 1] >> ((Lane & 1) * 4)) & 0xF;
  UINT8  Swing  = Value & 3;
  UINT8  Emph   = (Value >> 2) & 3;
  UINT8  Result = (UINT8)(Swing | (Emph << 3));

  if (Swing == 3) {
    Result |= DPCD_MAX_SWING_REACHED;
  }
  if (Emph == 3) {
    Result |= DPCD_MAX_PRE_EMPHASIS_REACHED;
  }
  return Result;
}

//
// s5p_dp_init_training + sw_link_training (link_start, clock recovery,
// equalisation), at the panel's maximum, capped to 2.7 Gbps x 4.
//
STATIC
INTN
DpTrain (
  IN OUT EXYNOS_DISPLAY  *Disp
  )
{
  UINT8   Caps[3];
  UINT8   Buf[4];
  UINT8   Status[3];
  UINT8   Adjust[2];
  UINT8   Lanes[LANES_MAX];
  UINT8   CrLoop[LANES_MAX];
  UINTN   Lane;
  UINTN   Count;
  UINTN   Loop;
  BOOLEAN Ok;

  // MACRO_RST after the PLL lock (inter-pair skew), at least 10 us.
  SETB (DP_PHY_TEST, MACRO_RST);
  MicroSecondDelay (10);
  CLRB (DP_PHY_TEST, MACRO_RST);

  if (DpcdRead (Disp, DPCD_REV, Caps, 3) != 0) {
    return -10;
  }
  Disp->DpcdRev   = Caps[0];
  Disp->LinkRate  = ((Caps[1] == LINK_RATE_2_70GBPS) || (Caps[1] == LINK_RATE_1_62GBPS)) ? Caps[1] : LINK_RATE_1_62GBPS;
  Disp->LaneCount = (UINT8)MIN ((UINT32)(Caps[2] & 0x1F), (UINT32)LANES_MAX);
  if (Disp->LaneCount == 0) {
    Disp->LaneCount = 1;
  }
  Count = Disp->LaneCount;
  WR (DP_PHY_PD, 0);

  // link_start
  if (DpcdWriteByte (Disp, DPCD_SINK_POWER_STATE, DPCD_POWER_D0) != 0) {
    return -11;
  }
  WR (DP_LINK_BW_SET, Disp->LinkRate);
  WR (DP_LANE_COUNT_SET, Disp->LaneCount);
  Buf[0] = Disp->LinkRate;
  Buf[1] = Disp->LaneCount;
  if (DpcdWrite (Disp, DPCD_LINK_BW_SET, Buf, 2) != 0) {
    return -12;
  }
  for (Lane = 0; Lane < Count; Lane++) {
    WR (DP_LN_LINK_TRAINING_CTL (Lane), 1u << PRE_EMPHASIS_SET_SHIFT);   // PRE_EMPHASIS_LEVEL_1
    Lanes[Lane]  = 0;
    CrLoop[Lane] = 0;
  }
  SetTrainingPattern (Disp, SW_TRAINING_PATTERN_PTN1);
  if ((DpcdWriteByte (Disp, DPCD_TRAINING_PATTERN_SET, DPCD_SCRAMBLING_DISABLED | DPCD_TRAINING_PATTERN_1) != 0) ||
      (DpcdWrite (Disp, DPCD_TRAINING_LANE0_SET, Lanes, (UINT32)Count) != 0)) {
    return -13;
  }

  // clock recovery
  for (Loop = 0; ; Loop++) {
    MicroSecondDelay (100);
    if ((DpcdRead (Disp, DPCD_LANE0_1_STATUS, Status, 2) != 0) ||
        (DpcdRead (Disp, DPCD_ADJUST_REQUEST_LANE0_1, Adjust, 2) != 0)) {
      return -14;
    }
    for (Ok = TRUE, Lane = 0; Lane < Count; Lane++) {
      if ((LaneStatus (Status, Lane) & DPCD_LANE_CR_DONE) == 0) {
        Ok = FALSE;
      }
    }
    if (Ok) {
      break;
    }
    for (Lane = 0; Lane < Count; Lane++) {
      UINT8  New = TrainingValue (Adjust, Lane);

      if ((New & 3) == 3 || ((New >> 3) & 3) == 3) {
        DEBUG ((DEBUG_ERROR, "Display: CR at maximum swing / pre-emphasis\n"));
        return -15;
      }
      if (((New ^ Lanes[Lane]) & 0x1B) == 0) {
        if (++CrLoop[Lane] == MAX_CR_LOOP) {
          DEBUG ((DEBUG_ERROR, "Display: CR max loop (status %02x %02x)\n", Status[0], Status[1]));
          return -16;
        }
      }
      Lanes[Lane] = New;
      WR (DP_LN_LINK_TRAINING_CTL (Lane), New);
    }
    if (DpcdWrite (Disp, DPCD_TRAINING_LANE0_SET, Lanes, (UINT32)Count) != 0) {
      return -17;
    }
  }

  // equalisation with pattern 2
  SetTrainingPattern (Disp, SW_TRAINING_PATTERN_PTN2);
  for (Lane = 0; Lane < Count; Lane++) {
    Lanes[Lane] = TrainingValue (Adjust, Lane);
    WR (DP_LN_LINK_TRAINING_CTL (Lane), Lanes[Lane]);
  }
  if ((DpcdWriteByte (Disp, DPCD_TRAINING_PATTERN_SET, DPCD_SCRAMBLING_DISABLED | DPCD_TRAINING_PATTERN_2) != 0) ||
      (DpcdWrite (Disp, DPCD_TRAINING_LANE0_SET, Lanes, (UINT32)Count) != 0)) {
    return -18;
  }
  for (Loop = 0; ; Loop++) {
    MicroSecondDelay (400);
    if ((DpcdRead (Disp, DPCD_LANE0_1_STATUS, Status, 3) != 0) ||
        (DpcdRead (Disp, DPCD_ADJUST_REQUEST_LANE0_1, Adjust, 2) != 0)) {
      return -19;
    }
    for (Ok = TRUE, Lane = 0; Lane < Count; Lane++) {
      if ((LaneStatus (Status, Lane) & DPCD_LANE_CR_DONE) == 0) {
        DEBUG ((DEBUG_ERROR, "Display: CR lost during EQ (status %02x %02x)\n", Status[0], Status[1]));
        return -20;
      }
      if ((LaneStatus (Status, Lane) & DPCD_CHANNEL_EQ_BITS) != DPCD_CHANNEL_EQ_BITS) {
        Ok = FALSE;
      }
    }
    if (Ok && ((Status[2] & DPCD_INTERLANE_ALIGN_DONE) != 0)) {
      break;
    }
    if (Loop >= MAX_EQ_LOOP) {
      DEBUG ((DEBUG_ERROR, "Display: EQ max loop (status %02x %02x align %02x)\n", Status[0], Status[1], Status[2]));
      return -21;
    }
    for (Lane = 0; Lane < Count; Lane++) {
      Lanes[Lane] = TrainingValue (Adjust, Lane);
      WR (DP_LN_LINK_TRAINING_CTL (Lane), Lanes[Lane]);
    }
    if (DpcdWrite (Disp, DPCD_TRAINING_LANE0_SET, Lanes, (UINT32)Count) != 0) {
      return -22;
    }
  }

  // training_pattern_dis
  SetTrainingPattern (Disp, 0);
  if (DpcdWriteByte (Disp, DPCD_TRAINING_PATTERN_SET, 0) != 0) {
    return -23;
  }
  return 0;
}

INTN
DpLinkUp (
  IN OUT EXYNOS_DISPLAY  *Disp
  )
{
  UINT8  Data;
  INTN   Result;

  Result = DpInit (Disp);
  if (Result != 0) {
    return Result;
  }

  //
  // Scrambling and enhanced framing off (s5p_dp_enable_scramble (0),
  // enable_rx_to_enhanced_mode (0)); the first AUX traffic.
  //
  SETB (DP_TRAINING_PTN_SET, SCRAMBLING_DISABLE);
  if (DpcdRead (Disp, DPCD_TRAINING_PATTERN_SET, &Data, 1) != 0) {
    DEBUG ((DEBUG_ERROR, "Display: no AUX reply from the panel\n"));
    return -30;
  }
  DpcdWriteByte (Disp, DPCD_TRAINING_PATTERN_SET, (UINT8)(Data | DPCD_SCRAMBLING_DISABLED));
  DpcdRead (Disp, DPCD_LANE_COUNT_SET, &Data, 1);
  DpcdWriteByte (Disp, DPCD_CONFIGURATION_SET, 0);
  DpcdWriteByte (Disp, DPCD_LANE_COUNT_SET, (UINT8)(Data & 0x1F));
  CLRB (DP_SYS_CTL_4, ENHANCED);

  Result = DpTrain (Disp);
  if (Result != 0) {
    SetTrainingPattern (Disp, 0);
    DpcdWriteByte (Disp, DPCD_TRAINING_PATTERN_SET, 0);
    return Result;
  }
  WR (DP_LANE_COUNT_SET, Disp->LaneCount);
  WR (DP_LINK_BW_SET, Disp->LinkRate);

  // init_video
  WR (DP_COMMON_INT_STA_1, VSYNC_DET | VID_FORMAT_CHG | VID_CLK_CHG);
  WR (DP_SYS_CTL_1, 0);
  WR (DP_SYS_CTL_2, CHA_CRI (4) | CHA_CTRL);
  WR (DP_SYS_CTL_3, 0);
  WR (DP_VIDEO_CTL_8, VID_HRES_TH (2) | VID_VRES_TH (0));
  return 0;
}

//
// s5p_dp_config_video(): slave mode (FIMD1 drives timing and clock), RGB
// 8 bpc VESA; FIMD must already be running.
//
INTN
DpVideoOn (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  UINTN   Wait;
  UINTN   Good;
  UINT32  Reg;

  Reg  = RD (DP_FUNC_EN_1);
  Reg &= ~(MASTER_VID_FUNC_EN_N | SLAVE_VID_FUNC_EN_N);
  Reg |= MASTER_VID_FUNC_EN_N;
  WR (DP_FUNC_EN_1, Reg);
  CLRB (DP_VIDEO_CTL_10, INTERACE_SCAN_CFG | VSYNC_POLARITY_CFG | HSYNC_POLARITY_CFG);
  WR (DP_SOC_GENERAL_CTL, AUDIO_MODE_SPDIF_MODE | VIDEO_MODE_SLAVE_MODE);
  WR (DP_VIDEO_CTL_2, IN_BPC_8_BITS);           // VESA range, RGB
  CLRB (DP_VIDEO_CTL_3, 1u << 7);               // ITU601 coefficients (unused for RGB)

  if ((RD (DP_DEBUG_CTL) & PLL_LOCK) == 0) {
    return -40;
  }
  // slave video stream clock from FIMD
  for (Wait = 0; ; Wait++) {
    WR (DP_SYS_CTL_1, RD (DP_SYS_CTL_1));
    WR (DP_SYS_CTL_2, RD (DP_SYS_CTL_2));
    if (((RD (DP_SYS_CTL_1) & DET_STA) != 0) && ((RD (DP_SYS_CTL_2) & CHA_STA) == 0)) {
      break;
    }
    if (Wait > TIMEOUT_LOOPS * 10) {
      DEBUG ((DEBUG_ERROR, "Display: no video clock from FIMD (SYS_CTL_1 0x%x SYS_CTL_2 0x%x)\n",
              RD (DP_SYS_CTL_1), RD (DP_SYS_CTL_2)));
      return -41;
    }
    MicroSecondDelay (10);
  }

  // calculated M/N, timing from capture, unmute, slave, video on
  CLRB (DP_SYS_CTL_4, FIX_M_VID);
  WR (DP_N_VID_0, 0x00);
  WR (DP_N_VID_1, 0x80);
  WR (DP_N_VID_2, 0x00);
  CLRB (DP_VIDEO_CTL_10, FORMAT_SEL);
  CLRB (DP_VIDEO_CTL_1, HDCP_VIDEO_MUTE);
  Reg = RD (DP_SOC_GENERAL_CTL);
  WR (DP_SOC_GENERAL_CTL, (Reg & ~VIDEO_MODE_MASK) | VIDEO_MODE_SLAVE_MODE);
  SETB (DP_VIDEO_CTL_1, VIDEO_EN);

  for (Wait = 0, Good = 0; Good <= 10; Wait++) {
    WR (DP_SYS_CTL_3, RD (DP_SYS_CTL_3));
    if ((RD (DP_SYS_CTL_3) & STRM_VALID) != 0) {
      Good++;
    } else {
      Good = 0;
    }
    if (Wait > TIMEOUT_LOOPS) {
      DEBUG ((DEBUG_ERROR, "Display: video stream not valid (SYS_CTL_3 0x%x)\n", RD (DP_SYS_CTL_3)));
      return -42;
    }
    MicroSecondDelay (1000);
  }
  return 0;
}

VOID
DpVideoOff (
  IN EXYNOS_DISPLAY  *Disp
  )
{
  CLRB (DP_VIDEO_CTL_1, VIDEO_EN);
  WR (DP_PHY_PD, DP_PHY_PD_ALL);
  SETB (DP_PLL_CTL, DP_PLL_PD);
}

#undef RD
#undef WR
