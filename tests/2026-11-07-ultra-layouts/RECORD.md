# Ultra 布局上机验证 — 2026-11-07

## 目的
验证 ESP32 Ultra 总览在 1/2/3/4 台主机下的布局、对齐、降级与字体覆盖；确认 Ultra 关闭时基础布局无回归。

## 环境 / 变量
- 板：Waveshare ESP32-S3-Touch-LCD-3.5，MAC `20:6e:f1:9a:ec:10`，IP `192.168.31.50`，调试口 `:8080`。
- 数据源：Mac `192.168.31.187` 上的 `mock_panel.py`（stdlib）。ComfyUI mock 端口 8188/8190/8191/8192（含最小 WebSocket，推 execution_start/executing/progress）；助手 mock `/api/panel` 端口 8189，轮换 4 组硬件画像使各格数值不同。
- 自变量：主机数 1→2→3→4；Ultra 开/关；助手端口 8189（活）/8199（死）。
- 阈值：助手离线判定 = 固件 `offlineMs()`（1/2 台 10s、3 台 15s、4 台 20s）；实测死端口约 24s 后进入离线态。
- 截图：`/shot.raw` → `tools/raw_to_png.py`，存 `shots/`。

## 结果
| 截图 | 场景 | 结论 |
|---|---|---|
| 00_original_servers | 改动前配置存档 | 原配置 195/95/42:8188、Ultra 关、端口 8189、中文 |
| 01_ultra_1host_monitor | 1 台 MONITOR | 左卡 CPU/GPU/RAM/VRAM 四条 + 整机 W + GPU 标记 + 版本；右运行日志；无重叠 |
| 02_ultra_2host_dash | 2 台 DASH | 两列对齐；NVIDIA eye / AMD 标记正确；指标与功耗清晰 |
| 03_ultra_3host_dash | 3 台窄列 | 标题优雅截断；节点/步数与右值不重叠 |
| 04_ultra_4host_dash | 4 台 2×2 紧凑 | 2×2 百分比瓦片 + 功耗；无重叠 |
| 05_settings_ultra_row | 设置页 | 修复后提示文字无方框（见下） |
| 06_base_ultraoff_4host | Ultra 关 4 台 | 基础布局无回归（VRAM/RAM 分数 + GPU + CUDA/ROCm） |
| 07_ultra_assistant_offline | 助手死端口 | 四指标 `--`、`功耗：助手离线`；ComfyUI 状态/进度仍实时 |

## 发现并修复
- **CJK 字体缺字**：新增 Ultra 中文串（与充内助占手整机板显用由补…）不在子集内，设置页提示渲染为方框。用缓存的 `lv_font_conv`（`~/.npm/_npx/5e891675aaf2bdf7`）+ Qoder 内置 Electron 作 Node（`ELECTRON_RUN_AS_NODE=1`，因系统无 npx）重生成 4 个字号子集，重编译烧录后无方框。

## 已知取舍 / 限制
- 4 台紧凑格为塞下 4 项占用 + 功耗，**省略 GPU 型号与节点/步数文字**（进度仍有条形）；1/2/3 台保留 GPU 型号与节点/步数。符合"空间有限挑重点"。
- 调试端点（`/debug/host`、`/debug/ultra`）改运行时 + 存 NVS，但**不刷新设置页的旧标签**（服务器行/Ultra 开关显示开机值）；真实按键/键盘流程会刷新。仅调试可见的不一致。
- 验证结束已恢复原配置（195/95/42:8188、Ultra 关、端口 8189），mock 进程已按 PID 停止。

## 教训
- 系统 shell 无 node/npx 时，可用 `ELECTRON_RUN_AS_NODE=1 "<Qoder.app>/Contents/MacOS/Qoder CN"` 跑缓存的 npm CLI，无需装 Node。
- 新增任何中文 UI 串后必须重跑字体子集，否则方框；`extract_cjk.py` 扫 .ino 全量汉字。

---

# 看板助手部署 — 2026-11-07

## halo（192.168.31.95，Ubuntu 7.0.0-38 / Py3.14）
- 路径 `~/comfyui-assistant/`，systemd `comfyui-assistant.service`（`User=<user>`，`ExecStart=/usr/bin/python3 .../assistant.py --port 8189`），已 `enable --now`，状态 active。
- 配置：`comfy_url=http://127.0.0.1:8188`，`workdir=/home/<user>/ComfyUI`，`start_cmd` 复刻现有启动参数（venv python `main.py --highvram --use-ck-attention --listen 0.0.0.0 --port 8188`）。
- `/api/panel` 实测：gpu `AMD Radeon 8060S`，cpu 0（空载），ram 63，vram 50（GTT），w 15.1，cpu_temp 42–43；满载 32 线程时 cpu=100、w=118 → CPU 采样正确。
- **修复 1（APU 显存）**：Strix Halo 的 `mem_info_vram_total` 仅 512MB（核显 carveout），原报 72% 无意义；改为 APU（vram_total<4G 且 gtt_total>4G）读 `mem_info_gtt_*` → 52.5/104 GiB = 50%。
- **修复 2（APU 名称）**：pci.ids/rocm-smi 都取不到 0x1586，回退解析 `/proc/cpuinfo` 的 `w/ Radeon 8060S` → `AMD Radeon 8060S`。
- 注：`power_note` 为「仅 GPU（CPU 功耗不可读）」，halo 的 hwmon 无 CPU 功耗；该值仍随 CPU 满载升到 118W，近似封装功耗。

## win（192.168.31.195，Windows）
- 路径 `%USERPROFILE%\comfyui-assistant\`（assistant.py / webui.html / start.bat / assistant_config.json）。
- 配置 `start_cmd=D:\jz-comfyui\Start_ComfyUI_H3.bat`（与本地手动启动同一脚本，env/参数完全一致）。
- **按用户长期规则未启动任何服务**；仅做一次性只读采集探针 `probe_win.py`（导入模块采样即退出）。
- 探针实测：cpu 8.8%，ram 31.5%（68GB），gpu 名 `AMD Radeon RX 7900 XT`，util 0.01%（空载），vram 4.56/20 GiB=21.2%。
- **修复 3（win 显存）**：无 amdsmi 时 `AdapterRAM` 被截断到 4GB；新增 PowerShell：总量读注册表 `HardwareInformation.qwMemorySize`（20GB），已用读 `\GPU Adapter Memory(*)\Dedicated Usage` 求和。
- 限制：win 无 amdsmi 时 GPU 功耗/温度不可得（`--`）；CPU 功耗无 RAPL 不可得。

## 固件标签
- `整机 N W` → `功耗 N W`：助手在多数主机只能拿到 GPU/封装功耗，标「整机」不实。

## 待用户操作
1. 在 win 桌面会话双击 `%USERPROFILE%\comfyui-assistant\start.bat`（不要在 SSH 里常驻）。
2. 面板设置里开 Ultra、端口 A=8189，即可同时看 halo/win 的实时硬件数据。

## 观察（非本次改动）
- halo 空闲时固件日志出现成对的 `WS 已断开 / WS 已连接`。空闲 ComfyUI 会周期性关闭被动 WS（见记忆 reference-comfyui-monitor-api，约 60–90s），属已知现象，非故障。
