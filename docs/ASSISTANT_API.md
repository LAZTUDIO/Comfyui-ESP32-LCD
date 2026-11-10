# ComfyUI 看板助手 HTTP 接口文档

**版本**：对应助手 v1.3.1
**适用平台**：Windows / Linux（同一套接口，字段差异见 §8）
**文档依据**：`assistant/assistant.py` 源码 + TX265K-7900XT（Windows build 26300）实测返回

---

## 1. 定位与架构

```
调度站 / 远程 Agent
        │  HTTP :8189
        ▼
ComfyUI 看板助手（assistant.py / exe，常驻）
        │  本机探测 + 启停托管
        ├──▶ ComfyUI 原生接口 :8188（任务提交仍走这里，助手不代理）
        └──▶ 主机硬件传感器（ADLX / PDH-RAPL / nvidia-smi / sysfs-hwmon）
```

分工边界，一句话说清：

| 事项 | 走谁 | 说明 |
|---|---|---|
| 提交工作流、上传图片、 websocket 进度 | **8188 原生接口** | 助手不做任务转发 |
| 主机硬件状态（温度/功耗/占用/显存/磁盘） | **助手 8189** | 8188 只有 `/system_stats` 一点静态信息，温度功耗一概没有 |
| ComfyUI 启动 / 停止 / 队列计数 / stdout 日志 | **助手 8189** | 含外部启动实例的停止 |

## 2. 基础约定

| 项 | 值 |
|---|---|
| 协议 | HTTP/1.1，JSON（UTF-8，无 BOM） |
| 默认端口 | 8189（`--port` 可改，配置持久化在 exe 旁 `assistant_config.json`） |
| 绑定 | 0.0.0.0，局域网可直接访问 |
| CORS | 全部响应带 `Access-Control-Allow-Origin: *`，浏览器直调无障碍 |
| 时间戳 | 毫秒级 Unix time（`ts` 字段） |
| 字节单位 | 内存/显存/磁盘均为 **bytes**（原始值，不自查单位） |
| 功率单位 | 瓦特 W；温度 ℃；频率 MHz |
| 不可读字段 | 一律 `null`，不省略键、不返回 0 冒充 |
| 错误格式 | 404 返回 `{"error":"not found"}`；业务失败返回 200 + `{"ok":false,"msg":"..."}` |
| 幂等性 | GET 全部只读；start/stop 可重复调用，重复 start 返回 `ok:false` 不产生副作用 |

助手必须运行在**主机本地桌面会话**（Windows）或本机终端 / systemd 用户服务（Linux），
不能落在 SSH / 服务会话里，否则由它拉起的 ComfyUI GPU 性能受限。是否踩坑看
`/api/status` 里 `assistant.session_warning`：空串 = 正常，非空 = 有问题（文案自解释）。

## 3. 接口总览

| # | 方法 | 路径 | 用途 | 建议轮询间隔 |
|---|---|---|---|---|
| 1 | GET | `/api/health` | 存活探测 + 版本 | 3~10 s |
| 2 | GET | `/api/panel` | **轻量状态**（调度站首选） | 1~2 s |
| 3 | GET | `/api/status` | **全量状态**（含硬件全字段） | 2~5 s |
| 4 | GET | `/api/history` | 近 5 分钟 1 Hz 曲线 | 按需 |
| 5 | GET | `/api/logs` | ComfyUI stdout 尾部 120 行 | 按需 |
| 6 | GET | `/api/config` | 读当前配置 | 按需 |
| 7 | POST | `/api/comfy/start` | 启动 ComfyUI | 事件驱动 |
| 8 | POST | `/api/comfy/stop` | 停止 ComfyUI（带任务保护） | 事件驱动 |
| 9 | POST | `/api/config` | 改配置（comfy_url / workdir / start_cmd / stop_timeout） | 事件驱动 |

---

## 4. GET 接口明细

### 4.1 `GET /api/health` —— 存活探测

```json
{"ok": true, "version": "1.3.1"}
```

