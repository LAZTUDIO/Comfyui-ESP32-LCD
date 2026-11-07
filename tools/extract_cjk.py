#!/usr/bin/env python3
"""Extract every CJK glyph used by the firmware so the LVGL font subset covers the UI."""
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "firmware/comfyui_panel/comfyui_panel.ino").read_text()
cjk = set(re.findall(r"[\u4e00-\u9fff]", src))
extra = set("，。…·（）→：、、「」《》？！；°")
ascii_range = "".join(chr(c) for c in range(0x20, 0x7F))
sys.stdout.write("".join(sorted(cjk | extra)) + ascii_range)
