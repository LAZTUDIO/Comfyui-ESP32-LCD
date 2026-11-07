# 开发接力手册（HANDOFF）

> 最后更新：2026-11-07。换机/换 agent 先读本文件 + 根目录 `AGENTS.md`，再看 `tests/2026-11-07-ultra-layouts/RECORD.md`。
> 本文件只记录**代码之外**的上下文、决策、验证结果与坑；代码本身以仓库为准。

---

## 0. 一句话状态

ESP32 面板的 **Ultra 模式**（从各主机的「看板助手」拉 CPU/GPU/内存/显存/功耗）已完成、已烧录、已用 mock + halo 真机逐布局验证；助手已部署到 **halo（systemd 常驻）** 与 **win（只放文件，未启动）**。下一步是让用户在 win 桌面手动启动助手，然后在面板上开 Ultra 做双机联调。

---

## 1. 系统由三部分组成

```
ComfyUI (每台主机 :8188)  ──HTTP/WS──►  ESP32-S3 面板 (:8080 调试)
        ▲                                   ▲
        │ 启停/日志                          │ GET /api/panel (端口 A, 默认 8189)
        │                                   │
看板助手 assistant.py (每台主机 :8189) ──────┘
        └─ WebUI: http://<host>:8189/  （赛博修仙风格「灵枢观星台」）
```

- **面板**：Waveshare ESP32-S3-Touch-LCD-3.5，480×320 横屏，ST7796S + FT6336U + TCA9554，MAC `20:6e:f1:9a:ec:10`，局域网 IP `192.168.31.50`，主机名 `esp32s3-9aec10`。
- **主机**：
  - halo：`192.168.31.95`，Ubuntu（内核 7.0.0-38，Python 3.14），Strix Halo（Ryzen AI MAX+ 395 / Radeon 8060S，GTT ≈104 GiB）。
  - win：`192.168.31.195`，Windows，RX 7900 XT 20GB，ComfyUI 在 `D:\jz-comfyui`。
  - 登录用户名与密码由用户另行提供，**不写入仓库**；文档中的 `<user>` / `%USERPROFILE%` 为占位符。
  - 第三台：`192.168.31.42`（RTX 5070 Laptop），只作为面板的第 3 台监控对象。
- **密码策略**：仓库/本文档**不写任何用户名或密码**。SSH 凭据由用户口头提供。

---

## 2. 当前进度