调度站用它做三件事：助手是否在线、版本是否升级过、（配合超时）网络是否通。
**不要**用 8188 探测主机存活——ComfyUI 停了 8188 就失联，而助手应该常驻。

### 4.2 `GET /api/panel` —— 轻量状态（调度站主用接口）

实测样例（TX265K-7900XT，ComfyUI 运行中）：

```json
{
  "v": 1,
  "host": "TX265K-7900XT",
  "ts": 1794278139083,
  "comfy": {
    "up": true,          // 8188 API 是否可达（轮询就绪用这个）
    "running": 0,        // 正在执行的任务数
    "pending": 0,        // 排队任务数
    "ver": "0.43.0",
    "job_ms": 0          // 当前任务已运行毫秒数（无任务为 0）
  },
  "gpu": {
    "name": "AMD Radeon RX 7900 XT",
    "vendor": "amd",     // nvidia / amd / unknown
    "util": 0.0,         // GPU 占用 %
    "temp": 41.0,        // GPU 温度 ℃
    "clk": 37            // 核心频率 MHz
  },
  "cpu": 12,             // CPU 占用 %（取整）
  "ram": 38,             // 内存占用 %（取整）
  "vram": 18,            // 显存占用 %（取整）
  "vram_used": 3962568704,
  "vram_total": 21458059264,
  "ram_used": 25521475584,
  "ram_total": 68044115968,
  "w": 120.1,            // 整机功率 W（GPU+CPU，APU 平台为 SoC 整包）
  "cpu_temp": null       // CPU 温度 ℃，平台读不到就是 null
}
```

单次响应约 400 字节，1 秒轮询无压力。

### 4.3 `GET /api/status` —— 全量状态

字段比 panel 多一档，含磁盘、GPU 细节（热点温度/风扇转速）、助手与主机元信息。
实测样例（省略 `cpu.cores` 数组尾部）：

```json
{
  "assistant": {
    "version": "1.3.1",
    "port": 8189,
    "uptime_s": 1943,
    "session_warning": ""          // 非空 = 运行在错误会话（SSH/Session 0），调度站应告警
  },
  "host": {
    "hostname": "TX265K-7900XT",
    "ip": "192.168.31.195",
    "os": "Windows build 26300",   // Linux 上为 "Linux 6.x.x-xxx"
    "python": "3.13.14",
    "arch": "AMD64"
  },
  "cpu": {
    "pct": 11.3,
    "cores": [],                   // Windows 为空数组；Linux 为逐核占用数组
    "freq_mhz": null,              // Linux 有值，Windows 无
    "temp_c": null,
    "power_w": 43.0,               // CPU 封装功耗（RAPL）
    "count": 20
  },
  "ram": { "total": 68044115968, "used": 25524604928, "pct": 37.5 },
  "gpu": {
    "vendor": "amd",
    "name": "AMD Radeon RX 7900 XT",
    "driver": "",
    "vram_total": 21458059264,
    "util": 3.0,
    "mem_util": 18.5,              // 显存带宽占用 %（NVIDIA 有，AMD 多数无）
    "vram_used": 3962568704,
    "vram_pct": 18.5,
    "power_w": 76.0,               // AMD = 整板功耗（ADLX GPUTotalBoardPower）
    "temp_c": 41.0,
    "clock_mhz": 82,
    "fan_pct": null,               // NVIDIA 有；AMD 报 fan_rpm
    "hotspot_c": 50.0,             // 热点温度（AMD）
    "fan_rpm": 544                 // 风扇转速（AMD）
  },
  "power": {
    "total_w": 119.0,
    "gpu_w": 76.0,
    "cpu_w": 43.0,
    "note": "GPU+CPU 实测合计"     // 组成说明，见下方取值表
  },
  "disk": {
    "path": "D:\\jz-comfyui\\ComfyUI",
    "total": 1352543866880,
    "free": 275833135104,
    "pct": 79.6
  },
  "net": {},                       // Windows 空；Linux 含 rx_bps/tx_bps
  "system": {
    "uptime_s": 339374,
    "load": null,                  // Linux 为 [1min, 5min, 15min]
    "procs": null                  // Linux 为进程总数
  },
  "comfyui": {
    "state": "running",            // 状态机，见 §5
    "url": "http://127.0.0.1:8188",
    "port": 8188,
    "version": "0.43.0",
    "python": "3.12.10",
    "pytorch": "2.14.0+rocm10.1.0",
    "device_name": "cuda:0 AMD Radeon RX 7900 XT : native",
    "vram_total": 21458059264,
    "vram_free": 21247426560,
    "ram_total": 68044115968,
    "ram_free": 42519539712,
    "running": 0,
    "pending": 0,
    "job_start_ms": null,          // 当前任务开始时刻（ms 时间戳），无任务 null
    "managed": true,               // true = 由助手拉起；false = 外部启动
    "pid": 23608,
    "started_at": 1794276249957,   // 助手拉起时刻（仅 managed）
    "external_cmd": "",
    "log_ver": 379                 // 日志版本号，变化 = 有新日志
  },
  "ts": 1794278155517
}
```

