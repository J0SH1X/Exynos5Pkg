/** @file
  FIMC-IS host protocol: see FimcIsHost.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "FimcIsHost.h"

STATIC
BOOLEAN
HasText (
  IN CONST CHAR8  *Line,
  IN CONST CHAR8  *Needle
  )
{
  UINTN  Index;
  UINTN  Inner;

  for (Index = 0; Line[Index] != 0; Index++) {
    for (Inner = 0; (Needle[Inner] != 0) && (Line[Index + Inner] == Needle[Inner]); Inner++) {
    }
    if (Needle[Inner] == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

VOID
FimcIsLogDump (
  IN OUT FIMC_IS  *Is
  )
{
  CONST CHAR8  *Log = (CONST CHAR8 *)Is->MemVa + FIMC_IS_FW_LOG_OFFSET;
  UINT32       End;
  CHAR8        Line[160];
  UINTN        Used = 0;
  UINTN        Total = 0;
  CHAR8        C;

  EnvCacheInvalidate ((VOID *)Log, FIMC_IS_FW_LOG_SIZE + 64);
  End = *(volatile UINT32 *)((UINT8 *)Is->MemVa + FIMC_IS_FW_LOG_OFFSET + FIMC_IS_FW_LOG_SIZE);
  if ((End < FIMC_IS_FW_LOG_OFFSET) || (End - FIMC_IS_FW_LOG_OFFSET >= FIMC_IS_FW_LOG_SIZE)) {
    return;
  }
  End -= FIMC_IS_FW_LOG_OFFSET;
  while ((Is->LogPos != End) && (Total < 12000)) {
    C = Log[Is->LogPos];
    Is->LogPos = (Is->LogPos + 1) % FIMC_IS_FW_LOG_SIZE;
    Total++;
    if (C == 0) {
      continue;
    }
    if ((C == '\n') || (Used == sizeof (Line) - 1)) {
      Line[Used] = 0;
      if (Used != 0) {
        if (HasText (Line, "Sensor I2C doesn't work")) {
          Is->SensorI2cFail = TRUE;
        }
        DEBUG ((DEBUG_INFO, "fimcis: fw| %a\n", Line));
      }
      Used = 0;
      if (C == '\n') {
        continue;
      }
    }
    Line[Used++] = ((C >= 0x20) && (C < 0x7F)) ? C : '.';
  }
  if (Used != 0) {
    Line[Used] = 0;
    DEBUG ((DEBUG_INFO, "fimcis: fw| %a\n", Line));
  }
}

INTN
FimcIsCommand (
  IN OUT FIMC_IS      *Is,
  IN     CONST CHAR8  *Name,
  IN     UINT32       Cmd,
  IN     UINT32       Group,
  IN     UINT32       P1,
  IN     UINT32       P2,
  IN     UINT32       P3,
  IN     UINT32       P4,
  OUT    FIMC_IS_MSG  *Reply OPTIONAL
  )
{
  FIMC_IS_MSG  Msg;
  UINTN        Count;

  if (FimcIsSend (Is, Cmd, Group << FIMC_IS_GROUP_SHIFT, P1, P2, P3, P4) != 0) {
    DEBUG ((DEBUG_ERROR, "fimcis: %a: A5 did not take the previous message\n", Name));
    return -1;
  }
  for (Count = 0; Count < 32; Count++) {
    if (FimcIsWaitMessage (Is, 5000, &Msg) != 0) {
      DEBUG ((DEBUG_ERROR, "fimcis: %a: no reply in 5 s\n", Name));
      FimcIsLogDump (Is);
      return -2;
    }
    if (((Msg.Command == ISR_DONE) || (Msg.Command == ISR_NDONE)) && (Msg.Param[0] == Cmd)) {
      DEBUG ((DEBUG_INFO, "fimcis: %a: %a params %08x %08x %08x\n", Name, (Msg.Command == ISR_DONE) ? "DONE" : "NDONE",
              Msg.Param[1], Msg.Param[2], Msg.Param[3]));
      if (Reply != NULL) {
        *Reply = Msg;
      }
      FimcIsLogDump (Is);
      return (Msg.Command == ISR_DONE) ? 0 : -3;
    }
    DEBUG ((DEBUG_INFO, "fimcis: %a: (other) cmd 0x%x id 0x%x params %08x %08x %08x %08x\n", Name, Msg.Command,
            Msg.SensorId, Msg.Param[0], Msg.Param[1], Msg.Param[2], Msg.Param[3]));
  }
  return -4;
}

INTN
FimcIsOpenSensor (
  IN OUT FIMC_IS  *Is,
  IN     UINT32   Group,
  IN     UINT32   Module,
  IN     BOOLEAN  Rear
  )
{
  UINT8             *Region = (UINT8 *)Is->MemVa + FIMC_IS_REGION_OFFSET (0);
  volatile UINT32   *Shared = (volatile UINT32 *)(Region + FIMC_IS_REGION_SHARED);
  FIMC_IS_OPEN_EXT  *Ext = (FIMC_IS_OPEN_EXT *)(Region + FIMC_IS_REGION_SHARED);
  FIMC_IS_MSG       Reply;

  FimcIsLogDump (Is);

  //
  // fimc_is_hw_enum(): answer IHC_GET_SENSOR_NUMBER (3 = up to three sensors).
  //
  if (FimcIsSend (Is, ISR_DONE, 0, IHC_GET_SENSOR_NUMBER, 3, 0, 0) != 0) {
    DEBUG ((DEBUG_ERROR, "fimcis: enum: A5 busy\n"));
    return -1;
  }
  MicroSecondDelay (10000);

  //
  // fimc_is_itf_open(): sensor_open_extended into shared[] (rear IMX134
  // module, fimc-is-device-sensor.c + the IMX134 actuator override in
  // fimc_is_ischain_power), then HIC_OPEN_SENSOR.
  //
  ZeroMem (Region, FIMC_IS_REGION_SIZE);
  if (Rear) {
    Ext->I2cChannel           = 0;
    Ext->Actuator.ProductName = 7;          // ACTUATOR_NAME_AK7345
    Ext->Actuator.PeriType    = 0;          // SE_I2C
    Ext->Actuator.Setting[0]  = 1;          // SENSOR_CONTROL_I2C1
    Ext->Flash.ProductName    = 3;          // FLADRV_NAME_MAX77693
    Ext->Flash.PeriType       = 2;          // SE_GPIO
    Ext->Flash.Setting[0]     = 0;
    Ext->Flash.Setting[1]     = 1;
    Ext->From.ProductName     = 100;        // FROMDRV_NAME_NOTHING
  } else {
    Ext->I2cChannel           = 2;          // front sensor: ISP I2C2, everything else zero
  }
  Ext->I2cSclk              = 108000000;  // I2C_L0
  EnvCacheClean (Region, FIMC_IS_REGION_SIZE);

  if (FimcIsCommand (Is, Rear ? "open sensor IMX134" : "open sensor S5K6B2", HIC_OPEN_SENSOR, 0, Module,
                     Is->MemPa + FIMC_IS_REGION_OFFSET (0) + FIMC_IS_REGION_SHARED, Group, 0, &Reply) != 0) {
    return -2;
  }
  EnvCacheInvalidate (Region, FIMC_IS_REGION_SIZE);
  DEBUG ((DEBUG_INFO, "fimcis: open: margin %ux%u, shared[499] 0x%08x\n", Reply.Param[1], Reply.Param[2],
          Shared[FIMC_IS_SHARED_COUNT - 1]));
  return (Shared[FIMC_IS_SHARED_COUNT - 1] == FIMC_IS_MAGIC) ? 0 : -3;
}

INTN
FimcIsOpenRearSensor (
  IN OUT FIMC_IS  *Is,
  IN     UINT32   Group
  )
{
  return FimcIsOpenSensor (Is, Group, FIMC_IS_SENSOR_IMX134, TRUE);
}

INTN
FimcIsOpenFrontSensor (
  IN OUT FIMC_IS  *Is,
  IN     UINT32   Group
  )
{
  return FimcIsOpenSensor (Is, Group, FIMC_IS_SENSOR_S5K6B2, FALSE);
}

INTN
FimcIsLoadSetfile (
  IN OUT FIMC_IS     *Is,
  IN     UINT32      Group,
  IN     CONST VOID  *Setfile,
  IN     UINTN       Size
  )
{
  FIMC_IS_MSG  Reply;
  UINT32       Addr;

  if (FimcIsCommand (Is, "setfile address", HIC_GET_SET_FILE_ADDR, 0, 0, 0, 0, 0, &Reply) != 0) {
    return -1;
  }
  Addr = Reply.Param[1];
  if ((Addr == 0) || (Addr + Size > FIMC_IS_A5_MEM_SIZE) || (Size > FIMC_IS_SETFILE_MAX)) {
    DEBUG ((DEBUG_ERROR, "fimcis: setfile address 0x%08x / size %u not usable\n", Addr, (UINT32)Size));
    return -2;
  }
  CopyMem ((UINT8 *)Is->MemVa + Addr, Setfile, Size);
  EnvCacheClean ((UINT8 *)Is->MemVa + Addr, Size);
  DEBUG ((DEBUG_INFO, "fimcis: setfile %u bytes at A5 +0x%08x\n", (UINT32)Size, Addr));
  if (FimcIsCommand (Is, "load setfile", HIC_LOAD_SET_FILE, 0, 0, 0, 0, 0, NULL) != 0) {
    return -3;
  }
  FimcIsCommand (Is, "stream off", HIC_STREAM_OFF, 0, 0, 0, 0, 0, NULL);
  FimcIsCommand (Is, "process stop", HIC_PROCESS_STOP, (1u << Group) | (1u << FIMC_IS_GROUP_ISP), 0, 0, 0, 0, NULL);
  return 0;
}
