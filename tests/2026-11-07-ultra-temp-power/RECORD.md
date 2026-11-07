# Ultra 总览：GPU 温度行 + 包功耗 — 2026-11-07（固件 v5）

## 需求
「在 esp32 端总览界面 Ultra 板上也加入 GPU 温度，功耗统一为包功耗（cpu+gpu）」

## 改动（firmware/comfyui_panel/comfyui_panel.ino）
- Ultra 网格卡与单主机 MONITOR 卡的指标行 4 → 5：新增 **TEMP** 行
  （GPU 温度，bar 按 0-100°C 缩放，颜色沿用 pctColor 的 60/85 阈值）。
- MONITOR 卡行距 30 → 27px，功耗/GPU 名/版本行下移，卡内不溢出。
- 功耗文案统一为 **「包功耗 xx W」**（英文 pkg xx W）：
  - 助手 `/api/panel` 的 `w` 在两台主机上语义一致——win = GPU+CPU 实测合计
    （ADLX 整板 + PDH RAPL），halo = APU SoC 整包 PPT（见助手 v1.3.0）。
- 4 台紧凑格（2×2）不加第五行（高度不够），GPU 温度并入功耗行：
  「包功耗 xx W · GPU xx°C」。
- `tools/extract_cjk.py` 增补 `°` 符号并重跑字体子集（gen_cn_fonts.sh）。

## 数据链路
`/api/panel` 已有 `"gpu": {"temp": ...}` 与 `w` 字段，固件 fetchUltra 早已解析
（gpuTemp/watts），本次只是把它显示出来——助手侧无需改动。

## 上机验证（备用板 f1:68，COM5，IP 192.168.31.134）
| 场景 | 结果 |
|---|---|
| 启动 | `boot (v5)`，heap 203688，psram 8388608，WiFi 在线 |
| 单主机 MONITOR（Ultra 开） | 5 行齐全：CPU 0% / GPU 0% / RAM 21% / VRAM 19% / **TEMP 37°C**，**包功耗 14 W**，无方框、无重叠 |
| 临时加第二台（win）2 列网格 | 两卡五行齐全（TEMP 37°C / 43°C），包功耗 13 W / 124 W，GPU 名与版本行完整可见 |
| 恢复 | 已删除临时第二台，恢复 1 台配置；Ultra 保持开（可用设置页或 /debug/ultra 关） |

Flash 1598543 B (50%)，RAM 154092 B (47%)。截图见 shots/。

## 注意
- 温度行颜色阈值复用 pctColor：<60 绿 / <85 黄 / ≥85 红（对温度即 <60°C /
  <85°C / ≥85°C）。
- halo 是 APU，TEMP 是 edge 温度、无热点；桌面卡（win）另有 hotspot_c，
  当前固件未显示，需要时可用 `/api/status` 的 gpu.hotspot_c 扩展。
