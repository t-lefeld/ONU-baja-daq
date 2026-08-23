@echo off
REM ====================================================================
REM  Build Telemetry.exe
REM
REM  Run this from a normal Command Prompt in this folder:
REM      build.bat
REM
REM  Output: dist\Telemetry.exe  (single file, no install needed)
REM
REM  Must run on Windows. PyInstaller freezes the interpreter of the
REM  machine it runs on; it cannot cross-compile a Windows exe.
REM ====================================================================

setlocal

echo.
echo === Checking Python ===
python --version
if errorlevel 1 (
    echo.
    echo Python not found on PATH.
    echo Install it from python.org and tick "Add python.exe to PATH".
    exit /b 1
)

echo.
echo === Installing build dependencies ===
python -m pip install --upgrade pip
REM PySide6-Essentials + shiboken6 instead of the "PySide6" umbrella package:
REM the umbrella pulls in PySide6-Addons, which bundles QtWebEngine - a full
REM embedded Chromium. The native window only uses QtCore/QtGui/QtWidgets
REM (all in Essentials), so Addons is never installed and there is nothing
REM Chromium-shaped anywhere in this app, intentionally.
python -m pip install --upgrade pyinstaller PySide6-Essentials shiboken6 pyserial aiohttp
if errorlevel 1 (
    echo.
    echo Dependency install failed.
    exit /b 1
)

echo.
echo === Running the test suite first ===
REM A broken build is easier to diagnose before it is frozen than after.
python ..\tools\test_roundtrip.py
if errorlevel 1 (
    echo.
    echo Tests failed - fix that before building.
    exit /b 1
)

echo.
echo === Building ===
REM Best-effort wipe of the previous build tree, then build WITHOUT --clean.
REM
REM Why not --clean: this folder lives in OneDrive, and OneDrive (plus HP Sure
REM Click / Wolf Security on this machine) intermittently holds an open handle
REM on build\telemetry\localpycs. --clean makes PyInstaller shutil.rmtree the
REM build directory and treat a failure to remove it as fatal, so a transient
REM lock on one empty scratch folder kills the whole build with
REM "PermissionError: [WinError 5] Access is denied". Without --clean,
REM PyInstaller is perfectly happy to reuse an existing build directory.
REM
REM The rmdir below still clears stale state when Windows lets it, but 2>nul
REM and the absent errorlevel check mean a locked folder is a shrug instead of
REM a failed build. If you ever need a guaranteed-pristine build, close
REM Explorer windows on this folder, pause OneDrive sync, delete build\ by
REM hand, and re-run.
if exist build rmdir /s /q build 2>nul

python -m PyInstaller --noconfirm telemetry.spec
if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo.
echo === Smoke test: native window (default mode) ===
REM Start it, let it come up, confirm the process is still alive (not a
REM crash-on-launch), kill it. This is the mode almost everyone will use.
start "" /b dist\Telemetry.exe --sim
timeout /t 4 /nobreak >nul
tasklist /fi "imagename eq Telemetry.exe" | find /i "Telemetry.exe" >nul
if errorlevel 1 (
    echo.
    echo Telemetry.exe is not running - it crashed on launch. Check
    echo the log file next to the exe for details.
    exit /b 1
)
echo Native window came up and is still running.
taskkill /f /im Telemetry.exe >nul 2>&1

echo.
echo === Smoke test: --web dashboard ===
REM Catches a missing static/ bundle, which otherwise only shows up when
REM you open the page.
start "" /b dist\Telemetry.exe --web --sim --no-browser --http-port 8799
timeout /t 6 /nobreak >nul
curl -s -o nul -w "index page HTTP %%{http_code}\n" http://127.0.0.1:8799/
curl -s http://127.0.0.1:8799/api/status
echo.
taskkill /f /im Telemetry.exe >nul 2>&1

echo.
echo === Adding a Desktop shortcut ===
call make_shortcut.bat

echo.
echo ====================================================================
echo  Built: dist\Telemetry.exe
echo.
echo  Double-click the Telemetry shortcut on your Desktop (or the exe
echo  itself). With no arguments it looks for the E22 dongle and falls
echo  back to the simulator if there isn't one.
echo.
echo  It opens as its own native window - no browser, no Chrome or Edge
echo  involved at all. Use "Telemetry.exe --web" if you want the old
echo  browser-based dashboard instead (e.g. to view from your phone with
echo  --host 0.0.0.0).
echo.
echo  Logs, CSVs and settings.json are written next to the exe.
echo ====================================================================

endlocal