`power.note` 取值：

| 值 | 含义 |
|---|---|
| `GPU+CPU 实测合计` | 两路都读到，total = 相加 |
| `仅 GPU（CPU 功耗不可读）` | Windows AMD 桌面平台常见（PDH Energy Meter 不暴露） |
| `仅 CPU（GPU 功耗不可读）` | 独显掉驱动等异常场景 |
| `APU 整包（PPT，含 CPU+GPU）` | APU 平台 hwmon PPT 已含 CPU，**不得再与 CPU 功耗相加** |
| `不可读` | 两路全空 |

### 4.4 `GET /api/history` —— 曲线数据

```json
{"points": [ {"ts": 1794278000000, "cpu": 11.2, "gpu": 3.0, "ram": 37.5, "vram": 18.5, "w": 119.0}, ... ]}
```

环形缓冲，最多 **300 点 × 1s = 5 分钟**。调度站要更长历史请自行落库。
`vram` / `w` 读不到时为 null。

### 4.5 `GET /api/logs` —— ComfyUI 日志

```json
{"lines": ["10:23:01 Import times for custom nodes:...", "..."], "ver": 379}
```

尾部最多 120 行，带 `HH:MM:SS` 前缀。`ver` 单调递增，调度站可比对 ver 决定是否重新拉取。
这是**助手托管启动**时管道采集的 stdout；外部启动的实例日志采集不到。

### 4.6 `GET /api/config` —— 读配置

```json
{"comfy_url": "http://127.0.0.1:8188", "workdir": "D:\\jz-comfyui\\ComfyUI", "start_cmd": "...", "stop_timeout": 15, "port": 8189}
```

`start_cmd` 是启动命令原文（与本机手动启动完全一致），调度站可用于展示/校验。

---

## 5. ComfyUI 状态机

`comfyui.state` 取值与迁移：

```
              POST /api/comfy/start
   stopped ────────────────────────────▶ starting
      ▲                                    │ 8188 API 就绪（≤ 数十秒，模型加载期 HTTP 无响应）
      │ POST /api/comfy/stop               ▼
   stopped ◀─────────────────────────── running   ⇄   external
              （managed=false 的实例显示为 external，启停接口同等对待）
```

| state | 含义 | `managed` | 典型场景 |
|---|---|---|---|
| `stopped` | 没有实例在跑 | false | 初始 / 已停止 |
| `starting` | 进程已拉起，8188 还没就绪 | true | start 之后、模型加载完之前 |
| `running` | API 就绪且**由助手托管** | true | 助手拉起并正常运行 |
| `external` | API 就绪但**不是**助手拉起（或检测到外部进程） | false | 用户手动双击 run_nvidia.bat / 手动 python main.py |

**调度站判"可用"只认 `state == "running" || state == "external"`（或 panel 的 `comfy.up`）**，
不要拿 `managed` 当可用条件——external 同样能干活。

---

## 6. POST 接口明细

### 6.1 `POST /api/comfy/start`

请求体可空（`{}`）。

