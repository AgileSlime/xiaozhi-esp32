@echo off
setlocal EnableDelayedExpansion
title XiaoZhi RLCD Console

set "DIR=%~dp0"
set "JAR=%DIR%target\xiaozhi-console.jar"

echo ==================================================
echo   XiaoZhi RLCD Console
echo   Browser: http://localhost:8800
echo ==================================================
echo.

rem ---------- 1. check jar ----------
if not exist "%JAR%" (
  echo [ERROR] Main program not found:
  echo         %JAR%
  echo.
  echo   Make sure the zip is fully extracted, and "target" folder
  echo   sits next to this start.bat.
  echo.
  pause
  exit /b 1
)

rem ---------- 2. find Java 17+ ----------
echo Looking for Java 17 or newer...
set "JAVA="
for /f "delims=" %%i in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%DIR%find-java.ps1" 2^>nul') do set "JAVA=%%i"

if not defined JAVA (
  echo.
  echo [ERROR] No Java 17+ found on this machine.
  echo.
  where java >nul 2>&1
  if not errorlevel 1 (
    echo   Your current java is a lower version:
    java -version 2>&1
    echo.
  )
  echo   This console needs Java 17 or newer ^(JRE is enough^).
  echo.
  echo   Download:
  echo     https://adoptium.net/temurin/releases/?version=17
  echo.
  echo   Pick the Windows x64 .msi, next-next-finish.
  echo   No environment variables needed - just run start.bat again.
  echo.
  pause
  exit /b 1
)

echo.
echo Using Java: %JAVA%
"%JAVA%" -version 2>&1
echo.

rem ---------- 3. start ----------
echo Starting, browser will open in a few seconds...
echo Close this window to stop the console.
echo.

start "" "http://localhost:8800"
"%JAVA%" -jar "%JAR%"

echo.
echo Console exited. If it closed immediately, screenshot the error above.
pause
