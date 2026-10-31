#!/bin/bash
#
#  Build script for Exynos5420Pkg (Galaxy Tab Pro/Note Pro 12.2 WiFi,
#  codename v1awifi, board universal5420).
#
#  Modelled on Exynos441xPkg's build.sh: sets up WORKSPACE/PACKAGES_PATH,
#  builds BaseTools once, applies the GenFw patches the ARM build needs,
#  builds the BootShim, runs `build`, prepends the shim to the FD, then
#  packages the result into an Android boot.img (this bootloader loads
#  boot.img from the BOOT partition, not a raw zImage).
#
#  Usage:
#    ./build.sh              # DEBUG build, output on the framebuffer
#    ./build.sh --uart       # DEBUG build, output on UART2 (115200 8N1)
#    TARGET=RELEASE ./build.sh
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

set -e

export WORKSPACE=${WORKSPACE:-$PWD/workspace}
mkdir -p "$WORKSPACE"
export EDK_TOOLS_PATH=$PWD/../edk2/BaseTools
# NB: PACKAGES_PATH gets the parent of this package so that -p
# Exynos5420Pkg/Exynos5420Pkg.dsc resolves against it (same layout as
# Exynos441xPkg, whose package root is a PACKAGES_PATH entry).
export PACKAGES_PATH=$PWD/../edk2:$PWD/../edk2-platforms:$PWD/..

# Where the finished image lands
OUT_DIR=$PWD/Artifacts
mkdir -p "$OUT_DIR"

UART=0
for arg in "$@"; do
    case "${arg}" in
        --uart) UART=1 ;;
        *) echo "Unknown argument: ${arg} (--uart)"; exit 1 ;;
    esac
done

DSCOPTS=()
if [ "${UART}" = 1 ]; then
    DSCOPTS=(-D USE_SCREEN_FOR_SERIAL_OUTPUT=0 -D USE_UART_FOR_SERIAL_OUTPUT=1)
    echo "Debug output on UART2, framebuffer console off"
else
    DSCOPTS=(-D USE_SCREEN_FOR_SERIAL_OUTPUT=1 -D USE_UART_FOR_SERIAL_OUTPUT=0)
    echo "Debug output on the framebuffer"
fi

function _patch_edk() {
    # GenFw cannot encode R_ARM_PC13 relocations (a clang-ism that shows up in
    # the ARM builds); map it onto R_ARM_REL32 like HtcLeoPkg does.
    local file="$EDK_TOOLS_PATH/Source/C/GenFw/Elf32Convert.c"
    if ! grep -q "case R_ARM_PC13:" "${file}"; then
        sed -i '/case R_ARM_REL32:/a\        case R_ARM_PC13:' "${file}"
        echo "Patched GenFw: R_ARM_PC13 -> R_ARM_REL32"
    fi

    # Some builds trip over an uninitialised Status in the English collation
    # driver; harmless to force to EFI_SUCCESS.
    local collate="$PWD/../edk2/MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/UnicodeCollationEng.c"
    if grep -q "EFI_STATUS  Status;" "${collate}"; then
        sed -i 's/EFI_STATUS  Status;/EFI_STATUS  Status = EFI_SUCCESS;/' "${collate}"
        echo "Patched UnicodeCollationEng.c"
    fi
}

function _base_tools() {
    if [ -n "${REBUILD_BASETOOLS}" ]; then
        make clean -C "$EDK_TOOLS_PATH"
        make -C "$EDK_TOOLS_PATH" -j"$(nproc)"
    elif [ ! -x "$EDK_TOOLS_PATH/Source/C/bin/VfrCompile" ] || \
         [ ! -x "$EDK_TOOLS_PATH/Source/C/bin/GenFw" ]; then
        make -C "$EDK_TOOLS_PATH" -j"$(nproc)"
    else
        echo "BaseTools present, skipping (REBUILD_BASETOOLS=1 to force)"
    fi
}

_patch_edk
_base_tools

# Build the BootShim: it carries the zImage magic the Samsung bootloader
# expects, cleans up CPU state, relocates the FD to UEFI_BASE and jumps to the
# GenFv reset vector at FD offset 0.
make -C BootShim UEFI_BASE=0x50000000 UEFI_SIZE=0x00100000

# edksetup.sh parses "$@" and rejects our own options.
set --
source "$PWD/../edk2/edksetup.sh"