### 已完成并验证
1. **助手 v1.0.0**（`assistant/`）：Python 标准库单文件，Linux/Windows 通用；硬件采集、WebUI、ComfyUI 启停、配置持久化。
2. **ESP32 Ultra**：设置页 Ultra 开关 + 端口 A；总览页按 1/2/3/4 台重排；数据与助手 `/api/panel` 对齐。
3. **四条 Ultra 布局上机截图核验**：1 台（MONITOR 左卡）、2 台（两列）、3 台（窄三列）、4 台（2×2 紧凑）、助手离线降级、Ultra 关闭回归。见 `tests/2026-11-07-ultra-layouts/shots/`。
4. **字体子集修复**：新增 Ultra 中文串缺字导致方框，已重生成 4 个字号并重烧。
5. **halo 部署**：`~/comfyui-assistant/`，systemd `comfyui-assistant.service` 已 `enable --now`，active；真机数值正确。
6. **win 部署（未启动）**：`%USERPROFILE%\comfyui-assistant\`，文件 + 配置齐全；只读探针验证采集器可用。

### 未完成 / 待用户
- **win 上手动启动助手**：在桌面会话双击 `%USERPROFILE%\comfyui-assistant\start.bat`（**不要在 SSH 里常驻**）。启动后才能在面板看到 win 的硬件数据。
- 面板上开 Ultra（端口 A=8189）做 halo+win 双机最终联调。当前面板已恢复原配置（195/95/42，Ultra 关）。
- win 的 ComfyUI 启停按钮首次实点验证（需要助手先跑起来）。
- 既有未提交改动尚未 commit/push（见 §10）。

---

## 3. 文件地图

| 路径 | 作用 |
|---|---|
| `firmware/comfyui_panel/comfyui_panel.ino` | 主固件（约 3200 行）：WiFi 配网、多主机轮询、WS、四种布局、10 主题、中英、Ultra、调试 HTTP |
| `firmware/comfyui_panel/cn_font_{12,14,16,20}.c` | 由 `.ino` 抽字生成的 LVGL 中文字体子集（Source Han Sans SC，OFL） |
| `firmware/comfyui_panel/gpu_logos.c/.h` | 16×16 NVIDIA/AMD 手绘标记（`tools/gen_gpu_logos.py` 生成） |
| `firmware/lv_conf.h` | LVGL 配置模板（已是 `#if 1`，直接复制用） |
| `assistant/assistant.py` | 看板助手（标准库；采集 + WebUI + 启停 + API） |
| `assistant/webui.html` | 赛博修仙单页 WebUI（原生 JS + canvas） |
| `assistant/start.sh` / `start.bat` | 本机启动脚本（默认端口 8189） |
| `tools/build.sh` / `tools/build.bat` | Mac / Windows 编译烧录 |
| `tools/extract_cjk.py` | 抽出 `.ino` 里全部汉字 + 标点 |
| `tools/gen_cn_fonts.sh` | 用 `lv_font_conv` 生成 4 个字号（需 Node/npx） |
| `tools/raw_to_png.py` | `/shot.raw` → PNG |
| `tools/patches/arduino_gfx_1.5.5_core3.patch` | Arduino_GFX 1.5.5 配 core≥3.3.6 的 SPI 补丁 |
| `tests/2026-11-07-ultra-layouts/` | 本轮测试：`mock_panel.py`、`probe_win.py`、`RECORD.md`、`shots/` |
| `docs/HANDOFF.md` | 本文件 |

---

## 4. 硬性约束（新 agent 必须遵守）

1. **win（192.168.31.195）：任何情况下都不要远程启动它的 ComfyUI 或其他常驻服务。** 用户手动启动。SSH 只用于查库存、传文件、改配置、只读探针。
2. **不要猜测/写入密码。** 需要时向用户要；不要写进仓库、argv 或提交。
3. **有两块同型号板子**：烧录前必须 `read-mac` 确认是 `20:6e:f1:9a:ec:10`。
4. **改面板的服务器/Ultra 配置后要恢复**（除非用户明确要保留）。本轮已恢复为 195/95/42、Ultra 关、端口 8189。
5. **不要回退这些既有设计**：按主机数自动选布局（不恢复手动布局开关）、只显示真实数据、Ultra 关闭时基础布局必须无回归。
6. **助手必须在桌面会话运行**（尤其 Windows）：SSH/服务会话里启动 ComfyUI 会导致 GPU 性能受限——这正是用户最初的痛点。Linux（halo）用 systemd 可以；Windows 不行。

---

## 5. ESP32 固件

### 5.1 构建与烧录

**Mac（已验证）**
```bash
tools/build.sh                          # 只编译
tools/build.sh --upload                 # 自动找 /dev/cu.usbmodem*
tools/build.sh --upload --port /dev/cu.usbmodemXXXX
```

**Windows（新增，未在 win 实机跑过）**
```bat
tools\build.bat                         :: 只编译
tools\build.bat --upload --port COM5
```

**工具链**
- `arduino-cli` + core `esp32:esp32@3.3.12`
- 库：`lvgl@8.4.0`、`SensorLib@0.5.1`、`TCA9554@0.1.3`、`ArduinoJson@7.4.3`、`WebSockets@2.7.2`、`JPEGDEC@1.8.5`
- **Arduino_GFX 1.5.5**（微雪 Demo 包内版本；上游 1.6.9 的 ST7796 初始化在这块屏上只亮背光）。配 core≥3.3.6 需打补丁：
  ```bash
  cd <Arduino>/libraries/Arduino_GFX
  patch -p0 < <repo>/tools/patches/arduino_gfx_1.5.5_core3.patch   # Windows 用 Git Bash
  ```
