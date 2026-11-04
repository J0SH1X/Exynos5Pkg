# Camera on v1awifi (SM-P900): research and plan

Status (2026-11-04, unattended night session):
- **Step 1 (A5 alive) WORKS** in UEFI: firmware boots, first mailbox message
  `IHC_GET_SENSOR_NUMBER` param1 0x82; secure config via SMC verified (0xFC).
- **Step 2 (rear sensor open + setfile) WORKS** in UEFI (`CameraTest.efi open`):
  module powered (CamBoard.c), `HIC_OPEN_SENSOR` IMX134 DONE (margin 15x4,
  shared[499] magic), AK7345 actuator initialised, setfile_imx134.bin loaded
  (5 x "SETFILE V3"), stream off / process stop DONE, `HIC_POWER_DOWN` -> A5 WFI,
  domain fully off.
- **Windows: `ExynosWinDrivers/exynoscam`** (EXYN5015, DSDT CAM0) builds:
  milestone-1 driver (power, firmware from System32\drivers, SMC secure config,
  ISR on GSI 51). **Not installed/tested yet** (needs TWRP + dism by the user).
- **Step 3a (raw frame) WORKS** (2026-11-04 14:10, `CameraTest.efi raw`):
  CamCapture.c arms D-PHY 0 + CSIS0 (4 lanes, RAW10, settle 15) + FIMC-Lite A
  (DMA, one slot, OTF off), then `HIC_STREAM_ON` -> firmware "IMX134_StreamOn
  SUCCESS", FIMC-Lite frame start/end, full 3280x2458 RAW10 frame (u16 per
  pixel, black ~64) in memory, saved to fs0 `\frame.raw`. Pull with
  `usbdbg.py pull '\frame.raw' out --vol 0` (~55 s), convert with
  `~/exynos5/camera-test/raw2png.py` (Bayer most likely RGGB; GRBG/GBRG come out grey).
  First image: ceiling + LED light bar, unfocused (AF not driven), ~/exynos5/camera-test/frames/.
  Firmware complains "[CSIS_ISRHandler] Fimc-lite stopped already" after we stop
  FIMC-Lite (it tracks FLITE itself; harmless).
- **Windows milestone 2 built, untested** (ExynosWinDrivers 1.0.0.123):
  `exynoscam` runs the UEFI sequence in a worker thread (shared FimcIsCore.c,
  FimcIsHost.c, CamBoard.c, CamCapture.c) and writes C:\malikm\camera.raw;
  sensor rails via the new **`exynospmic`** (EXYN5016, sole owner of the PMIC
  I2C, interface common/ExPmicIf.h). mxt1664s / exynosgpu use that interface
  when their DSDT PMIC connection is gone. The DSDT in the current build moves
  the PMIC connection to the PMIC device: old exynosgpu + new DSDT = GPU fails,
  so install exynospmic + exynosgpu + mxt1664s + exynoscam together with it.
- **Front camera (S5K6B2) works in UEFI** (2026-11-04, `CameraTest.efi raw front topmux`):
  CamFrontPowerOn (GPY7(0) AVDD, LDO19 1.8 V, reset GPE0(4), ID GPY7(2) low, MCLK sclk_isp_sensor2 /
  GPH0(7) func 2, ISP I2C2 = GPF0(4/5) func 2), module 9 (S5K6B2) with ext I2CChannel 2 (all else
  zero), setfile_6b2.bin (same firmware as the rear), CSIS1 0x13C30000 + FIMC-Lite B 0x13C10000 +
  D-PHY 1, 1936x1090 RAW10, settle 16 -> \front.raw, a sharp ceiling image. **Key fix: maximum
  drive strength (GPFx DRV +0xC = 3 per pin) on the SDA / SCL pins** (the kernel does it); without
  it the A5 logs `I2C_WaitForBusReady [ch 2] timeout` and the open fails. Now default for all
  ISP I2C pins (rear too). FIMC-Lite B skips CIGENERAL.
