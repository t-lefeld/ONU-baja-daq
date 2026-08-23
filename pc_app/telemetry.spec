# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec for the ground station.

Build with:   build.bat        (or: pyinstaller --noconfirm --clean telemetry.spec)
Output:       dist/Telemetry.exe

Must be run on Windows. PyInstaller freezes the interpreter and libraries of
the machine it runs on - it cannot cross-compile a Windows .exe from Linux or
macOS.

One-file build: everything unpacks to a temp directory at launch, which is why
telemetry/paths.py distinguishes bundled resources (sys._MEIPASS, deleted on
exit) from writable data (next to the .exe, persistent). Logs, CSVs and
settings.json land beside the executable.
"""

import os

block_cipher = None

a = Analysis(
    ['run.py'],

    # ../simulation is where vehicle_data.py lives. It is not a package and is
    # not importable by default, but the v2 simulator (SimulatorSourceV2 in
    # telemetry/sources.py) borrows its channel table and waveforms rather than
    # keeping a second copy that nothing tests. Putting the directory on pathex
    # lets PyInstaller find the module; hiddenimports below forces it in, since
    # sources.py imports it inside a function and static analysis misses that.
    pathex=[os.path.join(os.path.dirname(os.path.abspath(SPEC)), '..', 'simulation')],

    binaries=[],

    # The dashboard is served from disk at runtime, so it has to be bundled.
    # Forgetting this produces an .exe that starts fine and then serves a 500
    # on the index page - which is why server.py names the missing path in the
    # error text.
    datas=[('static', 'static')],

    hiddenimports=[
        # Imported by name inside pyserial, so static analysis misses it.
        'serial.tools.list_ports',
        # PySide6's __init__ imports this dynamically; PyInstaller's static
        # analysis can miss it depending on hook version.
        'shiboken6',
        # The v2 simulator's data source. Imported lazily inside
        # sources._load_vehicle_data(), so it must be named explicitly here or
        # --sim --proto v2 works from source and fails only in the frozen exe.
        'vehicle_data',
    ],

    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],

    # Trimming dead weight, and - importantly - making sure nothing
    # Chromium-shaped ever ends up in the build. requirements.txt installs
    # PySide6-Essentials (QtCore/QtGui/QtWidgets/...), not the "PySide6"
    # umbrella package, so PySide6-Addons - which contains QtWebEngine, a
    # full embedded Chromium - is never even installed. These excludes are
    # a second layer of defense in case someone's build machine has the
    # full PySide6 installed anyway: if any of these modules are absent,
    # PyInstaller silently skips them, so listing all of them is free.
    excludes=[
        'tkinter', 'matplotlib', 'numpy', 'pandas', 'scipy',
        'PIL', 'PyQt5', 'PyQt6', 'PySide2',
        'pytest', 'setuptools', 'pip',

        # PySide6-Addons modules - none of these are used, and several
        # (QtWebEngine*) embed Chromium, which must never be part of this app.
        'PySide6.QtWebEngineCore', 'PySide6.QtWebEngineWidgets',
        'PySide6.QtWebEngineQuick', 'PySide6.QtPdf', 'PySide6.QtPdfWidgets',
        'PySide6.QtQuick3D', 'PySide6.QtCharts', 'PySide6.QtDataVisualization',
        'PySide6.QtMultimedia', 'PySide6.QtMultimediaWidgets',
        'PySide6.QtBluetooth', 'PySide6.QtNfc', 'PySide6.QtPositioning',
        'PySide6.QtPositioningQuick', 'PySide6.QtLocation', 'PySide6.QtSensors',
        'PySide6.QtSerialBus', 'PySide6.QtSerialPort', 'PySide6.QtWebChannel',
        'PySide6.QtWebSockets', 'PySide6.QtRemoteObjects', 'PySide6.QtScxml',
        'PySide6.QtStateMachine', 'PySide6.QtTextToSpeech',
        'PySide6.QtNetworkAuth', 'PySide6.QtHttpServer',
        'PySide6.QtSpatialAudio', 'PySide6.QtGraphs', 'PySide6.QtGraphsWidgets',
        'PySide6.QtQml', 'PySide6.QtQuick', 'PySide6.QtQuickWidgets',
        'PySide6.QtQuickControls2',
    ],

    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name='Telemetry',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,

    # Keep the console. This is a diagnostic tool: the log tells you which port
    # it opened, whether the dongle is present, and what went wrong. Hiding it
    # would mean a silent failure with nothing to report.
    console=True,

    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,

    # Drop an icon.ico next to this file and it gets picked up.
    icon='icon.ico' if os.path.exists('icon.ico') else None,
)
