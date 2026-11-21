@echo off
chcp 65001 >nul
setlocal

rem 一键烧录脚本：把编译好的固件刷到板子上
rem 用法：先插上板子的 USB 线，然后双击本脚本，或运行 flash.bat COM口

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM7"

set "TOOLS=C:\Espressif\tools"
set "PY=%TOOLS%\python\v5.5.5\venv\Scripts\python.exe"
if not exist "%PY%" set "PY=%TOOLS%\python\v6.1\venv\Scripts\python.exe"
if not exist "%PY%" set "PY=python"

set "ESPTOOL=%TOOLS%\python\v5.5.5\venv\Scripts\esptool.py"
if not exist "%ESPTOOL%" set "ESPTOOL=python -m esptool"

echo ================================================
echo  烧录小智固件到 %PORT%
echo  如果端口不对，用  flash.bat COM3  这样指定
echo ================================================
echo.

if not exist "build\xiaozhi.bin" (
  echo [错误] 找不到 build\xiaozhi.bin，请先编译固件
  pause
  exit /b 1
)

echo 正在烧录（约 1-2 分钟，期间不要拔线）...
echo.

%ESPTOOL% --chip esp32s3 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash ^
  --flash_mode dio --flash_size 16MB --flash_freq 80m ^
  0x0 build\bootloader\bootloader.bin ^
  0x8000 build\partition_table\partition-table.bin ^
  0x19000 build\ota_data_initial.bin ^
  0x20000 build\xiaozhi.bin ^
  0xc00000 build\generated_assets.bin

if errorlevel 1 (
  echo.
  echo [烧录失败] 检查：
  echo   1. 板子 USB 是否插好，端口是不是 %PORT%
  echo   2. 是否有别的程序占着串口（比如串口调试助手）
  echo   3. 试着按住板子 BOOT 键再重新运行
) else (
  echo.
  echo [成功] 固件已烧录，板子会自动重启。
  echo 重启后等几秒连上 WiFi，再打开控制台 http://localhost:8800
)

pause
