#!/usr/bin/env bash
# Build the RTX 3090 Ti UEFI payloads in WSL/Linux with clang + GNU ld.
#
# The upstream build script needs Visual Studio's LLVM (clang.exe/lld-link.exe).
# This script produces the same two PE32+ EFI applications without Visual Studio:
#
#   unlock/build/BOOTX64.EFI     the unlocker (needs the five firmware assets)
#   build/TI_PROFILE.EFI         the read-only profiler (needs nothing)
#
# Usage:
#   bash unlock/build-wsl.sh            # build both
#   bash unlock/build-wsl.sh app        # unlocker only
#   bash unlock/build-wsl.sh profile    # profiler only
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${CC:-clang}"
LD="${LD:-ld}"
# Fall back to a versioned clang when the plain name is absent.
command -v "$CC" >/dev/null 2>&1 || CC=/usr/lib/llvm-18/bin/clang

WHAT="${1:-all}"

COMMON=(
  -target x86_64-pc-windows-msvc -std=c11 -ffreestanding -fno-builtin
  -fno-stack-protector -mno-red-zone -fshort-wchar -Os
  -Wall -Wextra -Werror -Wno-unused-parameter
  -I "$ROOT/deps/edk2/MdePkg/Include"
  -I "$ROOT/deps/edk2/MdePkg/Include/X64"
)

build_app() {
  local dir="$ROOT/unlock" out="$ROOT/unlock/build"
  local sources=(app app_io ownership format runtime sha256 checks port compat
                 vendor/unlock_610 vendor/prep_610 vendor/layout_610
                 vendor/vbios_610 vendor/gfw_eval)
  mkdir -p "$out"
  local objs=()
  for source in "${sources[@]}"; do
    local obj="$out/$(echo "$source" | tr '/' '_').obj"
    echo "CC  unlock/$source.c"
    "$CC" "${COMMON[@]}" -D__KERNEL__ -DGU_UEFI_PORT -I "$dir/compat" \
      -c "$dir/$source.c" -o "$obj"
    objs+=("$obj")
  done
  echo "LD  unlock/build/BOOTX64.EFI"
  "$LD" -mi386pep -nostdlib -e efi_main --subsystem 10 --image-base 0 \
    --enable-reloc-section -o "$out/BOOTX64.EFI" "${objs[@]}"
}

build_profile() {
  local out="$ROOT/build"
  mkdir -p "$out"
  local objs=()
  for source in probe_app probe_core unlock/sha256 unlock/runtime; do
    local obj="$out/$(echo "$source" | tr '/' '_').obj"
    echo "CC  $source.c"
    "$CC" "${COMMON[@]}" -c "$ROOT/$source.c" -o "$obj"
    objs+=("$obj")
  done
  echo "LD  build/TI_PROFILE.EFI"
  "$LD" -mi386pep -nostdlib -e efi_main --subsystem 10 --image-base 0 \
    --enable-reloc-section -o "$out/TI_PROFILE.EFI" "${objs[@]}"
}

case "$WHAT" in
  app) build_app ;;
  profile) build_profile ;;
  all) build_app; build_profile ;;
  *) echo "usage: $0 [all|app|profile]" >&2; exit 2 ;;
esac

for image in "$ROOT/unlock/build/BOOTX64.EFI" "$ROOT/build/TI_PROFILE.EFI"; do
  [ -f "$image" ] || continue
  echo "---- $image"
  file "$image"
  objdump -p "$image" | grep -E "Subsystem|ImageBase" || true
  objdump -h "$image" | grep -E "^ +[0-9]+ \.reloc" || echo "     (no .reloc section)"
done
