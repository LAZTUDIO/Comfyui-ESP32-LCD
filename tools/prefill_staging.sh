#!/bin/bash
# Prefill arduino-cli staging packages from Espressif github_assets CDN
set -u
STG="$HOME/Library/Arduino15/staging/packages"
mkdir -p "$STG"
BASE="https://dl.espressif.com/github_assets"
R="https://raw.githubusercontent.com"
files=(
"espressif/crosstool-NG/releases/download/esp-14.2.0_20260121/xtensa-esp-elf-14.2.0_20260121-aarch64-apple-darwin.tar.gz"
"espressif/crosstool-NG/releases/download/esp-14.2.0_20260121/riscv32-esp-elf-14.2.0_20260121-aarch64-apple-darwin.tar.gz"
"espressif/binutils-gdb/releases/download/esp-gdb-v17.1_20260402/xtensa-esp-elf-gdb-17.1_20260402-aarch64-apple-darwin24.5.tar.gz"
"espressif/binutils-gdb/releases/download/esp-gdb-v17.1_20260402/riscv32-esp-elf-gdb-17.1_20260402-aarch64-apple-darwin24.5.tar.gz"
"espressif/openocd-esp32/releases/download/v0.12.0-esp32-20260424/openocd-esp32-macos-arm64-0.12.0-esp32-20260424.tar.gz"
"espressif/esptool/releases/download/v5.3.1/esptool-v5.3.1-macos-arm64.tar.gz"
"earlephilhower/mklittlefs/releases/download/4.0.2/aarch64-apple-darwin-mklittlefs-db0513a.tar.gz"
"igrr/mkspiffs/releases/download/0.2.3/mkspiffs-0.2.3-arduino-esp32-osx.tar.gz"
"espressif/arduino-esp32/releases/download/3.3.12/esp32-core-3.3.12.zip"
"espressif/arduino-esp32/releases/download/3.3.12/esp32-libs-3.3.12.zip"
"espressif/arduino-esp32/releases/download/3.3.12/esp32s3-libs-3.3.12.zip"
"espressif/arduino-esp32/releases/download/3.3.12/esp32c3-libs-3.3.12.zip"
"espressif/arduino-esp32/releases/download/3.3.12/esp32c5-libs-3.3.12.zip"
"espressif/arduino-esp32/releases/download/3.3.12/esp32c6-libs-3.3.12.zip"
"espressif/arduino-esp32/releases/download/3.3.12/esp32h2-libs-3.3.12.zip"
"espressif/arduino-esp32/releases/download/3.3.12/esp32p4-libs-3.3.12.zip"
"espressif/arduino-esp32/releases/download/3.3.12/esp32p4_es-libs-3.3.12.zip"
"espressif/arduino-esp32/releases/download/3.3.12/esp32s2-libs-3.3.12.zip"
)
for f in "${files[@]}"; do
  fn=$(basename "$f")
  dest="$STG/$fn"
  echo ">>> $fn"
  curl -fsSL --retry 3 --retry-delay 2 -C - -o "$dest" "$BASE/$f" \
    || curl -fsSL --retry 2 -C - -o "$dest" "https://gh-proxy.com/https://github.com/$f" \
    || echo "!!! FAILED $fn"
done
echo "ALL DONE"
