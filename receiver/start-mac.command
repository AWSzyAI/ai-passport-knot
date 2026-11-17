#!/bin/zsh
# 结绳接收端 · macOS 调试入口（日常请用 KnotReceiver.app）。在「终端」里运行；首次会请求蓝牙权限。
# 首次配对：设备屏幕显示 Pairing PIN 时，macOS 会弹出蓝牙配对窗口，输入那 6 位数字。
HERE="${0:A:h}"
PY="$HOME/.cache/knot-receiver-venv/bin/python"
if [[ ! -x "$PY" ]]; then
  echo "建立 Mac 专用环境（放在 iCloud 外）…"
  uv venv -q -p 3.11 "$HOME/.cache/knot-receiver-venv" && \
  uv pip install -q -p "$PY" -r "$HERE/requirements.txt" || exit 1
fi
LOG="$HERE/../build/receiver-mac-$(date +%Y%m%d).log"
cd /tmp
# vault comes from ~/Library/Application Support/KnotNotes/config.json, or pass --vault
"$PY" -I -u "$HERE/knot_receiver.py" "$@" 2>&1 | tee -a "$LOG"
