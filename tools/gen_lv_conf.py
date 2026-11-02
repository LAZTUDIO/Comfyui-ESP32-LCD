#!/usr/bin/env python3
"""Generate an lv_conf.h tuned for the ComfyUI panel from the lvgl 8.4 template.

Usage:
  python3 tools/gen_lv_conf.py \
      --template ~/Documents/Arduino/libraries/lvgl/lv_conf_template.h \
      --out      ~/Documents/Arduino/libraries/lvgl/lv_conf.h
"""
import argparse
import re
import pathlib

REPLACEMENTS = [
    (r"#if 0 /\*Set it to \"1\" to enable content\*/", '#if 1 /*Set it to "1" to enable content*/'),
    (r"#define LV_MEM_SIZE .*", "#define LV_MEM_SIZE (64 * 1024U)"),
    (r"#define LV_COLOR_16_SWAP .*", "#define LV_COLOR_16_SWAP 0"),
    (r"#define LV_TICK_CUSTOM .*", "#define LV_TICK_CUSTOM 1"),
    (r"#define LV_TICK_CUSTOM_INCLUDE .*", '#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"'),
    (r"#define LV_TICK_CUSTOM_SYS_TIME_EXPR .*", "#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())"),
    (r"#define LV_FONT_DEFAULT .*", "#define LV_FONT_DEFAULT &lv_font_montserrat_14"),
    (r"#define LV_USE_FLEX .*", "#define LV_USE_FLEX 1"),
    (r"#define LV_USE_GRID .*", "#define LV_USE_GRID 1"),
    (r"#define LV_USE_QRCODE .*", "#define LV_USE_QRCODE 1"),
    (r"#define LV_USE_LOG .*", "#define LV_USE_LOG 0"),
    (r"#define LV_USE_PERF_MONITOR .*", "#define LV_USE_PERF_MONITOR 0"),
    (r"#define LV_USE_MEM_MONITOR .*", "#define LV_USE_MEM_MONITOR 0"),
]
for size in (12, 16, 20, 24, 36):
    REPLACEMENTS.append((rf"#define LV_FONT_MONTSERRAT_{size} .*", f"#define LV_FONT_MONTSERRAT_{size} 1"))
for size in (18, 22, 26, 28, 30, 32, 34, 38, 40, 44, 48):
    REPLACEMENTS.append((rf"#define LV_FONT_MONTSERRAT_{size} .*", f"#define LV_FONT_MONTSERRAT_{size} 0"))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--template", default=pathlib.Path.home() / "Documents/Arduino/libraries/lvgl/lv_conf_template.h")
    ap.add_argument("--out", default=pathlib.Path.home() / "Documents/Arduino/libraries/lvgl/lv_conf.h")
    args = ap.parse_args()

    text = pathlib.Path(args.template).read_text()
    text = text.replace("lv_conf_template.h", "lv_conf.h")
    for pattern, repl in REPLACEMENTS:
        text, n = re.subn(pattern, repl, text)
        if n == 0:
            print(f"[warn] not found: {pattern}")
    pathlib.Path(args.out).write_text(text)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
