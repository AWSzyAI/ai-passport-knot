#!/bin/zsh
# Build KnotReceiver.app (embedded Python via PyInstaller) and dist/KnotReceiver-<version>.dmg.
# Usage: receiver/macos/build_dmg.sh [version]
set -euo pipefail
HERE="${0:A:h}"; RECEIVER="${HERE:h}"; VERSION="${1:-0.1.0}"
WORK="${KNOT_BUILD_DIR:-$HOME/.cache/knot-dmg-build}"   # outside iCloud: no xattrs, no eviction
rm -rf "$WORK" && mkdir -p "$WORK"
cp "$RECEIVER/knot_receiver.py" "$HERE/knot_app.py" "$WORK/"
cd "$WORK"
uv venv -q -p 3.11 .venv
uv pip install -q -p .venv/bin/python -r "$RECEIVER/requirements.txt" pyinstaller
cat > KnotReceiver.spec <<SPEC
from PyInstaller.utils.hooks import collect_all, collect_submodules
datas, binaries, hidden = [], [], collect_submodules("bleak.backends.corebluetooth")
for package in ("mlx", "mlx_whisper", "tiktoken", "tiktoken_ext"):
    d, b, h = collect_all(package); datas += d; binaries += b; hidden += h
# torch is only used by mlx_whisper's model-conversion script, never for transcription.
hidden = [m for m in hidden if m != "mlx_whisper.torch_whisper"]
a = Analysis(["knot_app.py"], datas=datas, binaries=binaries, hiddenimports=hidden,
             excludes=["torch", "mlx_whisper.torch_whisper", "tkinter", "matplotlib", "IPython"])
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="KnotReceiver", console=False)
coll = COLLECT(exe, a.binaries, a.datas, name="KnotReceiver")
app = BUNDLE(coll, name="KnotReceiver.app", bundle_identifier="io.github.knot-notes.receiver",
    version="$VERSION", info_plist={
        "LSUIElement": True,
        "LSMinimumSystemVersion": "12.0",
        "CFBundleShortVersionString": "$VERSION",
        "NSBluetoothAlwaysUsageDescription": "Knot 结绳接收端通过蓝牙接收按键记录并写入 Obsidian 日记。",
        "NSAppleEventsUsageDescription": "用于显示设置对话框。",
    })
SPEC
.venv/bin/pyinstaller -y --log-level WARN KnotReceiver.spec
APP="dist/KnotReceiver.app"
xattr -cr "$APP"
codesign --force --deep --sign - "$APP"
mkdir -p dmgroot && cp -R "$APP" dmgroot/ && ln -s /Applications dmgroot/Applications
OUT="$RECEIVER/../build/KnotReceiver-$VERSION.dmg"
mkdir -p "${OUT:h}"; rm -f "$OUT"
hdiutil create -quiet -volname "Knot $VERSION" -srcfolder dmgroot -format UDZO "$OUT"
shasum -a 256 "$OUT"
