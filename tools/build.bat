@echo off
rem Compile (and optionally upload) the panel firmware on Windows.
rem Usage: tools\build.bat [--upload] [--port COM5]
setlocal EnableExtensions
cd /d "%~dp0.."

set "FQBN=esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB"
set "SKETCH=firmware\comfyui_panel"
set "UPLOAD=0"
set "PORT="

:args
if "%~1"=="" goto build
if /i "%~1"=="--upload" (set "UPLOAD=1" & shift & goto args)
if /i "%~1"=="--port" (set "PORT=%~2" & shift & shift & goto args)
echo unknown arg: %~1
exit /b 1

:build
arduino-cli compile --clean -b "%FQBN%" "%SKETCH%"
if errorlevel 1 exit /b 1

if "%UPLOAD%"=="1" (
  if "%PORT%"=="" (
    for /f "tokens=1" %%p in ('arduino-cli board list ^| findstr /i "COM"') do (
      if not defined PORT set "PORT=%%p"
    )
  )
  if "%PORT%"=="" (echo no serial port found & exit /b 1)
  echo uploading to %PORT%
  arduino-cli upload -p "%PORT%" -b "%FQBN%" "%SKETCH%"
)