| 场景 | 返回 |
|---|---|
| 成功发起 | `{"ok": true, "msg": "已发起启动，等待 API 就绪…"}` |
| 助手已有实例 | `{"ok": false, "msg": "ComfyUI 已由助手运行中"}` |
| 端口被外部实例占用 | `{"ok": false, "msg": "端口上已有 ComfyUI 在运行（外部启动），请先停止或改用接管"}` |
| 未配置启动命令 | `{"ok": false, "msg": "未配置启动命令：请在设置里填写 start_cmd（与本机手动启动一致）"}` |
| 进程创建失败 | `{"ok": false, "msg": "启动失败: <异常>"}` |

要点：

1. **start 是异步的**。`ok:true` 只代表进程已拉起，不代表可用。就绪判定用轮询：

   ```
   循环（建议 2s 间隔、总超时 120s）:
       GET /api/panel → comfy.up == true  → 就绪
       GET /api/status → comfyui.state == "running" → 就绪（更严格）
   ```

   模型加载期 ComfyUI 的 HTTP 可能长时间无响应，此时 state 停在 `starting`，属正常，
   不要把"探测 8188 失败"当成启动失败。
2. start 有现场探测：刚 stop 完 ≤8 s 内端口残留真值会被识别并拒绝（防止误判外部实例）。
3. 启动命令在 `POST /api/config` 里配置，Windows 支持 `.bat/.cmd` 与裸命令行。

### 6.2 `POST /api/comfy/stop`

请求体：`{"force": true|false}`（可省略，默认 false）。

| 场景 | 返回 |
|---|---|
| 无任务，正常停止 | `{"ok": true, "msg": "已停止"}` |
| **队列有任务且未加 force**（15 s 内确认过） | `{"ok": false, "msg": "队列中还有任务（running=1 pending=2），确认中断请带 force=true"}` |
| 加 force 强制中断 | `{"ok": true, "msg": "已停止"}` |
| 没有实例 | `{"ok": false, "msg": "没有正在运行的 ComfyUI"}` |
| 外部实例停止失败（权限） | `{"ok": false, "msg": "停止失败（权限不足？）"}` |

要点：

1. **任务保护**：助手后台采样最近 15 s 内确认过 `running+pending > 0` 时拒绝停止，
   调度站收到该返回后，若确要中断，原样重发并带 `"force": true`。
   之所以用缓存计数而非现场探测：模型加载期 8188 无响应，现场探测会误判"没有任务"。
2. 停止是**进程树终止**：Windows `taskkill /T /F`，Linux `killpg` SIGTERM → 等待 → SIGKILL。
   外部启动的实例（state=external）同样能停，按监听端口 / 进程命令行定位。
3. 停止成功后 `state` 立即回落 `stopped`（无残留真值窗口），但 **8188 端口释放可能有数秒延迟**，
   紧接着的 start 若撞上会返回"端口上已有 ComfyUI"，间隔 3 s 重试即可。

调度站推荐停止流程：

```
POST /api/comfy/stop {}
  ├─ ok:true  → 完成
  └─ ok:false 且 msg 含 "force=true" → 确认策略后 POST /api/comfy/stop {"force":true}
```

### 6.3 `POST /api/config`

请求体（任意子集，未带的键不动）：

```json
{"comfy_url": "http://127.0.0.1:8188", "workdir": "D:\\jz-comfyui\\ComfyUI",
 "start_cmd": "python main.py --windows-standalone-build", "stop_timeout": "15"}
```

返回 `{"ok": true, "cfg": {...全量配置}}`；`comfy_url` 格式不合法返回 400。
配置**立即生效并持久化**（写 exe 旁 `assistant_config.json`，重启不丢）。
`stop_timeout` 为进程树终止前的优雅等待秒数。

**v1.3.1 路径归一化**（Windows）：`workdir` 与 `start_cmd` 中的连续反斜杠会被自动
收敛为单道（UNC 的 `\\` 开头前缀保留）。调用方无需担心 cmd / PowerShell / bash
转义差异——发 `D:\\\\a\\\\b.bat` 或 `D:\a\b.bat`，落库均为 `D:\a\b.bat`。
JSON 返回里的 `\\` 是转义显示，实际值只有一道。

