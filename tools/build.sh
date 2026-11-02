#!/usr/bin/env bash
# Compile (and optionally upload) the panel firmware.
# Usage: tools/build.sh [--upload] [--port /dev/cu.usbmodem...]
set -euo pipefail
cd "$(dirname "$0")/.."

FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB"
SKETCH="firmware/comfyui_panel"

UPLOAD=0
PORT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --upload) UPLOAD=1; shift ;;
    --port) PORT="$2"; shift 2 ;;
    *) echo "unknown arg: $1"; exit 1 ;;
  esac
done

arduino-cli compile --clean -b "$FQBN" "$SKETCH"
if [ "$UPLOAD" = "1" ]; then
  if [ -z "$PORT" ]; then PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); fi
  [ -n "$PORT" ] || { echo "no serial port found"; exit 1; }
  arduino-cli upload -p "$PORT" -b "$FQBN" "$SKETCH"
fi
