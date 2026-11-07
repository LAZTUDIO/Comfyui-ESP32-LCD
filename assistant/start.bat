@echo off
rem ComfyUI panel assistant launcher.
rem Prefer the packaged exe (tray + status window); fall back to running the source.
rem NOTE: keep this file ASCII-only - cmd reads .bat as GBK and mangles UTF-8 Chinese.
cd /d "%~dp0"

if exist "%~dp0ComfyUIPanelAssistant.exe" (
  start "" "%~dp0ComfyUIPanelAssistant.exe" %*
  exit /b 0
)

if "%ASSISTANT_PORT%"=="" set ASSISTANT_PORT=8189
python assistant.py --port %ASSISTANT_PORT% %*
pause
