#!/usr/bin/env bash
# 从 Mac 分段刷机：只写 bootloader(0x0)、分区表(0x8000)、应用(0x10000)，
# 不碰 0x9000 配对分区和 0x7c0000 记录分区。用法：flash-mac.sh <merged.bin> [串口]
set -euo pipefail
bin="${1:?merged firmware .bin}"; port="${2:-$(ls /dev/cu.usbmodem* | head -1)}"
work="$(mktemp -d)"
dd if="$bin" of="$work/boot.bin" bs=4096 count=8 2>/dev/null
dd if="$bin" of="$work/pt.bin" bs=4096 skip=8 count=1 2>/dev/null
dd if="$bin" of="$work/app.bin" bs=4096 skip=16 2>/dev/null
python3 -c "import sys;d=open(sys.argv[1],'rb').read();assert d[0x8000:0x8002]==b'\xaa\x50','no partition table';assert set(d[0x9000:0x10000])=={255},'gap not empty'" "$bin"
shasum -a 256 "$bin"
python -m esptool --chip esp32c3 -p "$port" -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 "$work/boot.bin" 0x8000 "$work/pt.bin" 0x10000 "$work/app.bin"