- **LVGL 配置**：把 `firmware/lv_conf.h` 复制到 `lvgl` 库目录（仓库里已是 `#if 1`，直接覆盖即可）。
- **FQBN**：
  `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB`
- 编译占用参考：Flash 50%（~1.59 MB / 3 MB），RAM 57%（~187 KB / 320 KB）。

### 5.2 架构要点

- **双核 FreeRTOS**：core 0 worker 任务跑 HTTP/WS 轮询（`fetchVitals/fetchQueue/fetchHistory/fetchUltra`），core 1 跑 LVGL；网络/解码不阻塞 UI。
- **`netMux` 互斥**保护 `NetData net[]`；UI 侧通过 `pullHost()` 把 `net[]` 快照到 `HostView dview[]`，用 `lastVer`/`dviewVer` 版本号做变更检测，避免每帧重绘。
- `HostView` 是 UI 只读快照（包含 `Ultra ultra`）。
- 轮询节奏（每台）：vitals 3/4/5 s（1-2/3/4 台），queue 2/3/3.5 s，history 20 s，**Ultra 固定 2.5 s**。
- 离线判定 `offlineMs()`：≤2 台 10 s、3 台 15 s、4 台 20 s；超过即清空 vitals/queue 并显示 OFFLINE。
- WS：从 `/queue` 运行项的 `extra_data.client_id` 取 id，用同一 id 重连 `/ws?clientId=...`（被动监控也能拿到节点/步数）。空闲时 ComfyUI 会周期性关被动 WS，日志成对出现断开/连接，属正常。

### 5.3 Ultra 数据结构与解析

```cpp
struct Ultra {           // 百分比未知=-1；watts<0 表示读不到
  bool ok, everSeen;
  uint32_t lastSeen;
  int cpu=-1, gpu=-1, ram=-1, vram=-1;
  float watts=-1, gpuTemp=-1, cpuTemp=-1;
  char gpuName[48]; uint8_t gpuVendor;   // 0 unknown / 1 NVIDIA / 2 AMD
};
```

- `fetchUltra()`：`GET http://<host>:<ultraPort>/api/panel`；用 `jnumN(s, "cpu")` 解析（key **不带引号**，内部会补引号；键缺失或 `null` 返回默认值）。
- `gpu` 子对象用 `strstr(s,"\"gpu\"")` 定位后再取 `util/temp/name/vendor`。
- 超时 1.2 s 连接 / 3 s 读；失败且超过 `offlineMs()` 才置 `ok=false` 并清空数值（UI 显示 `--` 与「功耗：助手离线」）。
- Ultra 与 8188 的 `serverOk` **互相独立**：ComfyUI 挂了但助手在，硬件数值照常显示；反之亦然。

### 5.4 布局规则

| 主机数 | 页面 | 总览单元格 |
|---|---|---|
| 1 | `MONITOR` + `OUTPUTS` | 左侧卡片：CPU/GPU/RAM/VRAM 四条（30px）+ 功耗 + GPU 标记/型号 + 版本；右侧运行日志 |
| 2 | 总览/日志/产物 | 2 列 × 1 行，高格：状态/时长、节点/步数、进度条、GPU 行、**4 条指标**、功耗 |
| 3 | 总览/日志/产物 | 3 列 × 1 行，窄格：同上但字号 16、标题截断 |
| 4 | 总览/日志/产物 | 2×2 紧凑格（`ih<200`）：状态/时长、进度条、**2×2 百分比瓦片**、功耗 |

- 4 台紧凑格**故意省略 GPU 型号与节点文字**（空间不足）；进度仍以条形显示。
- 单元格字段按剩余高度**逐行 append**（`buildUltraCell`），所以四种布局共用一套绘制代码。
- 指标颜色 `pctColor()`：<60 绿（主题的 OK 色）、<85 黄、≥85 红，未知灰。
- 后端数据入口：`buildDashboard()` 里 `pg==0` 时按 `useGrid()` 选择 `buildUltraCell` / `buildMonitor`(单机) 或 `buildDashCell`；刷新用 `refreshUltraDash()` / `refreshMonitor()`。