---

## 7. 调度站集成清单（照抄即用）

1. **存活性**：ping `GET /api/health`（3~10 s 一拍）。连续失败 → 主机/助手离线告警。
   8188 失联 ≠ 主机失联，两级分开判断。
2. **渲染资源视图**：轮询 `GET /api/panel`（1~2 s），拿温度/功耗/占用/显存/队列。
3. **启动**：`POST /api/comfy/start` → 轮询 panel 直到 `comfy.up == true`（2 s 间隔，120 s 上限），
   超时未就绪拉 `GET /api/logs` 看原因。
4. **停止**：按 §6.2 流程，尊重 force 保护。
5. **长任务监控**：`comfy.job_ms` 是当前任务计时；`running/pending` 判断空闲（都可 0 = 空闲，可下机）。
6. **告警阈值建议**：`gpu.temp ≥ 85`、`gpu.hotspot_c ≥ 100`、`disk.pct ≥ 90`、
   `assistant.session_warning != ""`、state 停留 `starting` 超 180 s。
7. **主机元信息**：`/api/status` 的 `host.*` + `gpu.name/vendor` 用于资产登记；
   `assistant.version` 用于升级提醒。
8. **多主机扩展**：每台主机跑一份助手（默认 8189），调度站按 `host:8189` 逐一纳管即可，
   无中心注册机制。

## 8. 双平台字段差异（调度站解析时注意）

| 字段 | Windows | Linux |
|---|---|---|
| `cpu.cores` | `[]`（不提供逐核） | 逐核占用数组 |
| `cpu.freq_mhz` | null | 当前频率 |
| `cpu.temp_c` | 视平台（Intel 多数可读；实测本 AMD 机 null） | k10temp/zenpower 可读 |
| `cpu.power_w` | PDH Energy Meter（Intel 平台普遍有；AMD 桌面多数无 → null） | RAPL 或 hwmon，多数平台有 |
| `net` | `{}` | `rx_bps` / `tx_bps` |
| `system.load` / `system.procs` | null | 有值 |
| `disk.read_bps/write_bps` | 无 | 有 |
| `gpu.fan_pct` | NVIDIA 有；AMD null | NVIDIA 有；AMD null |
| `gpu.fan_rpm` | AMD（ADLX）有 | AMD hwmon 有 |
| `gpu.hotspot_c` | AMD（ADLX）有 | AMD 独显有；APU null |
| GPU 功耗语义 | AMD=整板功率（ADLX）；NVIDIA=power.draw | AMD=board/APU PPT；NVIDIA=power.draw |

解析原则：**一切按 null 容错**。字段存在但为 null 是"该平台传感器不暴露"的正常态，
不是故障；连续多分钟从有值变 null 才需要告警。

## 9. 端到端示例（curl）

```bash
# 存活
curl http://192.168.31.195:8189/api/health

# 轻量轮询
curl http://192.168.31.195:8189/api/panel

# 启动（异步，随后轮询 /api/panel 的 comfy.up）
curl -X POST http://192.168.31.195:8189/api/comfy/start -d "{}" -H "Content-Type: application/json"

# 停止（队列有任务时先被拒，再带 force）
curl -X POST http://192.168.31.195:8189/api/comfy/stop -d "{}" -H "Content-Type: application/json"
curl -X POST http://192.168.31.195:8189/api/comfy/stop -d '{"force":true}' -H "Content-Type: application/json"
```

## 10. 版本记录

| 助手版本 | 接口变更 |
|---|---|
| 1.0.0 | 初版：status / panel / history / config GET，config POST |
| 1.3.0 | 新增 `/api/comfy/start`、`/api/comfy/stop`（含 force 任务保护）、`/api/logs`、`/api/health`；state 机引入 external |
| 1.3.1 | stop 保护改用缓存计数（模型加载期不误判）；start 加现场探测；AMD Windows 功耗走 ADLX 整板功率 |