# CLANGDWARF only defines DEPS_FLAGS for RISCV64.  Without it no ARM object
# depends on the headers it includes, so changing a shared struct rebuilt
# only some of a module's files and linked mismatched layouts.  Same flags
# GCC5/CLANGPDB use.
TOOLS_DEF="${CONF_PATH:-$PWD/../edk2/Conf}/tools_def.txt"
if ! grep -q '^\*_CLANGDWARF_ARM_DEPS_FLAGS' "$TOOLS_DEF"; then
    echo '*_CLANGDWARF_ARM_DEPS_FLAGS = -MMD -MF $@.deps' >> "$TOOLS_DEF"
    echo "Added CLANGDWARF ARM DEPS_FLAGS to $TOOLS_DEF"
fi

# shellcheck disable=SC2086
build -n "$(($(getconf _NPROCESSORS_ONLN) + 2))" \
      -a ARM -t CLANGDWARF \
      -p Exynos5420Pkg/Exynos5420Pkg.dsc \
      -b "${TARGET:-DEBUG}" \
      "${DSCOPTS[@]}"

FD="$WORKSPACE/Build/Exynos5420/${TARGET:-DEBUG}_CLANGDWARF/FV/EXYNOS5420_UEFI.fd"
if [ ! -f "$FD" ]; then
    echo "Build failed: $FD not found" >&2
    exit 1
fi

# bootpayload.bin = BootShim + FD, one contiguous image the bootloader loads
# as if it were a zImage.
PAYLOAD="$OUT_DIR/bootpayload.bin"
cat BootShim/BootShim.bin "$FD" > "$PAYLOAD"

# Package the payload into an Android boot.img.  abootimg -u preserves the
# template's original bootsize (~10 MB of padding), so instead: extract the
# template, shrink bootsize to fit the new contents, swap in our kernel plus a
# 1-byte stub ramdisk (we don't boot a ramdisk - it only bloats the image),
# and repack with -f.
#
# boot.img template: the working TWRP image for v1awifi (kept untouched; we
# update a copy).  Lives at the repo root, next to the kernel/u-boot trees.
TEMPLATE="$PWD/../../twrp-3.6.0_9-0-v1awifi.img"
if [ ! -f "$TEMPLATE" ]; then
    echo "boot.img template not found: $TEMPLATE" >&2
    exit 1
fi

BOOTIMG="$OUT_DIR/boot-v1awifi.img"
RAMDISK="$OUT_DIR/ramdisk"
WORK="$OUT_DIR/.bootimg-work"

dd if=/dev/zero of="$RAMDISK" bs=1 count=1 status=none

rm -rf "$WORK"; mkdir -p "$WORK"
cp "$TEMPLATE" "$WORK/boot.img"
( cd "$WORK" && abootimg -x boot.img >/dev/null )

# Compute the smallest bootsize that holds header + padded kernel + padded
# ramdisk.  abootimg lays out each component on a pagesize boundary and
# reserves one pagesize for the header.
PAGESIZE=$(( $(grep '^pagesize' "$WORK/bootimg.cfg" | cut -d= -f2 | tr -d ' ') ))
KSIZE=$(stat -c%s "$PAYLOAD")
RSIZE=$(stat -c%s "$RAMDISK")
align_up() { local n=$1 p=$2; echo $(( (n + p - 1) / p * p )); }
BOOTSIZE=$(( PAGESIZE + $(align_up "$KSIZE" "$PAGESIZE") + $(align_up "$RSIZE" "$PAGESIZE") ))

sed -i "s/^bootsize = .*/bootsize = $(printf '0x%x' "$BOOTSIZE")/" "$WORK/bootimg.cfg"
cp "$WORK/bootimg.cfg" "$OUT_DIR/bootimg.cfg"   # keep for reference

abootimg -u "$BOOTIMG" -f "$WORK/bootimg.cfg" -k "$PAYLOAD" -r "$RAMDISK" >/dev/null
rm -rf "$WORK"

echo
echo "Build OK"
echo "  Firmware device : $FD ($(stat -c%s "$FD") bytes)"
echo "  Boot payload    : $PAYLOAD ($(stat -c%s "$PAYLOAD") bytes)"
echo "  Boot image      : $BOOTIMG ($(stat -c%s "$BOOTIMG") bytes)"
echo
echo "Flash it to the BOOT partition (Heimdall):"
echo "  heimdall flash --BOOT $BOOTIMG"
echo
echo "Then boot the tablet.  Watch the panel:"
echo "  RED     -> shim running, FIMD1 framebuffer found"
echo "  GREEN   -> CPU cleanup done, FD about to relocate"
echo "  MAGENTA -> FD relocated to 0x50000000, about to jump"
echo "  BLUE    -> PrePi C entry reached"
echo "  text    -> console up (DEBUG log + UEFI Shell)"
echo "  splash  -> shim never ran or framebuffer discovery failed"
