@echo off
rem ComfyUI 看板助手 — Windows 启动脚本
rem 请在本机桌面双击运行（或放入「启动」文件夹）。不要在 SSH 会话里常驻。
cd /d "%~dp0"
if "%ASSISTANT_PORT%"=="" set ASSISTANT_PORT=8189
python assistant.py --port %ASSISTANT_PORT% %*
pause