### 5.5 设置与 NVS

- NVS namespace `cfg`。
- 键：`ssid`、`psk`、`h0..h3`（`ip:port`）、`theme`、`lang`、`ultra`(bool)、`ultraPort`(int, 默认 8189)。
- 服务器行每个 38px；Ultra 行在 y=182；端口 A 用虚拟键盘编辑器（`editTarget==10`）。
- 服务器保存后**不需要重启**，立即重建布局；主题/语言切换会重启（约 3 秒）。

### 5.6 调试 HTTP（:`8080`）

- `GET /shot.raw` → 480×320 RGB565 LE（`tools/raw_to_png.py`）
- `GET /stats` → `{wifi,server,ws,running,pending,hosts,on,cols,rows,page,seen,up,rst,heap,ui_max_ms}`
- `GET /debug/show?s=dash|setup|servers|theme`
- `GET /debug/page?i=N`
- `GET /debug/edit?i=N`
- `GET /debug/scan`
- `GET /debug/thumb`
- `GET /debug/theme?i=N`、`/debug/lang?i=1`（会重启）
- `GET /debug/ultra?i=0|1&p=PORT`（存 NVS + 重建）
- `GET /debug/host?i=N&h=ip[:port]`、`/debug/addhost?h=`、`/debug/delhost?i=N`

> 注意：`/debug/host` 与 `/debug/ultra` 会改运行时并保存，但**不刷新设置页里已生成的旧标签**（服务器行/Ultra 开关仍显示开机值）。真实按键/键盘流程会刷新；仅调试时可见。

### 5.7 中文字体（改中文后必做）

```bash
# Mac/Linux
tools/gen_cn_fonts.sh
```

```powershell
# Windows（无 npx 时先装 Node LTS）
python tools/extract_cjk.py firmware/comfyui_panel/comfyui_panel.ino > $env:TEMP\cjk.txt
npx --yes lv_font_conv@1.5.3 --font tools/fonts/SourceHanSansSC-Regular.otf `
  --range 0x20-0x7E --symbols (Get-Content -Raw $env:TEMP\cjk.txt) `
  --size 12 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h -o firmware/comfyui_panel/cn_font_12.c
# 14 / 16 / 20 同理
```

- `extract_cjk.py` 扫 `.ino` 全量汉字 + `，。…·（）→：、。「」《》？！；`。
- **新增任何中文 UI 串都要重跑**，否则显示方框。
- 本机没有系统 Node 时的应急办法（Mac 实测）：用 Qoder 内置 Electron 当 node
  `ELECTRON_RUN_AS_NODE=1 "/Applications/Qoder CN.app/Contents/MacOS/Qoder CN" <lv_font_conv.js>`，`npx` 缓存通常在 `~/.npm/_npx/*/node_modules/lv_font_conv`。

---

## 6. 看板助手（`assistant/assistant.py`）

### 6.1 运行

```bash
python3 assistant.py --port 8189          # 或 ./start.sh / start.bat
# 可选：--comfy-url --workdir --start-cmd --bind
```
- 纯标准库，无需第三方包，无需 venv。
- 配置文件：同目录 `assistant_config.json`（已 gitignore）。
- Windows 必须**桌面会话**运行；启动时会检测 SSH/会话 0 并打印警告。

### 6.2 HTTP API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | WebUI（`webui.html`） |
| GET | `/api/panel` | **ESP32 Ultra 用精简接口**（见下） |
| GET | `/api/status` | 全量状态（WebUI 用） |
| GET | `/api/history` | 趋势点 |
| GET | `/api/logs` | ComfyUI 日志尾部 + 版本号 |
| GET | `/api/config` | 当前配置 |
| GET | `/api/health` | `{ok,version}` |
| POST | `/api/config` | 改 `comfy_url/workdir/start_cmd/stop_timeout`（`comfy_url` 有正则校验） |
| POST | `/api/comfy/start` | 启动 ComfyUI（等价于本机手动启动） |
| POST | `/api/comfy/stop` | 停止（Linux 进程组 SIGTERM→SIGKILL；Windows `taskkill /T /F`） |