- **Exposure control (2026-11-04):** the A5 never sends the sensor exposure/gain without HIC_SHOT
  requests, so streaming frames are very dark (default 302 lines / 1x on the rear). The host sets
  them itself over the sensor's I2C: CamSensorI2c.c (bit-bang on the ISP I2C pins, taken from the
  ISP per transfer; GPF0(0/1) rear sensor at 7-bit 0x1A, GPF0(4/5) front S5K6B2 at 0x35, model id
  0x6B20) + CamAe.c (software AE: exposure lines 0x0202 up to frame_length-6, analog gain 0x0204
  (IMX: code = 256 - 256/gain, max 224; S5K6B2: 0x20 = 1x), digital gain 8.8 at 0x020E..0x0214,
  grouped hold 0x0104). Test: `CameraTest.efi raw [front] topmux ae` (converges in ~9 steps even in
  a dark room); `i2c` / `expo=N again=N dgain=N` probe and set by hand. Rear: 3280x2458 frame_length
  2609 line_length 3600; front: 1936x1090 frame_length 1125 line_length 2192.
- **Windows rear camera works** (exynoscam 1.0.0.137): verified by camtest.exe, mftest.exe, mfpreview.exe
  (desktop capture-engine preview) and a photo from the Store Camera app. Store-app live preview is black
  (no GPU driver, WDDM removed on purpose). Needed: TopMux on, I2C drive strength, KS_FRAME_INFO + DataUsed,
  timestamps in system time, host AE.
- (history) Windows camera app (AVStream exynoscam): frames delivered but the app shows black while the raw
  frames are real (dim scene); diagnosing (timestamps / format). The raw buffer can be dumped from
  the running tablet with `usbdbg.py dump <PA> <len>`.
- Open: module calibration (F-ROM over ISP SPI0) not read -> firmware logs
  "FROM HEADER MAP VERSION check: FAIL" (harmless so far). Streaming not started.

### Hard-won facts (2026-11-04)
- Secure monitor convention: after `smc`, r0 stays -101 (SMC_CMD_REG) and the
  **status is in r1** (0 = done), read value in r2 (`exynos_smc_readsfr`).
  Treating r0 != 0 as failure was the first test's false "secure config failed".
- Interrupt: MCUCTL INTR_GENERAL -> combiner group 19 input 1 (ARMISP) ->
  **GIC INTID 51 = SPI 19 = GSI 51**. INTMR1 = 0x340 set by the firmware.
  Sample the GIC *before* acknowledging (INTCR1) or nothing is pending.
- Host -> A5: wait INTMSR0 bit 0 clear, write hicmd/hic_sensorid/param1..4
  (ISSR0 +0x00..+0x14), INTGR0 = 1. Replies come as general messages
  ISR_DONE (0x2000) / ISR_NDONE with param1 = the command.
- Answer `IHC_GET_SENSOR_NUMBER` with ISR_DONE, param1 0x1000, param2 3 (enum).
- Region for instance 0 at A5 + 0x13FB000; `shared[]` at +0x2D34 (struct
  is_region offsets compiled from the kernel header for ARM); A5 address of
  shared[] = BBOAR + 0x13FB000 + 0x2D34 goes in HIC_OPEN_SENSOR param2.
- Firmware text log: A5 + 0x1340000 (0x7D000 ring), write offset (absolute,
  from A5 base) at A5 + 0x13BD000. CameraTest prints it as `fw|` lines.
- Firmware memory map (from its log): cache 0..0x1200000, heap 0x290000,
  setfile 0x1200000 (+0x140000), debug 0x1340000, DCTRL 0x13BD000, share 0x13C0000.
- IMX134 module: actuator is **AK7345 (7)** (kernel overrides WV560 for IMX134;
  WV560 gave "Sensor I2C doesn't work. I2C ch(1)"). Flash MAX77693 GPIO 0/1.
- MCLK sclk_isp_sensor0: CMU TOP SCLK_SRC_ISP 0x10020270 [30:28] = 5 (IPLL),
  SCLK_DIV_ISP 0x10020580 [15:8] = 23 (18 MHz, stock V1A), gate 0x10020870 bit 4.
  GPIO banks: GPE0/GPF0/GPF1 = 0x14000000 + 0/0x40/0x60, GPH0 = 0x14010100.
  LDO24 1.05 V (kernel apply_uV min), LDO18 1.8 V; bootloader leaves them 0x28 / 0x14 (off).
- Power down: HIC_POWER_DOWN first (firmware enters WFI), then ISP_ARM OPTION
  0x10000 -> 0, sys-pwr regs 0, domain off. Without it the domain stays on.

