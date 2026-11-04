/** @file
  Software auto exposure for the camera sensor registers (SMIA / Sony
  layout): exposure in lines (0x0202), analog gain code (0x0204,
  gain = 256 / (256 - code), max code 224 = 8x), digital gain 8.8 (0x020E..,
  0x100 = 1x, max 0xFFF).  The A5 firmware does no exposure control without
  shot requests, so the host does it from the frame's brightness.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __CAM_AE_H__
#define __CAM_AE_H__

#include "CamSensorI2c.h"

typedef struct {
  UINT32  Lines;          // coarse integration time in lines
  UINT32  AnalogX256;     // analog gain x 256 (256 = 1x .. 2048 = 8x)
  UINT32  DigitalX256;    // digital gain x 256 (256 = 1x .. 4095)
  UINT32  MaxLines;       // frame_length - 6
  UINT32  Target;         // wanted mean of the green samples above black (10-bit levels)
  UINT32  Kind;           // CAM_AE_IMX134 / CAM_AE_S5K6B2: analog gain code format and limits
  UINT32  MaxDigital;     // digital gain x 256 limit
} CAM_AE;

#define CAM_AE_IMX134  0       // analog code = 256 - 256 / gain (0 = 1x, 224 = 8x)
#define CAM_AE_S5K6B2  1       // analog code = 32 x gain (0x20 = 1x), limited to 8x here

VOID
CamAeInit (
  OUT CAM_AE  *Ae,
  IN  UINT32  MaxLines,
  IN  UINT32  Target,
  IN  UINT32  Kind
  );

//
// One control step from the measured mean (above black, > 0).  Moves the
// total exposure (lines x analog x digital) towards the target by at most
// 2x either way per step.
//
VOID
CamAeStep (
  IN OUT CAM_AE  *Ae,
  IN     UINT32  Mean
  );

//
// Writes the state to the sensor (grouped parameter hold).  0 or < 0.
//
INTN
CamAeApply (
  IN CAM_I2C  *I2c,
  IN CAM_AE   *Ae
  );

#endif // __CAM_AE_H__