**`/api/panel` 契约（固件按此解析，改字段必须两边同步）**
```json
{
  "v": 1,
  "host": "halo-host",
  "ts": 1794032318163,
  "comfy": {"up": true, "running": 0, "pending": 0, "ver": "0.43.0", "job_ms": 0},
  "gpu": {"name": "AMD Radeon 8060S", "vendor": "amd", "util": 0.0, "temp": 38.0, "clk": 600},
  "cpu": 0, "ram": 63, "vram": 50,
  "vram_used": 56405245952, "vram_total": 111669149696,
  "ram_used": 83078275072, "ram_total": 131738574848,
  "w": 15.1, "cpu_temp": 42.2
}
```
- 百分比未知时为 `null`；功耗未知时为 `null`。固件把 `null`/缺失当 -1 并显示 `--`。
- `gpu.vendor` 只能是 `nvidia` / `amd` / `unknown`。

### 6.3 配置默认值

```json
{"comfy_url": "http://127.0.0.1:8188", "workdir": "", "start_cmd": "", "stop_timeout": 15}
```
`port` 由命令行/`ASSISTANT_PORT` 提供，默认 8189。

### 6.4 采集器与平台限制

| 指标 | Linux | Windows |
|---|---|---|
| CPU% | `/proc/stat` 差分 | `GetSystemTimes` 差分 |
| RAM | `/proc/meminfo`（MemAvailable） | `GlobalMemoryStatusEx` |
| NVIDIA | `nvidia-smi` | `nvidia-smi` |
| AMD 利用率 | `/sys/class/drm/cardX/device/gpu_busy_percent` | amdsmi（若有）→ 否则 `\GPU Engine(*)\Utilization Percentage`（3D 引擎求和） |
| AMD 显存 | `mem_info_vram_*`；**APU 自动改用 `mem_info_gtt_*`** | amdsmi（若有）→ 否则注册表 `HardwareInformation.qwMemorySize` + `\GPU Adapter Memory(*)\Dedicated Usage` |
| GPU 功耗/温度 | hwmon `power1_average` / `temp1_input`，rocm-smi 兜底 | 仅 amdsmi；没有则 `--` |
| CPU 功耗 | Intel RAPL / hwmon（k10temp/zenpower） | 无（`--`） |

**已知限制**
- APU（Strix Halo）的 512MB 是核显 carveout，不是可用显存；代码用 GTT（≈104GiB）替代。
- win 无 amdsmi 时 GPU 功耗/温度/CPU 功耗都拿不到；`AdapterRAM` 会被截断到 4GB，所以必须用注册表。
- `power_total_w` = GPU + CPU（能读到就相加）；`power_note` 会说明「仅 GPU / 仅 CPU / 不可读」。面板标签因此从「整机」改成「功耗」。

### 6.5 启停语义（关键需求）

- 用户痛点：SSH 远程拉起 ComfyUI 后 GPU 性能受限。
- 因此助手**必须**在桌面会话运行，`start_cmd` 必须与本机手动启动**完全一致**。
- Linux：`/bin/sh -c cmd` + `start_new_session=True`（新进程组），停止时整组 SIGTERM→SIGKILL。
- Windows：`.bat` → `cmd.exe /c` + `CREATE_NEW_PROCESS_GROUP`；停止用 `taskkill /T /F`。
- 日志泵把 stdout/stderr 收进 `/api/logs`。

### 6.6 部署现状

**halo（已运行）**
```
~/comfyui-assistant/{assistant.py,webui.html,start.sh,assistant_config.json}
/etc/systemd/system/comfyui-assistant.service
  User=<user>
  WorkingDirectory=/home/<user>/comfyui-assistant
  ExecStart=/usr/bin/python3 .../assistant.py --port 8189
  已 enable --now，active
```
配置：
```json
{"comfy_url":"http://127.0.0.1:8188","workdir":"/home/<user>/ComfyUI",
 "start_cmd":"/home/<user>/ai/venv_comfyui/bin/python /home/<user>/ComfyUI/main.py --highvram --use-ck-attention --listen 0.0.0.0 --port 8188",
 "stop_timeout":20,"port":8189}
```
更新代码后：`sudo systemctl restart comfyui-assistant`。