Firmware blobs (A5 firmware, setfiles): ExynosWinDrivers/firmware/ (stock LRX22G.P900XXS0BPL2, see its README);
the UEFI test loads them from the volume root (push with ~/exynos5/camera-test/run.sh).

Sources:
- Samsung 3.4 tree `android_kernel_samsung_exynos5420` with `lineageos_v1awifi_defconfig`;
- stock `/system/vendor/firmware` from `~/Downloads/system.img`.

Every fact below names its file; nothing comes from `~/exynos5/cam.md`
unless it was re-checked.

## How the camera works on this SoC

The camera does not follow the "sensor driver in the kernel" model. The
**FIMC-IS** block contains its own **Cortex-A5**, which runs Samsung firmware:

- the firmware talks to the sensors over the ISP's own I2C (GPF0 pins);
- it also drives the AF actuator;
- it runs 3A, the ISP pipeline and scalers;
- the kernel (`fimc-is-mc2`, `CONFIG_VIDEO_EXYNOS5_FIMC_IS=y`) only powers
  things, loads firmware and setfiles, and exchanges commands through a
  mailbox (`ISSR` registers).

To get a picture we have to replicate the host side of that protocol.
Writing sensor drivers is not needed.

| Piece | Rear | Front |
|---|---|---|
| Sensor | Sony **IMX134**, 8 MP (fw `fimc_is_fw2_IMX134.bin`; `fimc_is_ischain_power` picks IMX134 when the module header says Sony and 8 MP) | Samsung **S5K6B2**, 2 MP |
| Setfile | `setfile_imx134.bin` | `setfile_6b2.bin` |
| Module ID / calibration | SPI F-ROM on the module, read over the ISP SPI0 pins GPF1(0..3) (`fimc_is_sec_fw_sel`, `#else` of `CONFIG_CAMERA_EEPROM`) | — |
| AF | AK7345, driven by the A5 firmware | fixed focus |
| CSI | port A / FIMC-Lite 0 | `CONFIG_S5K6B2_CSI_D` → `gscl_flite1`, `CAM_PORT_B` (port B / CSIS1 / FIMC-Lite 1) |
| Reset | GPE0(5) `GPIO_MAIN_CAM_RESET` | GPE0(4) `GPIO_CAM_VT_nRST` |
| MCLK pin | GPH0(5), function 2 | GPH0(7), function 2 |
| Sensor I2C | GPF0(0/1) func 2 (ISP I2C0), AF GPF0(2/3) | GPF0(4/5) func 2 (ISP); vision mode: i2c-gpio `SEC_VISION_SWI2C_ID` |

The kernel's `s5k6b2` driver (`CONFIG_VISION_MODE`) is only the
low-power vision mode. Normal front-camera use also goes through the
FIMC-IS firmware with `setfile_6b2.bin` (`CAMERA_DUAL_FRONT`).

### Power (board-universal5420-media.c `gpio_universal5420`, V1A = the `#else` branches; board-v1-pmic.c)

**Rear sensor, in kernel order:**

| Step | Rail or pin | Source |
|---|---|---|
| AVDD 2.8 V | GPE0(7) `GPIO_CAM_IO_EN` high | GPIO |
| DVDD core | MAX77802 **LDO24**, 1.05 to 1.2 V | PMIC |
| AF 2.8 V | GPE0(6) `GPIO_CAM_AF_EN` high | GPIO |
| HOST / IO 1.8 V | MAX77802 **LDO18** | PMIC |
| MCLK | GPH0(5) | GPIO |
| Reset release | GPE0(5) | GPIO |
| ISP I2C / AF I2C | GPF0(0..3) func 2 | GPIO |
| Flash | GPE0(0/1) func 2 | GPIO |
| F-ROM SPI | GPF1(0..3) | GPIO |

**Front sensor:**
- GPY7(0) `A28V_VT_EN` high;
- MAX77802 **LDO19** 1.8 V;
- GPE0(4) reset;
- GPY7(2) `VT_CAM_ID` = 0;
- GPH0(7) MCLK;
- GPF0(4/5) I2C.

**MAX77802 LDO control:**
- registers: `LDOnCTRL1` = 0x60 + (n − 1), so LDO18 = 0x71, LDO19 = 0x72, LDO24 = 0x77;
- bits 7:6 = mode (3 = on), bits 5:0 = voltage;
- LDO18/19/24 are P-channel: 800 mV + 50 mV × code;
- 1.8 V = 0x14, 1.2 V = 0x08, 1.1 V = 0x06;
- the PMIC is on HS-I2C3, address 0x09, the same bus as the GPU test.

