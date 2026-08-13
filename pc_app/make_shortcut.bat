@echo off
REM ====================================================================
REM  Put a Telemetry shortcut on your Desktop.
REM
REM  Run this from a normal Command Prompt in this folder (pc_app):
REM      make_shortcut.bat
REM
REM  build.bat already calls this once the exe is built. Run it again on
REM  its own any time - after rebuilding, or if you deleted the shortcut
REM  and want it back.
REM ====================================================================

setlocal

set "HERE=%~dp0"
set "EXE=%HERE%dist\Telemetry.exe"
set "SHORTCUT=%USERPROFILE%\Desktop\Telemetry.lnk"

if not exist "%EXE%" (
    echo.
    echo dist\Telemetry.exe does not exist yet.
    echo Run build.bat first, then run this script again.
    exit /b 1
)

REM batch can't write a .lnk directly - COM via PowerShell is the standard way.
powershell -NoProfile -Command ^
    "$s = (New-Object -COM WScript.Shell).CreateShortcut('%SHORTCUT%');" ^
    "$s.TargetPath = '%EXE%';" ^
    "$s.WorkingDirectory = '%HERE%dist';" ^
    "$s.IconLocation = '%EXE%,0';" ^
    "$s.Description = 'CAN to LoRa telemetry ground station';" ^
    "$s.Save()"

if errorlevel 1 (
    echo.
    echo Could not create the shortcut. You can still run dist\Telemetry.exe
    echo directly, or copy it to the Desktop yourself - it's a single self
    echo -contained file.
    exit /b 1
)

echo.
echo Created: %SHORTCUT%
echo Double-click it any time - no Command Prompt needed.

endlocal
