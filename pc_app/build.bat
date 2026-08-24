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
echo === Smoke test: default mode (browser dashboard) ===
REM Start it, let it come up, confirm the process is still alive (not a
REM crash-on-launch), kill it. This is the mode almost everyone will use.
REM --no-browser matters here: the default launch opens an Edge/Chrome
REM app-mode window, and a build script has no business throwing a window
REM in your face on the way past. It was only absent before because the
REM default used to be the native Qt window.
start "" /b dist\Telemetry.exe --sim --no-browser
timeout /t 4 /nobreak >nul
tasklist /fi "imagename eq Telemetry.exe" | find /i "Telemetry.exe" >nul
if errorlevel 1 (
    echo.
    echo Telemetry.exe is not running - it crashed on launch. Check
    echo the log file next to the exe for details.
    exit /b 1
)
echo Default mode came up and is still running.
taskkill /f /im Telemetry.exe >nul 2>&1

echo.
echo === Smoke test: --web dashboard ===
REM Catches a missing static/ bundle, which otherwise only shows up when
REM you open the page.
start "" /b dist\Telemetry.exe --web --sim --no-browser --http-port 8799
timeout /t 6 /nobreak >nul
curl -s -o nul -w "index page HTTP %%{http_code}\n" http://127.0.0.1:8799/
REM Must be 200. The 4 alternate designs are bundled by telemetry.spec's
REM datas; when they were not (every build before 2026-08-23) this route
REM 404'd in the exe while working fine from source, and nothing caught it
REM because server.py logs the missing directory and carries on serving.
curl -s -o nul -w "dashboards   HTTP %%{http_code}\n" http://127.0.0.1:8799/dashboards/index.html
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
echo  itself). With no arguments it decodes the v2 wire format, looks for
echo  the E22 dongle, and falls back to the simulator if there isn't one.
echo.
echo  It opens the dashboard in an Edge/Chrome app-mode window - its own
echo  taskbar entry, not a tab. Add --tab for a normal tab, or
echo  --host 0.0.0.0 to watch from a phone on the same wifi.
echo.
echo  Reading the 1st Bluepill bench spare? That one still speaks the old
echo  format: Telemetry.exe --proto v1
echo.
echo  Logs, CSVs and settings.json are written next to the exe.
echo ====================================================================

endlocal