**win（只部署，未启动）**
```
%USERPROFILE%\comfyui-assistant\{assistant.py,webui.html,start.bat,assistant_config.json}
```
配置：
```json
{"comfy_url":"http://127.0.0.1:8188","workdir":"D:\\jz-comfyui\\ComfyUI",
 "start_cmd":"D:\\jz-comfyui\\Start_ComfyUI_H3.bat","stop_timeout":20,"port":8189}
```
- `Start_ComfyUI_H3.bat` 是用户本机启动脚本（设置 `HIP_VISIBLE_DEVICES=0`、`HYPERCORE=on`、线程数=4、`PYTHONUTF8`，然后跑 `python_embeded\python.exe -u main.py --disable-xformers --use-ck-attention --async-offload 2 --vram-headroom 1 --cache-none --listen 0.0.0.0 --port 8188`）。
- **未启动**：按约束由用户在桌面双击 `start.bat`。

### 6.7 WebUI（`webui.html`）

- 单文件、原生 JS、无构建；标题「灵枢观星台」，赛博修仙配色（金 `#e8c476`、青 `#45e0ff`、玉 `#57e6a8`、紫 `#b48cff`）。
- 卡片：灵枢 CPU、玄光 GPU、丹炉 ComfyUI（启炉/闭关＝start/stop）、气海 RAM、神识 VRAM、灵力 Power、寿元 System、观星 Trends（canvas 折线）、藏经阁 Disk、传音符 Network、炉边语录 Logs、阵法设置 Config。
- 轮询：`/api/status` 每 1 s，`/api/logs` 每 2 s；配置卡走 `POST /api/config`。
- 境界徽章按 GPU 负载映射：凡尘/辟谷/炼气/筑基/金丹/元婴/化神。
- 已在 Mac 浏览器验证渲染与交互、无 JS 控制台报错。

---

## 7. 联调与测试方法

### 7.1 mock 数据源（无需真实主机）

```bash
python3 tests/2026-11-07-ultra-layouts/mock_panel.py
# 在 Mac 192.168.31.187 上监听：
#  8188/8190/8191/8192 = ComfyUI mock（含最小 WebSocket，推 execution_start/executing/progress）
#  8189               = 助手 mock /api/panel（轮换 4 组硬件画像）
```
然后对面板：
```bash
B=http://192.168.31.50:8080
curl "$B/debug/host?i=0&h=192.168.31.187:8188"
curl "$B/debug/addhost?h=192.168.31.187:8190"     # 2 台
curl "$B/debug/addhost?h=192.168.31.187:8191"     # 3 台
curl "$B/debug/addhost?h=192.168.31.187:8192"     # 4 台
curl "$B/debug/ultra?i=1&p=8189"
curl "$B/debug/show?s=dash"; curl "$B/debug/page?i=0"
curl "$B/shot.raw" -o /tmp/u.raw
python3 tools/raw_to_png.py /tmp/u.raw /tmp/u.png
```

### 7.2 像素级检查清单

- 单元格之间无重叠；标题/右值不碰撞。
- 4 条指标的分类标签、百分比、条形三行对齐。
- 中文无方框；英文模式无中文。
- 助手离线：四项 `--` + 「功耗：助手离线」，但 ComfyUI 状态/进度仍实时。
- Ultra 关闭：原基础布局（VRAM/RAM 分数、GPU、CUDA/ROCm）无回归。
- 切换主机数后布局立即重建，无需重启。

### 7.3 Windows 采集器只读探针

`tests/2026-11-07-ultra-layouts/probe_win.py`（导入模块采样即退出，不起服务）：
```bat
python %USERPROFILE%\comfyui-assistant\probe_win.py
```
预期：`gpu.name` 正确、`vram_used/vram_total` 有值、`mem_util` 合理；`power_w/temp_c` 为 null 属正常。

