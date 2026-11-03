#!/bin/bash
# Thermal variant (prepared, not the default image): PL330 DMA on, the DSDT
# thermal zones (PlatformThermal.h) and TmuDxe in the FD.  Restores both
# headers afterwards; the image goes to
# Artifacts/boot-v1awifi-dma-gpu-pep-thermal.img.
set -e
cd "$(dirname "$0")"
restore() {
    sed -i 's/#define EXYNOS_PL330_DMA  1/#define EXYNOS_PL330_DMA  0/' AcpiTables/PlatformDma.h
    sed -i 's/#define EXYNOS_THERMAL  1/#define EXYNOS_THERMAL  0/' AcpiTables/PlatformThermal.h
}
trap restore EXIT
sed -i 's/#define EXYNOS_PL330_DMA  0/#define EXYNOS_PL330_DMA  1/' AcpiTables/PlatformDma.h
sed -i 's/#define EXYNOS_THERMAL  0/#define EXYNOS_THERMAL  1/' AcpiTables/PlatformThermal.h
THERMAL=1 ./build.sh "$@"
cp Artifacts/boot-v1awifi.img Artifacts/boot-v1awifi-dma-gpu-pep-thermal.img
echo "Artifacts/boot-v1awifi-dma-gpu-pep-thermal.img"
