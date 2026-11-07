#!/bin/sh
# ComfyUI 看板助手 — Linux 启动脚本
# 请在本机桌面终端运行，或配置为 systemd 服务（见 README）。
cd "$(dirname "$0")"
exec python3 assistant.py --port "${ASSISTANT_PORT:-8189}" "$@"