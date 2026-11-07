#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ComfyUI 看板助手 (ComfyUI Panel Assistant)

运行在 ComfyUI 主机上，补齐 8188 接口缺失的硬件状态（CPU/GPU 占用、内存/显存、
功耗、温度、磁盘/网络），聚合 ComfyUI 运行状态，向 ESP32 看板提供 /api/panel，
并提供赛博修仙风 WebUI 全量看板与 ComfyUI 启停控制。

仅用 Python 标准库，Linux / Windows 通用。

用法:
    python3 assistant.py [--port 8189] [--bind 0.0.0.0]
                         [--comfy-url http://127.0.0.1:8188]
                         [--workdir /path/to/comfyui] [--start-cmd "..."]

重要：本助手必须运行在主机【本地桌面会话】里（本机终端 / 开机自启 / 桌面计划任务），
不要放在 SSH 会话里运行——由它拉起的 ComfyUI 才能拿到与本机手动启动一致的 GPU 性能。
"""
import argparse
import collections
import ctypes
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

IS_WIN = os.name == "nt"
HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = bool(getattr(sys, "frozen", False))
# 打包成 exe 后：只读资源解到 sys._MEIPASS（临时目录），
# 配置文件必须落在 exe 旁边，否则每次重启都丢配置。
APP_DIR = os.path.dirname(os.path.abspath(sys.executable)) if FROZEN else HERE
RES_DIR = getattr(sys, "_MEIPASS", HERE)
VERSION = "1.3.0"
DEFAULT_PORT = 8189
HISTORY_LEN = 300          # 5 分钟 @1s
LOG_LINES = 400

CFG_PATH = os.path.join(APP_DIR, "assistant_config.json")
# WebUI 优先用同目录的（方便改），没有就用打包进 exe 的那份
WEBUI_PATH = os.path.join(APP_DIR, "webui.html")
if not os.path.exists(WEBUI_PATH):
    WEBUI_PATH = os.path.join(RES_DIR, "webui.html")
DEFAULT_CFG = {
    "comfy_url": "http://127.0.0.1:8188",
    "workdir": "",
    "start_cmd": "",
    "stop_timeout": 15,
}


def load_cfg():
    cfg = dict(DEFAULT_CFG)
    try:
        with open(CFG_PATH, "r", encoding="utf-8") as f:
            cfg.update(json.load(f))
    except (OSError, ValueError):
        pass
    return cfg


def save_cfg(cfg):
    tmp = CFG_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    os.replace(tmp, CFG_PATH)


def now_ms():
    return int(time.time() * 1000)


def clamp_pct(v):
    if v is None:
        return None
    return max(0.0, min(100.0, float(v)))


def http_json(url, timeout=2.0):
    req = urllib.request.Request(url, headers={"User-Agent": "comfyui-panel-assistant/" + VERSION})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def run_cmd(args, timeout=6):
    try:
        p = subprocess.run(args, capture_output=True, timeout=timeout,
                           creationflags=0x08000000 if IS_WIN else 0)
        return p.stdout.decode("utf-8", "replace")
    except Exception:
        return ""


# ============================ CPU / RAM ============================
class CpuRam:
    def __init__(self):
        self.prev_total = None
        self.prev_idle = None
        self.prev_cores = None
        self.win_prev = None

    def sample(self):
        out = {"pct": None, "cores": [], "freq_mhz": None,
               "ram_total": None, "ram_used": None, "ram_pct": None}
        if IS_WIN:
            self._win_cpu(out)
            self._win_ram(out)
        else:
            self._linux_cpu(out)
            self._linux_ram(out)
            try:
                with open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") as f:
                    out["freq_mhz"] = round(int(f.read().strip()) / 1000)
            except (OSError, ValueError):
                pass
        return out

    def _linux_cpu(self, out):
        try:
            with open("/proc/stat") as f:
                lines = f.readlines()
        except OSError:
            return
        def parse(line):
            p = line.split()
            vals = [int(x) for x in p[1:9]]
            idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
            return sum(vals), idle
        total, idle = parse(lines[0])
        cores = [parse(l) for l in lines[1:] if l.startswith("cpu")]
        if self.prev_total is not None:
            dt = total - self.prev_total
            di = idle - self.prev_idle
            if dt > 0:
                out["pct"] = clamp_pct(100.0 * (dt - di) / dt)
        self.prev_total, self.prev_idle = total, idle
        if self.prev_cores and len(self.prev_cores) == len(cores):
            arr = []
            for (t, i), (pt, pi) in zip(cores, self.prev_cores):
                dt = t - pt
                arr.append(round(clamp_pct(100.0 * (dt - (i - pi)) / dt), 1) if dt > 0 else 0.0)
            out["cores"] = arr
        self.prev_cores = cores

    def _linux_ram(self, out):
        info = {}
        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    k, _, v = line.partition(":")
                    info[k.strip()] = int(v.split()[0]) * 1024
        except (OSError, ValueError, IndexError):
            return
        total = info.get("MemTotal", 0)
        avail = info.get("MemAvailable", info.get("MemFree", 0))
        if total:
            used = total - avail
            out.update(ram_total=total, ram_used=used,
                       ram_pct=round(100.0 * used / total, 1))

    def _win_cpu(self, out):
        class FILETIME(ctypes.Structure):
            _fields_ = [("lo", ctypes.c_uint32), ("hi", ctypes.c_uint32)]
        idle, kern, user = FILETIME(), FILETIME(), FILETIME()
        if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kern), ctypes.byref(user)):
            return
        def ft(x):
            return (x.hi << 32) | x.lo
        # kernel time includes idle
        total = ft(kern) + ft(user)
        idl = ft(idle)
        if self.win_prev:
            dt = total - self.win_prev[0]
            di = idl - self.win_prev[1]
            if dt > 0:
                out["pct"] = clamp_pct(100.0 * (dt - di) / dt)
        self.win_prev = (total, idl)
        n = os.environ.get("NUMBER_OF_PROCESSORS")
        if n:
            out["cores"] = []  # per-core on Windows needs PDH; total is enough for the panel

    def _win_ram(self, out):
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_uint32), ("dwMemoryLoad", ctypes.c_uint32),
                        ("ullTotalPhys", ctypes.c_uint64), ("ullAvailPhys", ctypes.c_uint64),
                        ("ullTotalPageFile", ctypes.c_uint64), ("ullAvailPageFile", ctypes.c_uint64),
                        ("ullTotalVirtual", ctypes.c_uint64), ("ullAvailVirtual", ctypes.c_uint64),
                        ("ullAvailExtendedVirtual", ctypes.c_uint64)]
        m = MEMORYSTATUSEX()
        m.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
            used = m.ullTotalPhys - m.ullAvailPhys
            out.update(ram_total=m.ullTotalPhys, ram_used=used,
                       ram_pct=round(100.0 * used / m.ullTotalPhys, 1) if m.ullTotalPhys else None)


# ============================ GPU ============================
def read_text(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


class _PdhFmtValue(ctypes.Structure):
    _fields_ = [("CStatus", ctypes.c_ulong), ("doubleValue", ctypes.c_double)]


class _PdhFmtItem(ctypes.Structure):
    _fields_ = [("szName", ctypes.c_wchar_p), ("FmtValue", _PdhFmtValue)]


class PdhEnergy:
    """Windows：用 PDH 直接读 '\\Energy Meter(*)\\power'（RAPL，单位 mW）。

    不 spawn PowerShell，采样约 1 ms。自动挑封装那一路（RAPL_Package0_PKG），
    找不到就退回任何名字里带 PKG/PACKAGE 的实例。
    注意：这是 Windows 自带的能量计量（Intel 平台普遍可用），AMD 桌面平台
    多数不暴露，此时 ok=False，CPU 功耗仍显示 --。
    """

    FMT_DOUBLE = 0x00000200
    MORE_DATA = 0x800007D2

    def __init__(self):
        self.ok = False
        self.pdh = None
        self.q = None
        self.c = None
        if not IS_WIN:
            return
        try:
            pdh = ctypes.WinDLL("pdh")
            pdh.PdhOpenQueryW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p,
                                          ctypes.POINTER(ctypes.c_void_p)]
            pdh.PdhAddCounterW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p,
                                           ctypes.c_void_p,
                                           ctypes.POINTER(ctypes.c_void_p)]
            pdh.PdhCollectQueryData.argtypes = [ctypes.c_void_p]
            pdh.PdhGetFormattedCounterArrayW.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong,
                ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                ctypes.c_void_p]
            pdh.PdhGetFormattedCounterArrayW.restype = ctypes.c_ulong
            q = ctypes.c_void_p()
            if pdh.PdhOpenQueryW(None, None, ctypes.byref(q)) != 0:
                return
            c = ctypes.c_void_p()
            if pdh.PdhAddCounterW(q, r"\Energy Meter(*)\power", None,
                                  ctypes.byref(c)) != 0:
                return
            pdh.PdhCollectQueryData(q)
            self.pdh, self.q, self.c = pdh, q, c
            self.ok = True
        except Exception:
            self.ok = False

    def sample_w(self):
        if not self.ok:
            return None
        try:
            if self.pdh.PdhCollectQueryData(self.q) != 0:
                return None
            size = ctypes.c_ulong(0)
            cnt = ctypes.c_ulong(0)
            rc = self.pdh.PdhGetFormattedCounterArrayW(
                self.c, self.FMT_DOUBLE, ctypes.byref(size), ctypes.byref(cnt), None)
            if rc != self.MORE_DATA or size.value == 0:
                return None
            buf = ctypes.create_string_buffer(size.value)
            rc = self.pdh.PdhGetFormattedCounterArrayW(
                self.c, self.FMT_DOUBLE, ctypes.byref(size), ctypes.byref(cnt), buf)
            if rc != 0:
                return None
            items = ctypes.cast(buf, ctypes.POINTER(_PdhFmtItem))
            fallback = None
            for i in range(cnt.value):
                it = items[i]
                if it.FmtValue.CStatus != 0 or not it.FmtValue.doubleValue:
                    continue
                nm = (it.szName or "").upper()
                if nm.endswith("_PKG"):
                    return round(it.FmtValue.doubleValue / 1000.0, 1)
                if "PKG" in nm or "PACKAGE" in nm:
                    fallback = it.FmtValue.doubleValue
            return round(fallback / 1000.0, 1) if fallback else None
        except Exception:
            return None


class AdlxGpu:
    """Windows / AMD：官方 ADLX SDK（pip 包 `amd-adlx`）读整板功耗、温度、频率、占用。

    为什么必须走 ADLX：ADL 的 Overdrive5/6 传感器在 RDNA3 + 新驱动上已停供
    （实测 ADL2_Overdrive6_CurrentPower_Get 返回 rc=0 但值恒 0，
      Temperature 返回 -8/-9）。HWiNFO / GPU-Z 在 A 卡上取传感器走的也是 ADLX。
    另外：GPUPower（芯片功耗）多数卡不支持，GPUTotalBoardPower（整板）才是可用的那个。
    """

    def __init__(self):
        self.ok = False
        self.helper = None
        self.sys = None
        self.perf = None
        self.gpu = None
        self.note = ""

    def open(self):
        try:
            from adlx import ADLX
        except Exception as e:
            self.note = "未安装 amd-adlx（%s）" % type(e).__name__
            return False
        try:
            h = ADLX.ADLXHelper()
            if h.Initialize() != ADLX.ADLX_RESULT.ADLX_OK:
                self.note = "ADLX 初始化失败"
                return False
            s = h.GetSystemServices()
            p = s.GetPerformanceMonitoringServices() if s is not None else None
            gpus = (s.GetGPUs() or []) if s is not None else []
            if p is None or not gpus:
                self.note = "ADLX 无 GPU/性能服务"
                return False
            self.helper, self.sys, self.perf, self.gpu = h, s, p, gpus[0]
            self.ok = True
            return True
        except Exception as e:
            self.note = "ADLX 异常：%r" % e
            self.ok = False
            return False

    def sample(self):
        out = {}
        if not self.ok:
            return out
        try:
            m = self.perf.GetCurrentGPUMetrics(self.gpu)
        except Exception:
            return out

        def g(name):
            try:
                f = getattr(m, name, None)
                return f() if f is not None else None
            except Exception:
                return None

        v = g("GPUUsage")
        if v is not None:
            out["util"] = clamp_pct(v)
        v = g("GPUTemperature")
        if v is not None:
            out["temp_c"] = round(float(v), 1)
        v = g("GPUHotspotTemperature")
        if v is not None:
            out["hotspot_c"] = round(float(v), 1)
        v = g("GPUClockSpeed")
        if v:
            out["clock_mhz"] = int(v)
        v = g("GPUFanSpeed")
        if v:
            out["fan_rpm"] = int(v)
        v = g("GPUTotalBoardPower")
        if v is None:
            v = g("GPUPower")
        if v is not None:
            out["power_w"] = round(float(v), 1)
        v = g("GPUVRAM")
        if v:
            out["vram_used_mb"] = int(v)
        return out


class Gpu:
    """NVIDIA: nvidia-smi; AMD Linux: sysfs + hwmon (+rocm-smi 兜底);
    AMD Windows: ADLX（首选，给整板功耗）+ PowerShell 兜底。"""

    def __init__(self):
        self.vendor = None        # nvidia / amd
        self.name = ""
        self.driver = ""
        self.vram_total = None
        self.smi = None
        self.drm = None           # /sys/class/drm/cardX/device
        self.apu = False
        self.hwmon = None
        self.hwmon_power = None
        self.amdsmi = None
        self.amdsmi_dev = None
        self.adlx = None
        self.ps_cache = {}
        self._detect()

    # ---------- detection ----------
    def _detect(self):
        smi = shutil.which("nvidia-smi")
        if smi:
            txt = run_cmd([smi, "--query-gpu=name,memory.total,driver_version",
                           "--format=csv,noheader,nounits"], timeout=8)
            if txt.strip():
                self.vendor, self.smi = "nvidia", smi
                parts = txt.strip().splitlines()[0].split(",")
                self.name = parts[0].strip()
                try:
                    self.vram_total = int(float(parts[1].strip()) * 1024 * 1024)
                except (ValueError, IndexError):
                    pass
                if len(parts) > 2:
                    self.driver = parts[2].strip()
                return
        if not IS_WIN:
            self._detect_amd_linux()
        else:
            self._detect_amd_win()

    def _detect_amd_linux(self):
        import glob
        for dev in sorted(glob.glob("/sys/class/drm/card[0-9]*/device")):
            if read_text(os.path.join(dev, "vendor")) != "0x1002":
                continue
            self.vendor, self.drm = "amd", dev
            self.name = self._amd_name_linux(dev)
            vt = read_text(os.path.join(dev, "mem_info_vram_total"))
            if vt:
                try:
                    self.vram_total = int(vt)
                except ValueError:
                    pass
            gtt = read_text(os.path.join(dev, "mem_info_gtt_total"))
            try:
                gtt_n = int(gtt) if gtt else 0
            except ValueError:
                gtt_n = 0
            # APU: the VRAM carveout is tiny (512MB); models live in the GTT pool
            if gtt_n > 4 * 1024**3 and (not self.vram_total or self.vram_total < 4 * 1024**3):
                self.apu = True
                self.vram_total = gtt_n
            for hw in glob.glob(os.path.join(dev, "hwmon", "hwmon*")):
                nm = read_text(os.path.join(hw, "name")) or ""
                if nm in ("amdgpu", "zenpower", "k10temp"):
                    self.hwmon = hw
                if os.path.exists(os.path.join(hw, "power1_average")) or \
                   os.path.exists(os.path.join(hw, "power1_input")):
                    self.hwmon_power = hw
            break
        if self.drm and not self.name:
            self.name = "AMD GPU"

    def _amd_name_linux(self, dev):
        ids = "/usr/share/misc/pci.ids"
        did = (read_text(os.path.join(dev, "device")) or "").replace("0x", "")
        if did and os.path.exists(ids):
            try:
                with open(ids, encoding="utf-8", errors="replace") as f:
                    in_amd = False
                    for line in f:
                        if line.startswith("1002"):
                            in_amd = True
                            continue
                        if in_amd:
                            if not line.startswith("\t"):
                                break
                            if line.startswith("\t\t"):
                                continue
                            if line[1:5].lower() == did.lower():
                                return "AMD " + line[5:].strip()
            except OSError:
                pass
        out = run_cmd(["rocm-smi", "--showproductname", "--csv"], timeout=6)
        for line in out.splitlines()[1:]:
            parts = [p.strip() for p in line.split(",")]
            if len(parts) >= 2 and parts[1]:
                return parts[1]
        try:
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith("model name"):
                        m = re.search(r"Radeon\s+([A-Za-z0-9 ]+?)\s*$", line)
                        if m and m.group(1).strip():
                            return "AMD Radeon " + m.group(1).strip()
                        break
        except OSError:
            pass
        return "AMD GPU"

    def _detect_amd_win(self):
        try:
            import amdsmi  # type: ignore
            amdsmi.amdsmi_init()
            handles = amdsmi.amdsmi_get_processor_handles()
            if handles:
                self.amdsmi, self.amdsmi_dev = amdsmi, handles[0]
                self.vendor = "amd"
                try:
                    self.name = amdsmi.amdsmi_get_gpu_product_name(handles[0])
                except Exception:
                    self.name = "AMD GPU"
                try:
                    mi = amdsmi.amdsmi_get_gpu_memory_info(handles[0], amdsmi.AmdSmiMemoryType.VRAM)
                    self.vram_total = mi.get("total")
                except Exception:
                    pass
                self._attach_win_sensors()
                return
        except Exception:
            pass
        out = self._powershell(
            "Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name", key="vc")
        for line in out.splitlines():
            line = line.strip()
            if line and ("AMD" in line or "Radeon" in line):
                self.vendor, self.name = "amd", line
                break
        if self.vendor == "amd":
            self._attach_win_sensors()

    def _attach_win_sensors(self):
        """Windows / A 卡：挂 ADLX。拿不到就静默降级到 PowerShell 兜底。"""
        if self.vendor != "amd":
            return
        a = AdlxGpu()
        if a.open():
            self.adlx = a

    def _powershell(self, cmd, key, ttl=4.0):
        now = time.time()
        hit = self.ps_cache.get(key)
        if hit and now - hit[0] < ttl:
            return hit[1]
        out = run_cmd(["powershell", "-NoProfile", "-Command", cmd], timeout=8)
        self.ps_cache[key] = (now, out)
        return out

    # ---------- sampling ----------
    def sample(self):
        out = {"util": None, "mem_util": None, "vram_used": None, "vram_total": self.vram_total,
               "power_w": None, "temp_c": None, "clock_mhz": None, "fan_pct": None,
               "hotspot_c": None, "fan_rpm": None, "power_covers_cpu": False}
        if self.vendor == "nvidia":
            self._sample_nvidia(out)
        elif self.vendor == "amd":
            if IS_WIN:
                self._sample_amd_win(out)
            else:
                self._sample_amd_linux(out)
        return out

    def _sample_nvidia(self, out):
        txt = run_cmd([self.smi,
                       "--query-gpu=utilization.gpu,utilization.memory,memory.used,memory.total,"
                       "power.draw,temperature.gpu,clocks.sm,fan.speed",
                       "--format=csv,noheader,nounits"], timeout=5)
        line = txt.strip().splitlines()[0] if txt.strip() else ""
        p = [x.strip() for x in line.split(",")]
        if len(p) < 8:
            return
        def num(i, scale=1.0):
            try:
                return float(p[i]) * scale
            except ValueError:
                return None
        out["util"] = clamp_pct(num(0))
        out["mem_util"] = clamp_pct(num(1))
        mu, mt = num(2), num(3)
        if mu is not None:
            out["vram_used"] = int(mu * 1024 * 1024)
        if mt:
            out["vram_total"] = int(mt * 1024 * 1024)
        out["power_w"] = num(4)
        out["temp_c"] = num(5)
        out["clock_mhz"] = num(6)
        out["fan_pct"] = clamp_pct(num(7))

    def _read_linux_thermal(self, out):
        """hwmon 里的温度与风扇。

        按 label 取数，不按节点序号硬猜：amdgpu 的 temp1=edge、temp2=junction
        （热点）。APU 上通常只有 edge 一路，热点与风扇缺失属平台限制，留 None。
        """
        import glob
        temps = {}
        for path in sorted(glob.glob(os.path.join(self.hwmon, "temp*_input"))):
            raw = read_text(path)
            if not raw:
                continue
            try:
                val = round(int(raw) / 1000.0, 1)
            except ValueError:
                continue
            key = path[:-len("_input")]
            label = (read_text(key + "_label") or "").strip().lower()
            temps[label or os.path.basename(key)] = val
        edge = next((temps[k] for k in ("edge", "gpu edge") if k in temps), None)
        hotspot = next((temps[k] for k in ("junction", "hotspot") if k in temps), None)
        if edge is None and temps:
            # 没有 label 的老内核：低温那路是 edge，高温那路是 junction
            vals = sorted(temps.values())
            edge = vals[0]
            hotspot = vals[-1] if len(vals) > 1 else None
        out["temp_c"] = edge
        if hotspot is not None and (edge is None or hotspot > edge):
            out["hotspot_c"] = hotspot
        fan = read_text(os.path.join(self.hwmon, "fan1_input"))
        if fan:
            try:
                out["fan_rpm"] = int(fan)
            except ValueError:
                pass
        pwm = read_text(os.path.join(self.hwmon, "pwm1"))
        if pwm:
            try:
                out["fan_pct"] = round(int(pwm) / 2.55, 1)
            except ValueError:
                pass

    def _sample_amd_linux(self, out):
        dev = self.drm
        if dev:
            busy = read_text(os.path.join(dev, "gpu_busy_percent"))
            if busy:
                try:
                    out["util"] = clamp_pct(float(busy))
                except ValueError:
                    pass
            if self.apu:
                vu = read_text(os.path.join(dev, "mem_info_gtt_used"))
                vt = read_text(os.path.join(dev, "mem_info_gtt_total"))
            else:
                vu = read_text(os.path.join(dev, "mem_info_vram_used"))
                vt = read_text(os.path.join(dev, "mem_info_vram_total"))
            try:
                if vu:
                    out["vram_used"] = int(vu)
                if vt:
                    out["vram_total"] = int(vt)
            except ValueError:
                pass
            if out["vram_used"] is not None and out["vram_total"]:
                out["mem_util"] = round(100.0 * out["vram_used"] / out["vram_total"], 1)
            sclk = read_text(os.path.join(dev, "pp_dpm_sclk"))
            if sclk:
                m = re.search(r"(\d+):\s*(\d+)Mhz\s*\*", sclk)
                if m:
                    out["clock_mhz"] = int(m.group(2))
        if self.hwmon:
            self._read_linux_thermal(out)
        if self.hwmon_power:
            for name in ("power1_average", "power1_input"):
                v = read_text(os.path.join(self.hwmon_power, name))
                if v:
                    try:
                        out["power_w"] = round(int(v) / 1e6, 1)
                        break
                    except ValueError:
                        pass
            # APU 的 PPT 是 SoC 整包功率（实测空载 14W、CPU 满载 157W），
            # 已经包含 CPU，不能再与 CPU 功耗相加，否则重复计数。
            label = (read_text(os.path.join(self.hwmon_power, "power1_label")) or "").strip().lower()
            if out["power_w"] is not None and self.apu and label == "ppt":
                out["power_covers_cpu"] = True
        if out["power_w"] is None and shutil.which("rocm-smi"):
            txt = run_cmd(["rocm-smi", "--showpower", "--csv"], timeout=6)
            for line in txt.splitlines()[1:]:
                parts = [p.strip() for p in line.split(",")]
                for p in parts[1:]:
                    m = re.match(r"([\d.]+)\s*W?", p)
                    if m:
                        try:
                            out["power_w"] = float(m.group(1))
                        except ValueError:
                            pass
                        break
                if out["power_w"] is not None:
                    break

    def _sample_amd_win(self, out):
        # 首选 ADLX：整板功耗 / 温度 / 频率 / 占用 / 显存，都是驱动直接给的
        if self.adlx is not None:
            d = self.adlx.sample()
            if d:
                if d.get("util") is not None:
                    out["util"] = d["util"]
                if d.get("temp_c") is not None:
                    out["temp_c"] = d["temp_c"]
                if d.get("hotspot_c") is not None:
                    out["hotspot_c"] = d["hotspot_c"]
                if d.get("clock_mhz"):
                    out["clock_mhz"] = d["clock_mhz"]
                if d.get("fan_rpm"):
                    out["fan_rpm"] = d["fan_rpm"]
                if d.get("power_w") is not None:
                    out["power_w"] = d["power_w"]
                if d.get("vram_used_mb"):
                    out["vram_used"] = d["vram_used_mb"] * 1024 * 1024
        if self.amdsmi:
            a, d = self.amdsmi, self.amdsmi_dev
            try:
                eng = a.amdsmi_get_gpu_activity(d)
                out["util"] = clamp_pct(eng.get("gfx_activity"))
            except Exception:
                pass
            try:
                mi = a.amdsmi_get_gpu_memory_info(d, a.AmdSmiMemoryType.VRAM)
                out["vram_used"], out["vram_total"] = mi.get("used"), mi.get("total")
                if mi.get("total"):
                    out["mem_util"] = round(100.0 * mi.get("used", 0) / mi["total"], 1)
            except Exception:
                pass
            try:
                out["power_w"] = a.amdsmi_get_power_info(d).get("average_socket_power")
            except Exception:
                pass
            try:
                out["temp_c"] = a.amdsmi_get_temp_metric(d, a.AmdSmiTemperatureType.EDGE,
                                                         a.AmdSmiTemperatureMetric.CURRENT)
            except Exception:
                pass
        if out["util"] is None:
            txt = self._powershell(
                "(Get-Counter '\\GPU Engine(*)\\Utilization Percentage').CounterSamples | "
                "Where-Object {$_.InstanceName -match 'engtype_3D'} | "
                "Measure-Object -Property CookedValue -Sum | Select-Object -ExpandProperty Sum",
                key="gpu3d", ttl=3.0)
            m = re.search(r"([\d.]+)", txt.strip())
            if m:
                try:
                    out["util"] = clamp_pct(float(m.group(1)))
                except ValueError:
                    pass
        # AdapterRAM is capped at 4GB; the registry holds the real dedicated size
        if out["vram_total"] is None:
            tot = self._powershell(
                "$k='HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Class\\"
                "{4d36e968-e325-11ce-bfc1-08002be10318}\\0000'; "
                "try { (Get-ItemProperty $k -Name 'HardwareInformation.qwMemorySize')"
                ".'HardwareInformation.qwMemorySize' } catch { '' }",
                key="vram_total", ttl=300.0)
            m = re.search(r"(\d+)", tot.strip())
            if m:
                self.vram_total = int(m.group(1))
                out["vram_total"] = self.vram_total
        if out["vram_used"] is None:
            txt = self._powershell(
                "$d=(Get-Counter '\\GPU Adapter Memory(*)\\Dedicated Usage')"
                ".CounterSamples | Measure-Object -Property CookedValue -Sum; "
                "[int64]$d.Sum",
                key="vram_used", ttl=2.0)
            m = re.search(r"(\d+)", txt.strip())
            if m:
                out["vram_used"] = int(m.group(1))
        if out["vram_used"] is not None and out["vram_total"]:
            out["mem_util"] = round(100.0 * out["vram_used"] / out["vram_total"], 1)

    def info(self):
        return {"vendor": self.vendor or "unknown", "name": self.name,
                "driver": self.driver, "vram_total": self.vram_total}


# ============================ power / temps ============================
class Power:
    """CPU 封装功耗：Windows 用 PDH 读 Energy Meter(RAPL)；
    Linux 走 RAPL energy 差分 / hwmon(k10temp, zenpower)。"""

    def __init__(self):
        self.rapl = None
        self.prev = None
        self.hwmon = None
        self.pdh = PdhEnergy() if IS_WIN else None
        if not IS_WIN:
            import glob
            # RAPL 封装功耗：不同内核/平台编号不同（intel-rapl:0、:1…），
            # 只认 name=package-N 的那一级，别把 core 子域当封装。
            for p in sorted(glob.glob("/sys/class/powercap/*")):
                if not os.path.exists(os.path.join(p, "energy_uj")):
                    continue
                nm = (read_text(os.path.join(p, "name")) or "").lower()
                if nm.startswith("package"):
                    self.rapl = p
                    break
            for hw in glob.glob("/sys/class/hwmon/hwmon*"):
                nm = read_text(os.path.join(hw, "name")) or ""
                if nm in ("k10temp", "zenpower", "coretemp"):
                    self.hwmon = hw

    def sample(self):
        out = {"cpu_w": None, "cpu_temp_c": None, "source": None}
        if self.rapl:
            e = read_text(os.path.join(self.rapl, "energy_uj"))
            mx = read_text(os.path.join(self.rapl, "max_energy_range_uj"))
            try:
                e = int(e)
                now = time.time()
                if self.prev:
                    de = e - self.prev[0]
                    if de < 0 and mx:
                        de += int(mx)
                    dt = now - self.prev[1]
                    if dt > 0 and de >= 0:
                        out["cpu_w"] = round(de / 1e6 / dt, 1)
                        out["source"] = "rapl"
                self.prev = (e, now)
            except (ValueError, TypeError):
                pass
        if out["cpu_w"] is None and self.pdh is not None:
            w = self.pdh.sample_w()
            if w is not None:
                out["cpu_w"] = w
                out["source"] = "rapl-pdh"
        if self.hwmon:
            if out["cpu_w"] is None:
                for label in ("power1_input", "power1_average"):
                    v = read_text(os.path.join(self.hwmon, label))
                    if v:
                        try:
                            out["cpu_w"] = round(int(v) / 1e6, 1)
                            out["source"] = "hwmon"
                            break
                        except ValueError:
                            pass
            v = read_text(os.path.join(self.hwmon, "temp1_input"))
            if v:
                try:
                    out["cpu_temp_c"] = round(int(v) / 1000.0, 1)
                except ValueError:
                    pass
        return out


# ============================ disk / net / system ============================
class DiskNet:
    def __init__(self, workdir):
        self.workdir = workdir or HERE
        self.prev_disk = None
        self.prev_net = None
        self.prev_t = None

    def sample(self):
        out = {"disk": {}, "net": {}, "uptime_s": None, "load": None, "procs": None}
        now = time.time()
        dt = now - self.prev_t if self.prev_t else None
        self.prev_t = now
        # os.statvfs 只在 POSIX 存在；Windows 的磁盘容量由 _win_io 用 GetDiskFreeSpaceExW 取
        if not IS_WIN:
            try:
                st = os.statvfs(self.workdir)
                total = st.f_blocks * st.f_frsize
                free = st.f_bavail * st.f_frsize
                out["disk"] = {"path": self.workdir, "total": total, "free": free,
                               "pct": round(100.0 * (total - free) / total, 1) if total else None}
            except (OSError, AttributeError):
                pass
        if IS_WIN:
            self._win_io(out, dt)
        else:
            self._linux_io(out, dt)
            up = read_text("/proc/uptime")
            if up:
                try:
                    out["uptime_s"] = int(float(up.split()[0]))
                except ValueError:
                    pass
            la = read_text("/proc/loadavg")
            if la:
                try:
                    out["load"] = [float(x) for x in la.split()[:3]]
                except ValueError:
                    pass
            try:
                out["procs"] = sum(1 for p in os.listdir("/proc") if p.isdigit())
            except OSError:
                pass
        return out

    def _linux_io(self, out, dt):
        try:
            with open("/proc/diskstats") as f:
                rd = wr = 0
                for line in f:
                    p = line.split()
                    if len(p) < 14:
                        continue
                    name = p[2]
                    if re.match(r"^(sd[a-z]|nvme\d+n\d|vd[a-z]|mmcblk\d+)$", name):
                        rd += int(p[5]) * 512
                        wr += int(p[9]) * 512
            if self.prev_disk and dt:
                out["disk"]["read_bps"] = max(0, int((rd - self.prev_disk[0]) / dt))
                out["disk"]["write_bps"] = max(0, int((wr - self.prev_disk[1]) / dt))
            self.prev_disk = (rd, wr)
        except (OSError, ValueError):
            pass
        try:
            with open("/proc/net/dev") as f:
                rx = tx = 0
                for line in f.readlines()[2:]:
                    iface, _, data = line.partition(":")
                    if iface.strip() == "lo":
                        continue
                    p = data.split()
                    rx += int(p[0])
                    tx += int(p[8])
            if self.prev_net and dt:
                out["net"] = {"rx_bps": max(0, int((rx - self.prev_net[0]) / dt)),
                              "tx_bps": max(0, int((tx - self.prev_net[1]) / dt))}
            self.prev_net = (rx, tx)
        except (OSError, ValueError, IndexError):
            pass

    def _win_io(self, out, dt):
        # 磁盘/网络速率在 Windows 上需要 PDH；这里给出磁盘容量与系统运行时长
        try:
            free = ctypes.c_uint64()
            total = ctypes.c_uint64()
            drive = os.path.splitdrive(os.path.abspath(self.workdir))[0] + "\\"
            if ctypes.windll.kernel32.GetDiskFreeSpaceExW(drive, ctypes.byref(free),
                                                          ctypes.byref(total), None):
                out["disk"] = {"path": self.workdir, "total": total.value, "free": free.value,
                               "pct": round(100.0 * (total.value - free.value) / total.value, 1)}
        except Exception:
            pass
        try:
            out["uptime_s"] = int(ctypes.windll.kernel32.GetTickCount64() / 1000)
        except Exception:
            pass


# ============================ ComfyUI ============================
def pid_listening_on_port(port):
    """找到监听指定端口的进程 pid（不依赖 psutil）。"""
    if IS_WIN:
        txt = run_cmd(["netstat", "-ano", "-p", "TCP"], timeout=6)
        for line in txt.splitlines():
            p = line.split()
            if len(p) >= 5 and p[0] == "TCP" and p[3] == "LISTENING":
                if p[1].rsplit(":", 1)[-1] == str(port):
                    try:
                        return int(p[4])
                    except ValueError:
                        pass
        return None
    inodes = set()
    for path in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            with open(path) as f:
                for line in f.readlines()[1:]:
                    p = line.split()
                    if len(p) > 9 and p[3] == "0A":
                        if int(p[1].split(":")[1], 16) == port:
                            inodes.add(p[9])
        except OSError:
            pass
    if not inodes:
        return None
    targets = {"socket:[%s]" % i for i in inodes}
    try:
        pids = [p for p in os.listdir("/proc") if p.isdigit()]
    except OSError:
        return None
    for pid in pids:
        fd_dir = "/proc/%s/fd" % pid
        try:
            for fd in os.listdir(fd_dir):
                try:
                    if os.readlink(os.path.join(fd_dir, fd)) in targets:
                        return int(pid)
                except OSError:
                    continue
        except OSError:
            continue
    return None


def pid_cmdline(pid):
    if IS_WIN:
        txt = run_cmd(["powershell", "-NoProfile", "-Command",
                       "(Get-CimInstance Win32_Process -Filter 'ProcessId=%d').CommandLine" % pid],
                      timeout=8)
        return txt.strip()
    try:
        with open("/proc/%d/cmdline" % pid, "rb") as f:
            return f.read().replace(b"\0", b" ").decode("utf-8", "replace").strip()
    except OSError:
        return ""


class ComfyUI:
    def __init__(self, cfg):
        self.cfg = cfg
        self.lock = threading.Lock()
        self.proc = None
        self.started_at = None
        self.logs = collections.deque(maxlen=LOG_LINES)
        self.log_ver = 0
        self.stats = None
        self.queue = None
        self.last_ok = 0
        self.job_start_ms = None
        self.prev_running = False
        self.ext_pid = None
        self.ext_cmd = ""
        self.last_ext_check = 0

    @property
    def url(self):
        return self.cfg["comfy_url"].rstrip("/")

    @property
    def port(self):
        try:
            return int(self.url.rsplit(":", 1)[-1].strip("/"))
        except ValueError:
            return 8188

    def log(self, msg):
        line = time.strftime("%H:%M:%S") + " " + msg
        self.logs.append(line)
        self.log_ver += 1

    def probe(self):
        stats = queue = None
        try:
            stats = http_json(self.url + "/system_stats", timeout=2.0)
        except Exception:
            pass
        try:
            queue = http_json(self.url + "/queue", timeout=2.0)
        except Exception:
            pass
        ok = stats is not None
        running = pending = 0
        if queue:
            running = len(queue.get("queue_running") or [])
            pending = len(queue.get("queue_pending") or [])
        with self.lock:
            if ok:
                self.last_ok = now_ms()
                self.stats = stats
            self.queue = queue
            if running > 0 and not self.prev_running:
                self.job_start_ms = now_ms()
            if running == 0:
                self.job_start_ms = None
            self.prev_running = running > 0
            self.running_n, self.pending_n = running, pending
            if not ok and now_ms() - self.last_ext_check > 10000:
                self.last_ext_check = now_ms()
                pid = pid_listening_on_port(self.port)
                if pid and pid != os.getpid():
                    cmd = pid_cmdline(pid)
                    if "comfy" in cmd.lower() or "main.py" in cmd.lower():
                        self.ext_pid, self.ext_cmd = pid, cmd
                    else:
                        self.ext_pid = None
                else:
                    self.ext_pid = None
            elif ok:
                self.ext_pid = None
        return ok

    def alive(self):
        with self.lock:
            if self.proc is not None:
                return self.proc.poll() is None
            return False

    def api_up(self):
        return now_ms() - self.last_ok < 8000

    def snapshot(self):
        with self.lock:
            stats = self.stats or {}
            sysinfo = stats.get("system", {})
            devices = stats.get("devices") or []
            dev = devices[0] if devices else {}
            managed = self.proc is not None and self.proc.poll() is None
            state = "running" if self.api_up() else ("starting" if managed else "stopped")
            if not self.api_up() and self.ext_pid:
                state = "external"
            pid = self.proc.pid if managed else self.ext_pid
            snap = {
                "state": state,
                "url": self.url,
                "port": self.port,
                "version": sysinfo.get("comfyui_version", ""),
                "python": sysinfo.get("python_version", "").split(" ")[0],
                "pytorch": sysinfo.get("pytorch_version", ""),
                "device_name": dev.get("name", ""),
                "vram_total": dev.get("vram_total"),
                "vram_free": dev.get("vram_free"),
                "ram_total": sysinfo.get("ram_total"),
                "ram_free": sysinfo.get("ram_free"),
                "running": getattr(self, "running_n", 0),
                "pending": getattr(self, "pending_n", 0),
                "job_start_ms": self.job_start_ms,
                "managed": managed,
                "pid": pid,
                "started_at": self.started_at if managed else None,
                "external_cmd": self.ext_cmd if self.ext_pid else "",
                "log_ver": self.log_ver,
            }
            return snap

    def tail_logs(self, n=80):
        return list(self.logs)[-n:]

    # ---------- start / stop ----------
    def start(self):
        with self.lock:
            if self.proc is not None and self.proc.poll() is None:
                return False, "ComfyUI 已由助手运行中"
        if self.api_up():
            return False, "端口上已有 ComfyUI 在运行（外部启动），请先停止或改用接管"
        cmd = self.cfg.get("start_cmd", "").strip()
        workdir = self.cfg.get("workdir", "").strip() or None
        if not cmd:
            return False, "未配置启动命令：请在设置里填写 start_cmd（与本机手动启动一致）"
        if workdir and not os.path.isdir(workdir):
            return False, "工作目录不存在: %s" % workdir
        try:
            if IS_WIN:
                if cmd.lower().endswith(".bat") or cmd.lower().endswith(".cmd"):
                    args = ["cmd.exe", "/c", cmd]
                else:
                    args = cmd
                p = subprocess.Popen(args, cwd=workdir, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            else:
                p = subprocess.Popen(["/bin/sh", "-c", cmd], cwd=workdir,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        except Exception as e:
            return False, "启动失败: %s" % e
        with self.lock:
            self.proc = p
            self.started_at = now_ms()
        self.log("启动 ComfyUI (pid=%d): %s" % (p.pid, cmd))
        threading.Thread(target=self._pump, args=(p,), daemon=True).start()
        return True, "已发起启动，等待 API 就绪…"

    def _pump(self, p):
        try:
            for raw in iter(p.stdout.readline, b""):
                line = raw.decode("utf-8", "replace").rstrip()
                if line:
                    self.log(line[-300:])
        except Exception:
            pass
        code = p.poll()
        self.log("ComfyUI 进程退出 (code=%s)" % code)
        with self.lock:
            if self.proc is p:
                self.proc = None

    def stop(self):
        timeout = float(self.cfg.get("stop_timeout", 15))
        with self.lock:
            p = self.proc
            ext = self.ext_pid
        if p is not None and p.poll() is None:
            self.log("停止 ComfyUI (pid=%d)…" % p.pid)
            self._kill_tree(p.pid, timeout)
            try:
                p.wait(timeout=3)
            except Exception:
                pass
            with self.lock:
                self.proc = None
            return True, "已停止"
        if ext:
            self.log("停止外部 ComfyUI (pid=%d)…" % ext)
            ok = self._kill_pid(ext, timeout)
            return ok, "已请求停止外部进程" if ok else "停止失败（权限不足？）"
        return False, "没有正在运行的 ComfyUI"

    def _kill_tree(self, pid, timeout):
        if IS_WIN:
            run_cmd(["taskkill", "/PID", str(pid), "/T", "/F"], timeout=10)
            return
        try:
            os.killpg(os.getpgid(pid), signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                return
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                os.killpg(os.getpgid(pid), 0)
                time.sleep(0.5)
            except OSError:
                return
        try:
            os.killpg(os.getpgid(pid), signal.SIGKILL)
        except OSError:
            pass

    def _kill_pid(self, pid, timeout):
        if IS_WIN:
            txt = run_cmd(["taskkill", "/PID", str(pid), "/T", "/F"], timeout=10)
            return "SUCCESS" in txt or "成功" in txt
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            return False
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                os.kill(pid, 0)
                time.sleep(0.5)
            except OSError:
                return True
        try:
            os.kill(pid, signal.SIGKILL)
            return True
        except OSError:
            return False


# ============================ state / sampler ============================
class State:
    def __init__(self, cfg):
        self.cfg = cfg
        self.cpu_ram = CpuRam()
        self.gpu = Gpu()
        self.power = Power()
        self.disknet = DiskNet(cfg.get("workdir"))
        self.comfy = ComfyUI(cfg)
        self.lock = threading.Lock()
        self.hw = {}
        self.history = collections.deque(maxlen=HISTORY_LEN)
        self.started = time.time()
        self.hostname = socket.gethostname()
        try:
            self.lan_ip = socket.gethostbyname(self.hostname)
        except OSError:
            self.lan_ip = ""

    def sample_once(self):
        cr = self.cpu_ram.sample()
        g = self.gpu.sample()
        pw = self.power.sample()
        dn = self.disknet.sample()
        gpu_w = g.get("power_w")
        cpu_w = pw.get("cpu_w")
        total_w = None
        if g.get("power_covers_cpu") and gpu_w is not None:
            # APU：hwmon 的 PPT 已是 SoC 整包（CPU+GPU），CPU 侧再读会重复
            total_w = gpu_w
            note = "APU 整包（PPT，含 CPU+GPU）"
        elif gpu_w is not None or cpu_w is not None:
            total_w = round((gpu_w or 0) + (cpu_w or 0), 1)
            note = "GPU+CPU 实测合计" if (gpu_w is not None and cpu_w is not None) \
                else ("仅 GPU（CPU 功耗不可读）" if gpu_w is not None else "仅 CPU（GPU 功耗不可读）")
        else:
            note = "不可读"
        hw = {
            "ts": now_ms(),
            "cpu_pct": round(cr["pct"], 1) if cr["pct"] is not None else None,
            "cpu_cores": cr["cores"],
            "cpu_freq_mhz": cr["freq_mhz"],
            "cpu_temp_c": pw["cpu_temp_c"],
            "cpu_power_w": cpu_w,
            "ram_total": cr["ram_total"], "ram_used": cr["ram_used"], "ram_pct": cr["ram_pct"],
            "gpu_util": round(g["util"], 1) if g["util"] is not None else None,
            "gpu_mem_util": g["mem_util"],
            "gpu_vram_used": g["vram_used"],
            "gpu_vram_total": g["vram_total"] or self.gpu.vram_total,
            "gpu_power_w": gpu_w,
            "gpu_temp_c": g["temp_c"],
            "gpu_clock_mhz": g["clock_mhz"],
            "gpu_fan_pct": g["fan_pct"],
            "gpu_hotspot_c": g.get("hotspot_c"),
            "gpu_fan_rpm": g.get("fan_rpm"),
            "power_total_w": total_w,
            "power_note": note,
            "disk": dn["disk"], "net": dn["net"],
            "uptime_s": dn["uptime_s"], "load": dn["load"], "procs": dn["procs"],
        }
        # ComfyUI 自报的显存/内存作为兜底（Windows A 卡无 sysfs 时）
        cs = self.comfy.snapshot()
        if hw["gpu_vram_used"] is None and cs.get("vram_total") and cs.get("vram_free") is not None:
            hw["gpu_vram_used"] = cs["vram_total"] - cs["vram_free"]
            hw["gpu_vram_total"] = cs["vram_total"]
        if hw["ram_total"] is None and cs.get("ram_total"):
            hw["ram_total"] = cs["ram_total"]
            if cs.get("ram_free") is not None:
                hw["ram_used"] = cs["ram_total"] - cs["ram_free"]
                hw["ram_pct"] = round(100.0 * hw["ram_used"] / hw["ram_total"], 1)
        with self.lock:
            self.hw = hw
            self.history.append({
                "ts": hw["ts"], "cpu": hw["cpu_pct"], "gpu": hw["gpu_util"],
                "ram": hw["ram_pct"],
                "vram": round(100.0 * hw["gpu_vram_used"] / hw["gpu_vram_total"], 1)
                        if hw["gpu_vram_used"] is not None and hw["gpu_vram_total"] else None,
                "w": total_w,
            })

    def full_status(self):
        with self.lock:
            hw = dict(self.hw)
        cs = self.comfy.snapshot()
        if not hw.get("gpu_vram_total") and cs.get("vram_total"):
            hw["gpu_vram_total"] = cs["vram_total"]
        vram_pct = None
        if hw.get("gpu_vram_used") is not None and hw.get("gpu_vram_total"):
            vram_pct = round(100.0 * hw["gpu_vram_used"] / hw["gpu_vram_total"], 1)
        return {
            "assistant": {"version": VERSION, "port": getattr(self, "listen_port", DEFAULT_PORT),
                          "uptime_s": int(time.time() - self.started),
                          "session_warning": SESSION_WARNING},
            "host": {"hostname": self.hostname, "ip": self.lan_ip,
                     "os": ("Windows build %d" % sys.getwindowsversion().build) if IS_WIN
                           else (os.uname().sysname + " " + os.uname().release),
                     "python": sys.version.split()[0], "arch": os.environ.get("PROCESSOR_ARCHITECTURE", "")},
            "cpu": {"pct": hw.get("cpu_pct"), "cores": hw.get("cpu_cores", []),
                    "freq_mhz": hw.get("cpu_freq_mhz"), "temp_c": hw.get("cpu_temp_c"),
                    "power_w": hw.get("cpu_power_w"), "count": os.cpu_count()},
            "ram": {"total": hw.get("ram_total"), "used": hw.get("ram_used"), "pct": hw.get("ram_pct")},
            "gpu": dict(self.gpu.info(), util=hw.get("gpu_util"), mem_util=hw.get("gpu_mem_util"),
                        vram_used=hw.get("gpu_vram_used"), vram_total=hw.get("gpu_vram_total"),
                        vram_pct=vram_pct, power_w=hw.get("gpu_power_w"), temp_c=hw.get("gpu_temp_c"),
                        clock_mhz=hw.get("gpu_clock_mhz"), fan_pct=hw.get("gpu_fan_pct"),
                        hotspot_c=hw.get("gpu_hotspot_c"), fan_rpm=hw.get("gpu_fan_rpm")),
            "power": {"total_w": hw.get("power_total_w"), "gpu_w": hw.get("gpu_power_w"),
                      "cpu_w": hw.get("cpu_power_w"), "note": hw.get("power_note")},
            "disk": hw.get("disk", {}), "net": hw.get("net", {}),
            "system": {"uptime_s": hw.get("uptime_s"), "load": hw.get("load"), "procs": hw.get("procs")},
            "comfyui": cs,
            "ts": now_ms(),
        }

    def panel(self):
        """ESP32 Ultra 模式用的精简接口（字段与固件解析一一对应）。"""
        st = self.full_status()
        g, c, cpu = st["gpu"], st["comfyui"], st["cpu"]
        vram_pct = g.get("vram_pct")
        if vram_pct is None and g.get("mem_util") is not None:
            vram_pct = g["mem_util"]
        return {
            "v": 1, "host": self.hostname, "ts": st["ts"],
            "comfy": {"up": c["state"] == "running", "running": c["running"],
                      "pending": c["pending"], "ver": c["version"],
                      "job_ms": (st["ts"] - c["job_start_ms"]) if c["job_start_ms"] else 0},
            "gpu": {"name": g.get("name") or c.get("device_name", ""),
                    "vendor": g.get("vendor", "unknown"), "util": g.get("util"),
                    "temp": g.get("temp_c"), "clk": g.get("clock_mhz")},
            "cpu": round(cpu["pct"]) if cpu.get("pct") is not None else None,
            "ram": round(st["ram"]["pct"]) if st["ram"].get("pct") is not None else None,
            "vram": round(vram_pct) if vram_pct is not None else None,
            "vram_used": g.get("vram_used"), "vram_total": g.get("vram_total"),
            "ram_used": st["ram"].get("used"), "ram_total": st["ram"].get("total"),
            "w": st["power"].get("total_w"),
            "cpu_temp": st["cpu"].get("temp_c"),
        }


SESSION_WARNING = ""


def detect_session_warning():
    global SESSION_WARNING
    if IS_WIN:
        # SSH 会话通常落在 session 0/1 之外；交互式桌面一般是 session 1+
        try:
            sid = ctypes.c_uint32()
            ctypes.windll.kernel32.ProcessIdToSessionId(os.getpid(), ctypes.byref(sid))
            if sid.value == 0:
                SESSION_WARNING = "当前运行在 Session 0（服务/SSH 会话）。请改为本机桌面会话运行，否则拉起的 ComfyUI 可能 GPU 性能受限。"
        except Exception:
            pass
    else:
        if os.environ.get("SSH_CONNECTION") or os.environ.get("SSH_TTY"):
            SESSION_WARNING = "当前运行在 SSH 会话中。请改为本机桌面终端或 systemd 用户服务运行，否则拉起的 ComfyUI 可能 GPU 性能受限。"


def sampler_loop(state, stop_evt):
    tick = 0
    while not stop_evt.is_set():
        t0 = time.time()
        try:
            state.comfy.probe()
            state.sample_once()
        except Exception as e:
            sys.stderr.write("sample error: %r\n" % e)
        tick += 1
        elapsed = time.time() - t0
        stop_evt.wait(max(0.2, 1.0 - elapsed))


# ============================ HTTP ============================
class Handler(BaseHTTPRequestHandler):
    state = None  # type: State
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj, ensure_ascii=False), "application/json; charset=utf-8")

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        st = self.state
        if path in ("/", "/index.html"):
            html_path = WEBUI_PATH
            try:
                with open(html_path, "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
            except OSError:
                self._send(500, "webui.html missing", "text/plain; charset=utf-8")
        elif path == "/api/status":
            self._json(st.full_status())
        elif path == "/api/panel":
            self._json(st.panel())
        elif path == "/api/history":
            with st.lock:
                hist = list(st.history)
            self._json({"points": hist})
        elif path == "/api/logs":
            self._json({"lines": st.comfy.tail_logs(120), "ver": st.comfy.log_ver})
        elif path == "/api/config":
            self._json(st.cfg)
        elif path == "/api/health":
            self._json({"ok": True, "version": VERSION})
        else:
            self._send(404, '{"error":"not found"}')

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        st = self.state
        try:
            n = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(n).decode("utf-8")) if n else {}
        except (ValueError, json.JSONDecodeError):
            body = {}
        if path == "/api/comfy/start":
            ok, msg = st.comfy.start()
            self._json({"ok": ok, "msg": msg})
        elif path == "/api/comfy/stop":
            ok, msg = st.comfy.stop()
            self._json({"ok": ok, "msg": msg})
        elif path == "/api/config":
            allowed = ("comfy_url", "workdir", "start_cmd", "stop_timeout")
            changed = {k: str(body[k]) for k in allowed if k in body}
            if "comfy_url" in changed and not re.match(r"^https?://[\w.\-]+(:\d+)?/?$", changed["comfy_url"]):
                self._json({"ok": False, "msg": "comfy_url 格式不合法"}, 400)
                return
            st.cfg.update(changed)
            save_cfg(st.cfg)
            st.disknet.workdir = st.cfg.get("workdir") or HERE
            self._json({"ok": True, "cfg": st.cfg})
        else:
            self._send(404, '{"error":"not found"}')


def make_state(cfg, port):
    """构造采集状态机（CLI 与托盘 exe 共用）。"""
    state = State(cfg)
    state.listen_port = port
    return state


class PanelServer(ThreadingHTTPServer):
    """客户端断开（刷新页面/浏览器取消）是常态，不要往日志里刷 traceback。"""
    daemon_threads = True

    def handle_error(self, request, client_address):
        import sys as _sys
        et = _sys.exc_info()[0]
        if et in (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
            return
        super().handle_error(request, client_address)


def make_server(state, port, bind="0.0.0.0"):
    """构造 HTTP 服务（CLI 与托盘 exe 共用）。"""
    Handler.state = state
    srv = PanelServer((bind, port), Handler)
    srv.daemon_threads = True
    return srv


def main():
    ap = argparse.ArgumentParser(description="ComfyUI 看板助手")
    ap.add_argument("--port", type=int, default=None, help="助手监听端口 A（默认 8189）")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--comfy-url", default=None)
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--start-cmd", default=None)
    args = ap.parse_args()

    cfg = load_cfg()
    if args.comfy_url:
        cfg["comfy_url"] = args.comfy_url
    if args.workdir:
        cfg["workdir"] = args.workdir
    if args.start_cmd:
        cfg["start_cmd"] = args.start_cmd
    port = args.port or int(cfg.get("port", DEFAULT_PORT))
    cfg["port"] = port

    detect_session_warning()
    state = make_state(cfg, port)

    stop_evt = threading.Event()
    th = threading.Thread(target=sampler_loop, args=(state, stop_evt), daemon=True)
    th.start()

    srv = make_server(state, port, args.bind)
    print("ComfyUI 看板助手 v%s" % VERSION)
    print("  WebUI / API : http://%s:%d/" % (args.bind, port))
    print("  ESP32 panel : http://<本机IP>:%d/api/panel" % port)
    print("  ComfyUI     : %s" % cfg["comfy_url"])
    print("  GPU         : %s (%s)" % (state.gpu.name or "未检测到", state.gpu.vendor or "?"))
    if SESSION_WARNING:
        print("  !! " + SESSION_WARNING)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop_evt.set()
        srv.shutdown()


if __name__ == "__main__":
    main()