**Open question:** in `board-v1-pmic.c`, LDO24's consumer supply is named
`cam_sensor_core_1.1v`, but the media file asks for `cam_sensor_core_1.2v`.
In this tree that `regulator_get` would fail. Either stock differs, or
the core rail is already on. We drive LDO24 directly; start at 1.1 V.

### ISP block (fimc-is-regs.h, fimc-is-device-ischain.c, setup-fimc-is.c, clock-exynos5420.c, pm_domains5.c)

**Register blocks:**
- FIMC-IS: 0x13000000, size 0x1A0000.
- MCUCTL: +0x180000.
  - BBOAR: +4 (A5 boot address).
  - INTCR1 / INTMR1 / INTMSR1: +0x20 / +0x24 / +0x2C.
  - ISSR0: +0x80. The mailbox `struct is_common_reg` starts here: host→A5 `hicmd` at +0x00; A5→host `ihcmd_iflag` at +0x24 and `ihcmd` at +0x28.
- IRQs (downstream): `COMBINER_IRQ(19,1)` ARMISP and `(19,4)` ISP. The test logs which GIC SPI actually fires.

**Power:**
- `pd-isp` is PMU 0x10044020 (CONFIGURATION = 7, STATUS & 7).
- The A5 is PMU 0x10042480 CONFIGURATION, 0x10042484 STATUS and 0x10042488 OPTION.
- On power-down, clear the sys-power registers 0x10041584, 0x100414C4 and 0x10041090.

**Clocks (`exynos5420_fimc_is_cfg_clk`):**

| Clock | Rate | Divider register |
|---|---|---|
| aclk_400_isp | 400 MHz | — |
| mcuisp_div0 | 200 MHz | CLKDIV_ISP1 [2:0] (0x1001C304) |
| mcuisp_div1 | 100 MHz | CLKDIV_ISP1 [6:4] |
| aclk_333_432_isp | 432 MHz (IPLL) | — |
| aclk_div0 | 216 MHz | CLKDIV_ISP0 [2:0] (0x1001C300) |
| aclk_div1 | 108 MHz | CLKDIV_ISP0 [6:4] |
| aclk_div2 | 54 MHz | CLKDIV_ISP2 [2:0] (0x1001C308) |
| aclk_266_isp | 267 MHz | — |

User muxes:
- SRC_TOP3 bit 0 (aclk_400_isp);
- SRC_TOP4 bits 4, 12 and 16 (333_432_isp, 333_432_isp0, 266_isp).

**Gates (`clk_on`):**

| Gate register | Address | Bits |
|---|---|---|
| GATE_IP_ISP0 | 0x1001C800 | (0x3FFFF<<14) \| 0x1F \| (7<<5) \| (1<<3) \| (1<<1) |
| GATE_IP_ISP1 | 0x1001C804 | (0x3F<<17) \| (3<<12) \| (0xF<<8) \| 7 |
| GATE_IP_GSCL0 | 0x10020910 | `gscl_3aa` (1<<19) \| (1<<9) \| (1<<4) |

**A5 start (`fimc_is_ischain_power`):**
1. Load the firmware at the start of a 20 MiB region (`FIMC_IS_A5_MEM_SIZE`).
   - Shared region at +0x13C0000; calibration at +0x13D0000.
   - The base is masked by `(1<<26)-1`, so we align it to 64 MiB.
2. Pixel async reset: clear SYSREG 0x10050224 bit 25 and 0x1005022C bit 5.
3. Write BBOAR = base.
4. **Secure monitor** (`CONFIG_ARM_TRUSTZONE`): `smc` with r0 = −101, r1 = `(1<<30)|(addr>>2)`. Writes:
   - 0x131E0004 = 0xFF;
   - 0x131F0080..88 = ~0;
   - 0x131F0400..444 = 0x10101010.

   Then read back 0x131E0004 with r1 = `(3<<30)|(addr>>2)`; the value comes back in r2 and must be 0xFC.
5. Write ISP_ARM CONFIGURATION = 1 and OPTION = 0x18000, then wait for STATUS bit 0.
6. The firmware's first message is `IHC_GET_SENSOR_NUMBER` (0x1000), with param1 = firmware version. The kernel then sets `IS_IF_STATE_START`.

