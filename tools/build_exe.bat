@echo off
rem Build single-file exe (tray + status window). ASCII only: cmd reads .bat as GBK.
rem Usage: tools\build_exe.bat
rem Output: assistant\dist\ComfyUIPanelAssistant.exe
setlocal EnableExtensions
cd /d "%~dp0.."
set "ROOT=%CD%"

python tools\make_icon.py
if errorlevel 1 exit /b 1

rem Onefile exe. AMD GPU sensors need the official "amd-adlx" wheel bundled in
rem (pip install amd-adlx) - without it power/temp fall back to "--".
python -m PyInstaller --noconfirm --clean ^
  --onefile --windowed ^
  --name ComfyUIPanelAssistant ^
  --icon "%ROOT%\assistant\app.ico" ^
  --add-data "%ROOT%\assistant\webui.html;." ^
  --paths "%ROOT%\assistant" ^
  --collect-all adlx ^
  --distpath "%ROOT%\assistant\dist" ^
  --workpath "%ROOT%\assistant\build" ^
  --specpath "%ROOT%\assistant" ^
  "%ROOT%\assistant\tray_app.py"
if errorlevel 1 exit /b 1

echo.
echo Built: assistant\dist\ComfyUIPanelAssistant.exe
dir /b "%ROOT%\assistant\dist\ComfyUIPanelAssistant.exe"