---

## 8. 已知问题 / 待办

1. **win 助手未启动**（等用户）。
2. **win ComfyUI 启停按钮未实点验证**（等助手启动）。
3. 面板当前 Ultra 关闭、三台真机配置；要联调需手动开 Ultra。
4. 设置页的服务器行/Ultra 开关在**调试端点**修改后不刷新（真实操作会刷新）；可顺手修但非必须。
5. halo 的 `cpu_power_w` 读不到（无 RAPL/zenpower），功耗只有 GPU/封装值；标签已改为「功耗」。
6. `docs/screenshots/*` 是 11-06 的旧图，README 里还没有 Ultra 截图；需要的话补。
7. 未提交的改动很多（见 §10），需要用户决定 commit/push。

---

## 9. 踩坑百科（都踩过）

1. **CJK 字体是子集**：`.ino` 里新增汉字必须重跑字体生成，否则方框。
2. **`jnumN(s,key)` 的 key 不带引号**（内部会补），否则永远取不到。
3. **`/system_stats` 的 `devices` 是数组不是对象**。
4. **WS 事件只发给提交者**：必须跟随 `/queue` 里的 `client_id`。
5. **空闲 ComfyUI 会周期性关闭被动 WS**，日志里成对断开/连接，正常。
6. **APU 显存看 GTT**，`mem_info_vram_total` 只有 512MB carveout。
7. **Windows `AdapterRAM` 截断 4GB**；用注册表 `qwMemorySize` + `GPU Adapter Memory` 计数器。
8. **Win 无 amdsmi 时拿不到 GPU 功耗/温度**，别硬编一个假值。
9. **Arduino_GFX 必须 1.5.5 + 补丁**；上游 1.6.9 只亮背光。
10. **两块同型号板**：烧录前 `read-mac`。
11. **不要用 `pkill -f <pattern>` 驱动远程 SSH**：会匹配到自己的 `bash -c` 包装进程，把会话杀掉。按 PID 杀。
12. **Windows cmd 不用 `;` 分隔命令**，用 `&`。
13. **sudo 提示可能是中文「密码：」**，expect 匹配要同时覆盖 `password` 和 `密码`；或用 `sudo -p 'password:'`。
14. **Windows SSH 默认 shell 是 cmd**；复杂 PowerShell 用 `-EncodedCommand`（base64 UTF-16LE）。
15. **面板改造后必须恢复用户配置**（服务器列表/Ultra 开关都存 NVS）。
16. **`/debug/ultra` 会 saveCfg**，调试完记得还原。
17. **系统无 Node 时**可用 Qoder 内置 Electron 当 node（macOS）；Windows 装 Node LTS 即可。
18. **exFAT 上 AppleDouble `._*` 文件**会混进目录列表，`.gitignore` 已忽略。

---

## 10. 换机 Checklist（Mac → Windows）

### 10.1 代码怎么带过去

当前工作树在 SanDisk（exFAT），`.git` 是一个指针文件：
```
gitdir: /Users/laz/.gitrepos/comfyui-panel
```
它指向 Mac 用户目录里的真实仓库——**插到 Windows 上会失效**。二选一：

- **A（推荐，保留未提交改动）**：把整个工作树（含未提交文件）复制到 win，同时把 Mac 上 `/Users/laz/.gitrepos/comfyui-panel` 整个目录也复制到 win，然后把工作树里的 `.git` 指针改成新路径。
- **B（干净历史）**：在 win `git clone git@github.com:LAZTUDIO/Comfyui-ESP32-LCD.git`，再把未提交的 `assistant/`、`tests/`、改过的 `firmware/`、`tools/`、`README.md`、`.gitignore` 覆盖过去。
- 注意：`tools/fonts/*.otf` 和 `tools/samples/` 被 gitignore，字体源文件不会随 clone 来；`gen_cn_fonts.sh` 会自动从 TUNA 镜像下载。

### 10.2 Windows 开发环境

