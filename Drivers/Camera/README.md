# Camera on v1awifi (SM-P900): research and plan

Status (2026-11-03): **prepared, nothing has run on the device.**
Shared core `FimcIsCore.{c,h}` and the UEFI test `Application/CameraTestApp`
(staged in `~/exynos5/camera-test`) build. There is no Windows driver yet.

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
