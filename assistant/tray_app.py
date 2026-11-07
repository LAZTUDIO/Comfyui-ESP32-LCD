#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ComfyUI 看板助手 —— 桌面版（打包成单文件 exe 的入口）

相对 assistant.py 只改「怎么跑起来」，采集/接口/WebUI 全部复用 assistant.py：

  - 打开就有状态窗口（CPU/GPU/内存/显存/ComfyUI 状态），不再是"双击没反应"
  - 点 X 或「隐藏到托盘」→ 收到系统托盘，服务继续跑
  - 托盘菜单：显示窗口 / 打开看板 / 复制地址 / 退出
  - 单实例：重复双击只把已有窗口叫出来，不起第二份服务
  - 日志写 exe 同目录的 assistant.log（窗口版看不到 stdout，全靠它）

用法：python tray_app.py [--port 8189] [--bind 0.0.0.0]
"""
import argparse
import ctypes
import os
import subprocess
import sys
import threading
import time
import webbrowser

import assistant as A

LOG_PATH = os.path.join(A.APP_DIR, "assistant.log")
NO_WINDOW = 0x08000000 if A.IS_WIN else 0

BG = "#0b0e14"
BG2 = "#121620"
FG = "#e8e6e1"
GOLD = "#e8c476"
CYAN = "#45e0ff"
JADE = "#57e6a8"
RED = "#ff6b6b"
DIM = "#8b93a7"


# ---------------------------------------------------------------- 基础设施
def redirect_stdout():
    """--windowed 打包后 sys.stdout 是 None，任何 print 都会炸，先兜住并落盘。"""
    if sys.stdout is None or sys.stderr is None:
        f = open(LOG_PATH, "a", encoding="utf-8", buffering=1)
        if sys.stdout is None:
            sys.stdout = f
        if sys.stderr is None:
            sys.stderr = f


def log(msg):
    line = "%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line)
    except OSError:
        pass


def install_excepthook():
    """打包后看不到 stderr，未捕获异常一律落盘，否则就是"双击没反应"。"""
    import traceback

    def hook(tp, exc, tb):
        log("未捕获异常:\n" + "".join(traceback.format_exception(tp, exc, tb)))

    sys.excepthook = hook


def message_box(text, error=False):
    try:
        ctypes.windll.user32.MessageBoxW(None, text, "ComfyUI 看板助手",
                                         0x10 if error else 0x40)
    except Exception:
        pass


def probe_running(port, timeout=0.6):
    """端口上是否已经跑着本助手（认 /api/health 的 version 字段，不是随便什么程序）。

    不用命名互斥体：ctypes 读 GetLastError 不可靠，实测会误判"已有实例"。
    """
    import json
    import urllib.request
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/api/health" % port,
                                    timeout=timeout) as r:
            d = json.loads(r.read().decode("utf-8", "replace"))
        return bool(d.get("ok")) and "version" in d
    except Exception:
        return False


def copy_to_clipboard(text):
    try:
        subprocess.run(["clip"], input=text.encode("ascii", "ignore"), check=False,
                       creationflags=NO_WINDOW)
        return True
    except Exception:
        return False


def make_image():
    """托盘图标：深底圆角 + 金环 + 青点（与 WebUI 同色系）。"""
    from PIL import Image, ImageDraw
    s = 64
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2, 2, s - 3, s - 3], radius=15,
                        fill=(17, 21, 29, 255), outline=(122, 96, 46, 255), width=2)
    d.ellipse([15, 15, s - 16, s - 16], outline=(232, 196, 118, 255), width=3)
    d.ellipse([26, 26, s - 27, s - 27], fill=(69, 224, 255, 255))
    return img


# ---------------------------------------------------------------- 应用
class App:
    def __init__(self, cfg, port, bind):
        self.cfg = cfg
        self.port = port
        self.url = "http://127.0.0.1:%d/" % port
        self.bind = bind
        self.state = A.make_state(cfg, port)
        self.stop_evt = threading.Event()
        self.srv = A.make_server(self.state, port, bind)
        self.started = time.time()
        self.icon = None
        self.root = None
        self.labels = {}
        self._want_show = False
        self._want_quit = False
        self._notified = False

    # ---- 服务
    def start_service(self):
        threading.Thread(target=A.sampler_loop, args=(self.state, self.stop_evt),
                         daemon=True).start()
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def stop_service(self):
        self.stop_evt.set()
        try:
            self.srv.shutdown()
        except Exception:
            pass

    # ---- 窗口
    def build_window(self):
        import tkinter as tk
        r = tk.Tk()
        self.root = r
        r.title("灵枢观星台 · 助手 v%s" % A.VERSION)
        r.configure(bg=BG)
        r.geometry("470x406")
        r.resizable(False, False)

        def lbl(parent, text, fg=FG, size=10, bold=False, bg=BG):
            w = tk.Label(parent, text=text, fg=fg, bg=bg,
                         font=("Microsoft YaHei UI", size, "bold" if bold else "normal"),
                         anchor="w")
            return w

        head = tk.Frame(r, bg=BG2, height=64)
        head.pack(fill="x")
        lbl(head, "灵枢观星台", GOLD, 16, True, BG2).place(x=16, y=10)
        lbl(head, "COMFYUI PANEL ASSISTANT · v%s" % A.VERSION, DIM, 8, False, BG2).place(x=18, y=40)

        status = tk.Frame(r, bg=BG)
        status.pack(fill="x", padx=16, pady=(12, 4))
        lbl(status, "服务地址", DIM, 9).grid(row=0, column=0, sticky="w")
        self.addr = tk.Label(status, text=self.url, fg=CYAN, bg=BG, cursor="hand2",
                             font=("Consolas", 10, "underline"))
        self.addr.grid(row=1, column=0, sticky="w")
        self.addr.bind("<Button-1>", lambda e: self.open_webui())

        grid = tk.Frame(r, bg=BG2)
        grid.pack(fill="x", padx=16, pady=8)
        rows = [("cpu", "处理器 CPU"), ("gpu", "显卡 GPU"), ("ram", "内存 RAM"),
                ("vram", "显存 VRAM"), ("w", "功耗 POWER"), ("comfy", "COMFYUI")]
        for i, (key, name) in enumerate(rows):
            lbl(grid, name, DIM, 9, bg=BG2).grid(row=i, column=0, sticky="w", padx=(12, 0), pady=3)
            v = tk.Label(grid, text="--", fg=FG, bg=BG2, anchor="e",
                         font=("Consolas", 10))
            v.grid(row=i, column=1, sticky="e", padx=(0, 12), pady=3)
            self.labels[key] = v
            if key == "comfy":
                v.configure(font=("Microsoft YaHei UI", 9))
        grid.columnconfigure(0, weight=1)
        grid.columnconfigure(1, weight=0)

        foot = tk.Frame(r, bg=BG)
        foot.pack(fill="x", padx=16, pady=(2, 6))
        self.uptime = tk.Label(foot, text="已运行 00:00:00", fg=DIM, bg=BG,
                               font=("Consolas", 9))
        self.uptime.pack(side="left")

        btns = tk.Frame(r, bg=BG)
        btns.pack(fill="x", padx=16, pady=(0, 14), side="bottom")

        def mk(text, cmd, fg=GOLD, bg=BG2):
            b = tk.Label(btns, text=text, fg=fg, bg=bg, cursor="hand2",
                         font=("Microsoft YaHei UI", 9),
                         padx=12, pady=6)
            b.pack(side="left", padx=(0, 8))
            b.bind("<Button-1>", lambda e: cmd())
            b.bind("<Enter>", lambda e: b.configure(bg="#1d2434"))
            b.bind("<Leave>", lambda e: b.configure(bg=bg))
            return b

        mk("打开看板", self.open_webui, BG, GOLD)
        mk("复制地址", self.do_copy)
        mk("隐藏到托盘", self.hide_to_tray)
        mk("退出", self.quit, BG, "#f0a0a0")

        r.protocol("WM_DELETE_WINDOW", self.hide_to_tray)
        r.bind("<Unmap>", self._on_unmap)
        # 某些启动方式（explorer/任务计划）会把窗口以最小化状态创建，这里强制正常显示并置顶一次
        r.update_idletasks()
        r.state("normal")
        r.deiconify()
        r.lift()
        r.attributes("-topmost", True)
        r.after(1500, lambda: r.attributes("-topmost", False))
        r.after(500, self.tick)

    def _on_unmap(self, ev):
        # 最小化（iconify）也收进托盘，不留任务栏按钮
        if self.root is not None and ev.widget is self.root:
            try:
                if self.root.state() == "iconic":
                    self.root.after(50, self.hide_to_tray)
            except Exception:
                pass

    def tick(self):
        if self._want_quit:
            self.quit()
            return
        if self._want_show:
            self._want_show = False
            self.show_window()
        try:
            self.refresh()
        except Exception:
            pass
        if self.root is not None:
            self.root.after(1000, self.tick)

    def refresh(self):
        hw = getattr(self.state, "hw", None) or {}
        snap = self.state.comfy.snapshot()

        def pct(v):
            return "--" if v is None else "%d%%" % round(v)

        def col(v):
            if v is None:
                return DIM
            return JADE if v < 60 else (GOLD if v < 85 else RED)

        cpu = hw.get("cpu_pct")
        gpu = hw.get("gpu_util")
        ram = hw.get("ram_pct")
        vram = (round(100.0 * hw["gpu_vram_used"] / hw["gpu_vram_total"], 1)
                if hw.get("gpu_vram_used") is not None and hw.get("gpu_vram_total") else None)
        w = hw.get("power_total_w")
        gname = (self.state.gpu.name or "").replace("AMD Radeon ", "").replace("NVIDIA GeForce ", "")

        self.labels["cpu"].configure(text=pct(cpu), fg=col(cpu))
        self.labels["gpu"].configure(text="%s  %s" % (pct(gpu), gname) if gname else pct(gpu),
                                     fg=col(gpu))
        self.labels["ram"].configure(text=pct(ram), fg=col(ram))
        self.labels["vram"].configure(text=pct(vram), fg=col(vram))
        self.labels["w"].configure(text="--" if w is None else "%.0f W" % w,
                                   fg=FG if w is not None else DIM)
        st = snap.get("state", "stopped")
        zh = {"running": "运行中", "starting": "启动中", "stopped": "未运行", "external": "外部启动"}
        txt = zh.get(st, st)
        if snap.get("running"):
            txt += " · 任务 %d / 排队 %d" % (snap.get("running"), snap.get("pending", 0))
        self.labels["comfy"].configure(text=txt,
                                       fg=JADE if st == "running" else DIM)

        s = int(time.time() - self.started)
        self.uptime.configure(text="已运行 %02d:%02d:%02d" % (s // 3600, s // 60 % 60, s % 60))
        if self.icon is not None:
            try:
                self.icon.title = "灵枢观星台 %s\nCPU %s · GPU %s · 内存 %s\n%s" % (
                    A.VERSION, pct(cpu), pct(gpu), pct(ram), self.url)
            except Exception:
                pass

    # ---- 动作
    def open_webui(self):
        webbrowser.open(self.url)

    def do_copy(self):
        ok = copy_to_clipboard(self.url)
        if self.root is not None:
            self.uptime.configure(text="地址已复制" if ok else "复制失败（手动选上面那行）")

    def hide_to_tray(self):
        if self.root is None:
            return
        self.root.withdraw()
        log("窗口已收到托盘")
        if self.icon is not None and not self._notified:
            self._notified = True
            try:
                self.icon.notify("助手在托盘继续运行，双击图标可恢复窗口。", "灵枢观星台")
            except Exception:
                pass

    def show_window(self):
        if self.root is None:
            return
        try:
            self.root.deiconify()
            self.root.lift()
            self.root.focus_force()
        except Exception:
            pass

    def quit(self):
        self.stop_service()
        log("退出")
        try:
            if self.icon is not None:
                self.icon.stop()
        except Exception:
            pass
        if self.root is not None:
            try:
                self.root.destroy()
            except Exception:
                pass

    # ---- 托盘
    def build_tray(self):
        import pystray
        menu = pystray.Menu(
            pystray.MenuItem("显示窗口", self._menu_show, default=True),
            pystray.MenuItem("打开看板", self._menu_open),
            pystray.MenuItem("复制访问地址", self._menu_copy),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("退出", self._menu_quit),
        )
        self.icon = pystray.Icon("comfyui-panel-assistant", make_image(),
                                 "灵枢观星台 %s" % A.VERSION, menu)

    def _menu_show(self, icon=None, item=None):
        self._want_show = True

    def _menu_open(self, icon=None, item=None):
        self.open_webui()

    def _menu_copy(self, icon=None, item=None):
        copy_to_clipboard(self.url)

    def _menu_quit(self, icon=None, item=None):
        self._want_quit = True

    def run(self):
        self.start_service()
        log("run: 服务已启动")
        try:
            self.build_tray()
            threading.Thread(target=self.icon.run, daemon=True).start()
            log("run: 托盘已就绪")
        except Exception as e:
            log("托盘不可用：%r" % e)
        try:
            self.build_window()
            log("run: 窗口已建立，进入主循环")
            self.root.mainloop()
            log("run: 主循环结束")
        except Exception as e:
            import traceback
            log("窗口不可用（回退为仅托盘）：%r\n%s" % (e, traceback.format_exc()))
            message_box("状态窗口初始化失败，助手将继续在托盘运行。\n\n%s" % e)
            if self.icon is not None:
                self.icon.run()
        finally:
            self.stop_service()


def main():
    redirect_stdout()
    install_excepthook()
    log("=== 启动（frozen=%s, dir=%s）===" % (A.FROZEN, A.APP_DIR))
    try:
        return real_main()
    except Exception:
        import traceback
        tb = traceback.format_exc()
        log("启动失败:\n" + tb)
        message_box("助手启动失败，详情见 assistant.log：\n\n" + tb[-600:], error=True)
        return 1


def real_main():
    ap = argparse.ArgumentParser(description="ComfyUI 看板助手（桌面/托盘版）")
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    cfg = A.load_cfg()
    port = args.port or int(cfg.get("port", A.DEFAULT_PORT))
    url = "http://127.0.0.1:%d/" % port

    if probe_running(port):
        log("端口 %d 上已有助手在跑，只打开看板" % port)
        webbrowser.open(url)
        message_box("助手已经在运行了，已帮你打开看板。\n\n看不清窗口的话，"
                    "点任务栏右下角的托盘图标（可能在 ^ 折叠区里）。")
        return 0

    A.detect_session_warning()
    if A.SESSION_WARNING:
        log("警告：" + A.SESSION_WARNING)

    app = None
    try:
        app = App(cfg, port, args.bind)
    except OSError as e:
        log("端口 %d 无法监听：%r" % (port, e))
        message_box("端口 %d 无法监听。\n\n%s\n\n可能已有一份助手在跑（看右下角托盘），"
                    "或该端口被别的程序占用。" % (port, e), error=True)
        return 1

    log("启动 v%s，监听 %s:%d，ComfyUI=%s" % (A.VERSION, args.bind, port, cfg.get("comfy_url")))
    app.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
