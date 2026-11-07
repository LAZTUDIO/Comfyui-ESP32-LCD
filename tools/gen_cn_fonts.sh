#!/usr/bin/env bash
# Regenerate the LVGL CJK font subsets (SIL OFL 1.1).
# Uses Source Han Sans SC (same design as Noto Sans CJK SC) from the TUNA mirror;
# jsDelivr Noto is only a fallback because it throttles large downloads in CN.
# Requires Node.js (npx); lv_font_conv is fetched on demand.
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_DIR="tools/fonts"
FONT="$FONT_DIR/SourceHanSansSC-Regular.otf"
mkdir -p "$FONT_DIR" firmware/comfyui_panel

if [ ! -f "$FONT" ]; then
  echo "downloading Source Han Sans SC (OFL)..."
  curl -fL --connect-timeout 10 --max-time 300 -o "$FONT" \
    "https://mirrors.tuna.tsinghua.edu.cn/adobe-fonts/source-han-sans/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf"
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
echo "done. fonts are subsets of Source Han Sans SC (SIL OFL 1.1, see firmware/comfyui_panel/fonts/OFL.txt)"