**IOMMU:** the kernel uses SysMMU and IOVMM, so BBOAR gets an IOVA. After
reset the ISP SysMMUs are off (bypass). UEFI hands the A5 physical
addresses. This is a hypothesis, and the test checks it.

**Firmware:** `fimc_is_fw2_IMX134.bin` is 1 750 995 bytes. Its tail reads
`[E5420 1960-2014/04/23 12:22:54]E08LSHD02OD`. The last 11 bytes are the
version, as in `fimc_is_ischain_version`.

## Corrections to cam.md (DeepSeek)

- **"No rear sensor driver exists / no driver built":** wrong. The rear
  camera does not need a kernel sensor driver. `fimc-is-mc2` is built,
  and the IMX134 is driven by the A5 firmware, which ships in stock.
- **"13 MP rear":** that is the board file's comment, copied from other
  5420 boards. v1awifi's stock ROM has only IMX134 (8 MP) firmware, and
  the kernel picks IMX134 for an 8 MP Sony module.
- **"Camera EEPROM not enabled":** true as a config option, but it misses
  the point. Without `CONFIG_CAMERA_EEPROM`, module identity and
  calibration come from the module's SPI F-ROM over ISP SPI0 (GPF1).
- **"No flash or AF driver":** AF (AK7345) is driven by the firmware.
- **Front port:** "CSI-B / FIMC-Lite 1" matches `CONFIG_S5K6B2_CSI_D` →
  `CAM_PORT_B` / `gscl_flite1`. The config option's letter differs, but
  the port is right.
- **LDO table:** the LDO18 / LDO19 / LDO24 numbers are right. It does not
  mention the LDO24 supply-name mismatch.

## Plan

1. **A5 alive (UEFI, `CameraTest.efi`; next on the device).**
   - Steps: ISP domain, clocks, firmware, secure configuration, A5, first mailbox message.
   - Also identifies the GIC SPI (pending diff) and records the TOP clock state for rate math.
2. **Sensor power + open sensor (UEFI).**
   - Power the rear rails and pins as above.
   - Send `HIC_OPEN_SENSOR` with IMX134 ID / I2C channel / CSI port.
   - Load the setfile through `HIC_GET_SET_FILE_ADDR` and `HIC_LOAD_SET_FILE`.
   - Read the F-ROM calibration into +0x13D0000. Without it the firmware may refuse or use defaults.
   - Port the message encoding from `fimc-is-interface.c` (`fimc_is_hw_open`, `_saddr`, `_setfile`).
3. **First frame (UEFI).**
   - Bring up CSIS0 + D-PHY and FIMC-Lite 0 (or the ISP's own DMA output through the shot / region protocol).
   - Capture one frame to memory and show it on the GOP framebuffer.
   - This is the big step: `struct camera2_shot` / `is_region` parameter blocks.
4. **Windows.**
   - A KMDF driver (`exynoscam`) on the same core: interrupt from step 1, firmware from the driver package.
   - Plus a minimal AVStream camera (KS) mini-driver for RT 8.1's camera stack.
   - Front camera after rear, with the same firmware and `setfile_6b2.bin`.

## Running the step 1 test

```sh
cd ~/exynos5/camera-test && sudo /bin/sh -c "$PWD/push.sh CameraTest.efi fimc_is_fw2_IMX134.bin startup.nsh bootshell.flag"
sudo /bin/sh -c "python3 ~/exynos5/uefi/Exynos5420Pkg/Tools/usbdbg.py reboot normal"
```

Options: `nosmc`, `topmux` and `keep`. Change the line in `startup.nsh` to use them.

**Expected:**
- `CameraTest: A5 says cmd 0x1000 ... -> ALIVE`;
- a `newly pending: GIC ID n` line;
- `0x131E0004 = 0x000000fc`.

**If it fails:**
- **Hang after the `powered:` lines, with no "secure monitor" line:** the SMC is not handled from where
  UEFI runs. Retry with `nosmc`; the A5 may then fault on secure accesses.
- **`A5 start failed at step 8`:** the A5 power did not come up.
- **"no message":** check `ISSR6` (A5 state) and `MCUCTLR` in the
  `after` line. Then try `topmux` (the ISP clocks may be on the 24 MHz
  oscillator) and the IOVA hypothesis.
