#!/usr/bin/env python3
"""Mock ComfyUI (8188/8190/8191/8192) + assistant /api/panel (8189) for ESP32 Ultra layout screenshots.

Stdlib only. Serves realistic /system_stats, /queue, /history and a minimal
WebSocket that streams execution_start / executing / progress so the panel shows
live node + step state. The ultra endpoint rotates four hardware profiles so the
1/2/3/4-host cells render visibly different numbers (catches truncation/overlap).
"""
import base64
import hashlib
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAC_IP = "192.168.31.187"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# slot -> (comfy port, gpu name, vendor, running, pending, steps)
SLOTS = [
    (8188, "cuda:0 NVIDIA GeForce RTX 5070 : native", "nvidia", True, 1, 6),
    (8190, "cuda:0 AMD Radeon RX 7900 XT : native", "amd", False, 0, 20),
    (8191, "cuda:0 NVIDIA GeForce RTX 4090 : native", "nvidia", True, 2, 30),
    (8192, "cuda:0 AMD Radeon RX 9070 XT : native", "amd", False, 0, 12),
]
PORT2SLOT = {p: i for i, (p, *_rest) in enumerate(SLOTS)}

# Rotating ultra profiles: (host, gpu name, vendor, cpu, gpu, ram, vram, watts, cpu_temp, gpu_temp)
ULTRA = [
    ("halo-nuc", "AMD Radeon Graphics (Strix Halo)", "amd", 34, 78, 41, 63, 118.0, 62.0, 71.0),
    ("win-host", "NVIDIA GeForce RTX 5070", "nvidia", 22, 96, 55, 88, 305.0, 58.0, 77.0),
    ("halo-nuc", "AMD Radeon RX 7900 XT", "amd", 61, 44, 72, 35, 142.0, 70.0, 64.0),
    ("win-host", "NVIDIA GeForce RTX 4090", "nvidia", 88, 100, 91, 97, 412.0, 84.0, 83.0),
]
_ultra_n = 0
_ultra_lock = threading.Lock()


def now_ms():
    return int(time.time() * 1000)


def ws_frame(payload: bytes, opcode=1):
    head = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    return head + payload


def ws_send(sock, obj):
    try:
        sock.sendall(ws_frame(json.dumps(obj).encode()))
    except OSError:
        raise ConnectionError("ws peer gone")


