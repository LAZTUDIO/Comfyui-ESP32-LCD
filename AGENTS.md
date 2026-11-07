# AGENTS.md — ComfyUI LAN 看板

> 这是给接手的 coding agent 的入口。完整上下文、验证记录、踩坑与换机步骤都在
> **`docs/HANDOFF.md`**，动手前必须读。测试记录在 `tests/2026-11-07-ultra-layouts/RECORD.md`。

## 项目
480×320 横屏 ESP32-S3（Waveshare ESP32-S3-Touch-LCD-3.5）监控 1–4 台 ComfyUI 主机；
每台主机可运行 `assistant/assistant.py`（端口 A，默认 8189）补充 CPU/GPU/内存/显存/功耗，
面板设置里开 **Ultra** 后总览页显示这些硬件指标。

## 硬性规则
1. **`192.168.31.195`（win 主机）绝不允许远程启动 ComfyUI 或任何常驻服务。** 用户手动启动；SSH 只做只读查询、传文件、改配置。
2. **不要把密码写进仓库、argv 或提交。** 需要时向用户要。
3. **有两块同型号板子**（MAC `20:6e:f1:9a:ec:10` 才是本项目的板），烧录前先 `read-mac`。
4. **改过面板的服务器/Ultra 配置后要恢复原状**（195/95/42，Ultra 关，端口 8189）。
5. **不要回退**：按主机数自动布局、只显示真实数据、Ultra 关闭时基础布局无回归。
6. Windows 上助手必须在**桌面会话**运行；SSH/服务会话里启动 ComfyUI 会掉 GPU 性能。

## 快速命令
```bash
# 编译 / 烧录（Mac）
tools/build.sh [--upload] [--port /dev/cu.usbmodemXXXX]
# Windows
tools\build.bat [--upload] [--port COM5]

# 面板状态 / 截图
curl http://192.168.31.50:8080/stats
curl http://192.168.31.50:8080/shot.raw -o /tmp/s.raw && python3 tools/raw_to_png.py /tmp/s.raw /tmp/s.png

# 助手
python3 assistant/assistant.py --port 8189
curl http://<host>:8189/api/panel
```

## 当前状态（2026-11-07）
- Ultra 固件已烧录；1/2/3/4 台布局 + 离线降级 + 基础回归均已截图验证。
- halo `192.168.31.95`：助手 **v1.3.0** systemd 常驻（active），SSH 用 `max@192.168.31.95`（本机
  `~/.ssh/id_ed25519` 免密）；重启服务无 sudo NOPASSWD，用 `kill -9` 主进程触发
  `Restart=on-failure` 自动拉起。APU 整包功耗走 amdgpu PPT（见 v1.3.0 提交）。
- win `192.168.31.195`：**v1.3.0** 桌面 exe 常驻（托盘 + 窗口），GPU 功耗走 ADLX、
  CPU 走 PDH Energy Meter。
- 面板已恢复原配置（Ultra 关）。待用户在面板开 Ultra 做双机联调。

## 改代码时的注意
- 改中文文案后**必须**重跑字体子集（`tools/gen_cn_fonts.sh` / HANDOFF §5.7），否则方框。
- 改 `/api/panel` 字段要同步改固件 `fetchUltra()`。
- 先拍基线截图，改完逐布局截图对比，再做结论。
