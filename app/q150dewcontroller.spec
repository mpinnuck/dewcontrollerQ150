# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['q150dewcontroller.py'],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='q150dewcontroller',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=True,
    target_arch='arm64',
    codesign_identity=None,
    entitlements_file=None,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='q150dewcontroller',
)

app = BUNDLE(
    coll,
    name='Q150DewController.app',
    icon=None,
    bundle_identifier='com.q150dewcontroller',
    info_plist={
        'CFBundleShortVersionString': '1.0.0',
        'CFBundleVersion': '1.0.0',
        'NSBluetoothAlwaysUsageDescription': 'This app needs Bluetooth access to connect to and control the Q150 Dew Controller device.',
        'NSBluetoothPeripheralUsageDescription': 'This app needs Bluetooth access to connect to and control the Q150 Dew Controller device.',
    },
)