1. 安装 `arduino-cli`（加入 PATH）。
2. `arduino-cli core install esp32:esp32@3.3.12`
3. `arduino-cli lib install lvgl@8.4.0 SensorLib@0.5.1 TCA9554@0.1.3 ArduinoJson@7.4.3 WebSockets@2.7.2 JPEGDEC@1.8.5`
4. Arduino_GFX 1.5.5：从微雪 Demo 包取出 `GFX_Library_for_Arduino`，放到
   `%USERPROFILE%\Documents\Arduino\libraries\Arduino_GFX`，用 **Git Bash** 打补丁：
   `cd /c/Users/<user>/Documents/Arduino/libraries/Arduino_GFX && patch -p0 < <repo>/tools/patches/arduino_gfx_1.5.5_core3.patch`
5. 把 `firmware/lv_conf.h` 复制到
   `%USERPROFILE%\Documents\Arduino\libraries\lvgl\lv_conf.h`。
6. 插板子 → 设备管理器看 COM 口 → `tools\build.bat --upload --port COMx`。
7. 先 `arduino-cli board list` 确认是 ESP32-S3 的板子；同型号两块，**烧前用 esptool 读 MAC**：
   `esptool --port COMx read-mac`（路径在 `%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\...`）。
8. Python 3.10+ 用于助手；Node LTS 用于字体生成（可选）。
9. 网络：确保能访问面板 `192.168.31.50:8080`、halo `:8189`、win 本机 `:8189`。

### 10.3 到 win 后先做的验证（5 分钟）

```bat
:: 1) 面板在线
curl http://192.168.31.50:8080/stats
:: 2) 编译
tools\build.bat
:: 3) 助手导入无语法错
python -c "import ast,os;ast.parse(open(os.path.expanduser('~/comfyui-assistant/assistant.py'),encoding='utf-8').read());print('ok')"
:: 4) 若已在桌面启动助手：
curl http://127.0.0.1:8189/api/health
curl http://127.0.0.1:8189/api/panel
```

---

## 11. 新 agent 接手后的第一轮检查

1. 读本文件、`AGENTS.md`、`tests/2026-11-07-ultra-layouts/RECORD.md`。
2. `git status --short` 看未提交改动，**不要随手 reset/checkout**。
3. 确认面板在线：`curl http://192.168.31.50:8080/stats`（应 `hosts:3,on:3,wifi:2`）。
4. 确认 halo 助手：`curl http://192.168.31.95:8189/api/panel`（应 `gpu.name=AMD Radeon 8060S`）。
5. 确认 win 助手：`curl http://192.168.31.195:8189/api/health`；若失败，提醒用户桌面双击 `start.bat`，**不要自己远程启动**。
6. 要改固件 UI：先拍 `/shot.raw` 基线，改完逐布局截图对比。
7. 要改中文文案：改完立刻重跑字体子集，再编译。
8. 要改 `/api/panel`：同步改固件 `fetchUltra()`，两端一起提交。

---

## 12. 关键决策记录（为什么这样做）

- **Ultra 与基础监控解耦**：8188 数据（状态/进度/日志/产物）与助手硬件数据分别来自两个端口、两套超时；任一挂掉都不影响另一套显示。
- **Ultra 是设置开关而非新固件分支**：关闭时走原代码路径，降低回归风险（已用 4 台基础布局截图验证）。
- **按主机数重排而不是缩字号硬塞**：3/4 台必须保证可读性；4 台用 2×2 + 瓦片，牺牲 GPU 型号/节点文字。
- **助手用标准库单文件**：用户要求 Linux/Windows 通用、无需安装依赖；不引入 psutil/GPUtil。
- **启停必须复刻本机启动**：直接调用用户自己的 `.bat`/命令与工作目录，避免 SSH 拉起导致的 GPU 性能问题。
- **APU 用 GTT 当显存**：统一内存架构下 carveout 无参考价值；GTT 才是模型实际占用池。
- **功耗标签改为「功耗」**：多数平台只能拿到 GPU/封装功耗，标「整机」不诚实。
