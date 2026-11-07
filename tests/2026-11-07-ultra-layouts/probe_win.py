import json
import os
import sys
import time

sys.path.insert(0, os.path.expanduser(r"~\comfyui-assistant"))
import assistant as a  # noqa: E402

cr, g, pw = a.CpuRam(), a.Gpu(), a.Power()
cr.sample()
g.sample()
pw.sample()
time.sleep(1.2)
print(json.dumps({
    "detect": {"vendor": g.vendor, "name": g.name, "vram_total": g.vram_total,
               "amdsmi": bool(g.amdsmi), "cpu_count": __import__("os").cpu_count()},
    "cpu": cr.sample(),
    "gpu": g.sample(),
    "power": pw.sample(),
}, ensure_ascii=True, default=str))