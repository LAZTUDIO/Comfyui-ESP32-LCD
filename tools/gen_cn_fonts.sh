#!/usr/bin/env bash
# Regenerate the LVGL CJK font subsets from Noto Sans CJK SC (SIL OFL 1.1).
# Requires Node.js (npx); lv_font_conv is fetched on demand.
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_DIR="tools/fonts"
FONT="$FONT_DIR/NotoSansCJKsc-Regular.otf"
mkdir -p "$FONT_DIR" firmware/comfyui_panel

if [ ! -f "$FONT" ]; then
  echo "downloading Noto Sans CJK SC (OFL)..."
  curl -fL -o "$FONT" "https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
fi

SYMBOLS_FILE=$(mktemp)
python3 tools/extract_cjk.py firmware/comfyui_panel/comfyui_panel.ino > "$SYMBOLS_FILE"

for size in 12 14 16 20; do
  echo "generating cn_font_$size.c"
  npx --yes lv_font_conv@1.5.3 \
    --font "$FONT" --range 0x20-0x7E --symbols "$(cat "$SYMBOLS_FILE")" \
    --size "$size" --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
    -o "firmware/comfyui_panel/cn_font_$size.c"
done
rm -f "$SYMBOLS_FILE"
echo "done. fonts are subsets of Noto Sans CJK SC (SIL OFL 1.1, see firmware/comfyui_panel/fonts/OFL.txt)"
