# Thermal (Exynos5420 TMU) — prepared, not run

Status (2026-11-03): built, **untested**, only in the thermal variant image
(`./build-thermal.sh` → `Artifacts/boot-v1awifi-dma-gpu-pep-thermal.img`,
which also has PEP0 + GPU0 + PL330 DMA). Default images are unchanged
(`PlatformThermal.h` = 0, TmuDxe only with `-D EXYNOS_THERMAL=TRUE`).

## Pieces

| Piece | What it does |
|---|---|
| `TmuCore.{c,h}` | shared core: TMU clocks (GATE_IP_PERIS 21 / 22), triminfo reload, per-sensor trim (5420 ring: CPU2 ← block 3, CPU3 ← block 4, GPU ← block 2; 55 if outside 16..76), thresholds 75 / 90 / 110 / 115 °C, CONTROL (Vref 16, gain 5, noise-cancel 4, mux 6, trip enable, core on), °C = code − trim + 25 |
| `TmuDxe` | starts the sensors at boot (for the OS), arms the **115 °C hardware trip** (PMU powers off without software), logs temperatures once a minute in UEFI |
| `Application/ThermalTestApp` | triminfo / trims, 10 readings with gain 8 (mainline) then 5 (Samsung board file), raw codes; staged in `~/exynos5/thermal-test` |
| DSDT `\_TZ.TZ00` / `TZ01` | ACPI thermal zones reading the TMU registers directly (same calibration in AML): CPU = hottest of CPU0..3, passive trip 90 °C on CPU0..3 (`_PSL`), critical 110 °C, polled every 3 s; GPU critical 110 °C |

No Windows driver is needed for temperatures: RT 8.1's ACPI thermal
zones evaluate `_TMP`. Passive cooling (`_PSV` / `_PSL`) can only act once
the processors have performance states, i.e. after exynospep's DVFS.

## Sources

Samsung 3.4 `drivers/thermal/exynos_thermal.c` + board data
(`board-universal5420-power.c`), mainline
`drivers/thermal/samsung/exynos_tmu.c` (5420 triminfo ring, clocks), and
`~/exynos5/thermals.md` (validated against both). Open question: gain 5
(board file) vs 8 (mainline) — ThermalTest prints both; the right one gives
room-ish temperatures on a cool tablet.

## Test (UEFI shell)

    cd ~/exynos5/thermal-test && sudo /bin/sh -c "$PWD/push.sh ThermalTest.efi startup.nsh bootshell.flag"
    sudo /bin/sh -c "python3 ~/exynos5/uefi/Exynos5420Pkg/Tools/usbdbg.py reboot normal"

Then flash the thermal variant: the boot log shows `Tmu: trim ...` and
`Tmu: boot: CPU .. GPU .. C`; in Windows the thermal zones appear under
ACPI (and in the OS log nothing — they are pure AML).
