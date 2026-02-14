# -*- mode: python ; coding: utf-8 -*-

# Read version from source file
import re
with open('q150dewcontroller.py', 'r', encoding='utf-8') as f:
    version_match = re.search(r'^VERSION\s*=\s*[\'"]([^\'"]+)[\'"]', f.read(), re.MULTILINE)
    VERSION = version_match.group(1) if version_match else '1.0.0'

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
    a.binaries,
    a.datas,
    [],
    name='q150dewcontroller',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=None,
)