def ws_pump(sock, slot):
    """Stream a believable job for running slots; idle slots just answer pings."""
    _port, gpu, vendor, running, pending, steps = SLOTS[slot]
    cid = f"mock-client-{slot}"
    nodes = [str(3 + i) for i in range(12)]
    try:
        if running:
            ws_send(sock, {"type": "execution_start", "data": {"prompt_id": f"p{slot}", "timestamp": now_ms()}})
            ws_send(sock, {"type": "execution_cached", "data": {"nodes": nodes[:2], "prompt_id": f"p{slot}"}})
        step = 0
        node_i = 2
        t0 = time.time()
        while True:
            # read + answer client ping/close so WebSocketsClient stays connected
            sock.settimeout(0.4)
            try:
                hdr = sock.recv(2)
                if len(hdr) == 2:
                    op = hdr[0] & 0x0F
                    ln = hdr[1] & 0x7F
                    if ln == 126:
                        ln = struct.unpack(">H", sock.recv(2))[0]
                    elif ln == 127:
                        ln = struct.unpack(">Q", sock.recv(8))[0]
                    if hdr[1] & 0x80:
                        sock.recv(4)
                    body = sock.recv(ln) if ln else b""
                    if op == 8:
                        return
                    if op == 9:
                        sock.sendall(ws_frame(body, opcode=10))
            except socket.timeout:
                pass
            if not running:
                time.sleep(0.3)
                continue
            elapsed = time.time() - t0
            # cycle a 6/20/30-step job roughly every steps*1.1s, then restart
            cycle = int(elapsed / (steps * 1.1 + 4))
            phase = elapsed - cycle * (steps * 1.1 + 4)
            if phase < 1.0:
                ws_send(sock, {"type": "execution_start", "data": {"prompt_id": f"p{slot}-{cycle}", "timestamp": now_ms()}})
                ws_send(sock, {"type": "execution_cached", "data": {"nodes": nodes[:2], "prompt_id": f"p{slot}-{cycle}"}})
                step = 0
                node_i = 2
            if phase > steps * 1.1 + 2.0:
                ws_send(sock, {"type": "execution_success", "data": {"prompt_id": f"p{slot}-{cycle}", "timestamp": now_ms()}})
                ws_send(sock, {"type": "executing", "data": {"node": None, "prompt_id": f"p{slot}-{cycle}"}})
                time.sleep(0.4)
                continue
            cur = nodes[min(node_i, len(nodes) - 1)]
            ws_send(sock, {"type": "executing", "data": {"node": cur, "prompt_id": f"p{slot}-{cycle}"}})
            step = min(steps, int(phase / 1.1) + 1)
            ws_send(sock, {"type": "progress", "data": {"value": step, "max": steps, "prompt_id": f"p{slot}-{cycle}", "node": cur}})
            if step >= steps:
                node_i = min(node_i + 1, len(nodes) - 1)
            time.sleep(1.0)
    except (ConnectionError, OSError):
        return


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    slot = 0
    role = "comfy"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if self.role == "ultra":
            if path == "/api/panel":
                global _ultra_n
                with _ultra_lock:
                    prof = ULTRA[_ultra_n % len(ULTRA)]
                    _ultra_n += 1
                host, gname, vendor, cpu, gpu, ram, vram, w, ct, gt = prof
                self._send(200, {
                    "v": 1, "host": host, "ts": now_ms(),
                    "comfy": {"up": True, "running": True, "pending": 1, "ver": "0.43.0", "job_ms": 42000},
                    "gpu": {"name": gname, "vendor": vendor, "util": gpu, "temp": gt, "clk": 2400},
                    "cpu": cpu, "ram": ram, "vram": vram,
                    "vram_used": int(vram * 0.24), "vram_total": 24,
                    "ram_used": int(ram * 0.64), "ram_total": 64,
                    "w": w, "cpu_temp": ct,
                })
            elif path == "/api/health":
                self._send(200, {"ok": True, "mock": True})
            else:
                self._send(404, {"error": "not found"})
            return

        slot = self.slot
        _port, gpu, vendor, running, pending, steps = SLOTS[slot]
        if path == "/system_stats":
            vt = 24 * 1024**3
            rt = 64 * 1024**3
            self._send(200, {
                "system": {
                    "os": "linux" if vendor == "amd" else "windows",
                    "ram_total": rt, "ram_free": int(rt * 0.45),
                    "comfyui_version": "0.43.0", "required_frontend_version": "1.20.7",
                    "python_version": "3.12.3",
                    "pytorch_version": "2.14.0+rocm10.1.0" if vendor == "amd" else "2.7.1+cu128",
                    "embedded_python": False, "argv": ["main.py"],
                },
                "devices": [{
                    "name": gpu, "type": "cuda", "index": 0,
                    "vram_total": vt, "vram_free": int(vt * (0.2 if running else 0.7)),
                    "torch_vram_total": vt, "torch_vram_free": int(vt * 0.3),
                }],
            })
        elif path == "/queue":
            run_items = []
            if running:
                prompt = {str(3 + i): {"class_type": n, "inputs": {}} for i, n in enumerate(
                    ["CheckpointLoaderSimple", "CLIPLoader", "VAELoader", "EmptyLatentImage",
                     "CLIPTextEncode", "CLIPTextEncode", "KSampler", "VAEDecode",
                     "SaveImage", "UpscaleModelLoader", "ImageUpscaleWithModel", "SaveImage"])}
                run_items = [[0, f"p{slot}", prompt,
                              {"client_id": f"mock-client-{slot}", "create_time": now_ms()},
                              ["3", "7"]]]
            pend_items = [[i + 1, f"q{slot}-{i}", {"3": {"class_type": "KSampler", "inputs": {}}},
                           {"client_id": f"mock-client-{slot}"}, []] for i in range(pending)]
            self._send(200, {"queue_running": run_items, "queue_pending": pend_items})
        elif path == "/history":
            self._send(200, {})
        elif path == "/ws":
            self.do_ws()
        elif path == "/":
            self._send(200, {"mock": True, "slot": slot}, "application/json")
        else:
            self._send(404, {"error": "not found"})

    def do_ws(self):
        key = self.headers.get("Sec-WebSocket-Key", "")
        if not key:
            self._send(400, {"error": "no ws key"})
            return
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        try:
            self.wfile.flush()
        except OSError:
            return
        sock = self.connection
        ws_send(sock, {"type": "status", "data": {"status": {"exec_info": {"queue_remaining": SLOTS[self.slot][4]}}}})
        ws_pump(sock, self.slot)


def make(role, port, slot=0):
    h = type("H", (Handler,), {"role": role, "slot": slot})
    srv = ThreadingHTTPServer(("0.0.0.0", port), h)
    srv.daemon_threads = True
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


def main():
    servers = []
    for i, (port, *_r) in enumerate(SLOTS):
        servers.append(make("comfy", port, i))
        print(f"comfy mock  slot{i} -> http://{MAC_IP}:{port}")
    servers.append(make("ultra", 8189))
    print(f"ultra mock         -> http://{MAC_IP}:8189/api/panel")
    print("Ctrl-C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        for s in servers:
            s.shutdown()


if __name__ == "__main__":
    main()