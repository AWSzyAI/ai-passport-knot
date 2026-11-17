[English](README.md) · [简体中文](README.zh_CN.md)

# Knot — one-press time bookmarks for FoloToy AI Passport

Press a key on the [FoloToy AI Passport](https://ai-passport.folotoy.cn/) and the exact second is
saved on the device, sent over Bluetooth LE to your computer, and inserted into that day's
Obsidian daily note as a bullet you can write after later:

```markdown
## Log
- 10:40 arrived at the lab
- 2026-11-17 10:53:54 
- 2026-11-17 11:49:03 
```

Offline presses are kept on the device and uploaded later with their original time. Every step
is "store durably, then acknowledge": the device deletes an event only after the computer confirms
the diary write, and retries are de-duplicated, so a break anywhere means a delayed line, never a
lost or doubled one.

## Device

- **OK**: record the current second.
- **UP / DOWN**: page through the on-screen history (32 most recent presses, newest first).
  Green = synced to the computer, yellow = still waiting on the device. On a dark screen the first
  press only wakes it.
- Backlight turns off after 30 s. The power button is a hardware switch the firmware cannot see.

### Flash the firmware

Download `FoloToy-AI-Passport-full.bin` from the Releases page, or build it (below).

- First install / complete refresh: write the merged image at `0x0`
  (`esptool.py --chip esp32c3 write_flash 0x0 FoloToy-AI-Passport-full.bin`). This resets pairing.
- Updates that keep pairing and pending records: `tools/flash-mac.sh <merged.bin> [port]` writes
  only bootloader, partition table and app.

## macOS receiver

1. Download `KnotReceiver-<version>.dmg` from Releases, drag **KnotReceiver** into Applications.
2. The app is ad-hoc signed, not notarized. On first open use right-click → Open, or run
   `xattr -dr com.apple.quarantine /Applications/KnotReceiver.app`.
3. Open it and choose your Obsidian vault. It installs a per-user LaunchAgent: starts at login,
   restarts within 10 s if it ever exits. Allow Bluetooth when asked; when the device shows
   `Pairing PIN`, type those 6 digits into the macOS prompt.
4. Open the app again to see status, change vault, or stop and remove the background service.

Logs: `~/Library/Logs/KnotReceiver/receiver.log`. Config:
`~/Library/Application Support/KnotNotes/config.json`.

## Windows receiver

```powershell
uv venv --python 3.11 .venv
uv pip install --python .venv/Scripts/python.exe -r receiver/requirements.txt
./receiver/start.ps1 -Vault "C:\path\to\vault"
```

The receiver pairs with passkey entry in the console (Bleak's own Windows pairing is Just Works
only, which the firmware rejects). No startup task is installed.

## Where lines go

- Diary path comes from Obsidian's core **Daily notes** settings (`.obsidian/daily-notes.json`,
  folder + moment format); default `YYYY-MM-DD.md` in the vault root.
- Lines are inserted in time order under `## Log` (configurable `heading`), between your
  hand-written `- HH:MM` lines; existing lines are never modified.
- If today's note does not exist yet, nothing is written and the event stays on the device until
  you create the note (so your template is used). Missing past days get a minimal note.
- Presses made before the device ever got the time, and then lost power, go to
  `<folder>/时间待核.md` (time unknown) instead of a guessed date.
- De-duplication ledger: `<folder>/.knot-ledger.tsv` (default folder `Knot`).

Config keys (JSON, all optional except `vault`): `vault`, `folder`, `diary_format`
(moment pattern, e.g. `[Journal]/YYYY/MM/YYYY-MM-DD`), `heading`, `address`.

## How it works

| Layer | Stack |
| --- | --- |
| Firmware | ESP-IDF 5.5.3, NimBLE, LVGL, official AI Passport BSP |
| Storage | 256 KiB NVS partition `knots` @ `0x7c0000`, up to 512 pending events + 32-entry history |
| Time | microsecond monotonic clock + anchor from the computer (3 BLE ping round trips, lowest-latency midpoint); re-synced hourly |
| Link | BLE service `6e410001-b5a3-f393-e0a9-e50e24dcca9e`, LE Secure Connections + MITM, passkey shown on device, bonded |
| Protocol | newline-delimited JSON: `hello` → `ping/pong` ×3 → `sync/synced` → `event` → `ack` (per ID) |
| Receiver | Python 3.11 + Bleak 3 (CoreBluetooth / WinRT); atomic file writes; intent/done ledger |
| macOS app | PyInstaller bundle with `NSBluetoothAlwaysUsageDescription`, LaunchAgent `RunAtLoad` + `KeepAlive` |

Accuracy: the clock source is the computer's system time plus half a BLE round trip (~30 ms), so
seconds are trustworthy; sub-second accuracy is not claimed. If the device is switched off before
it ever synced in that power cycle, those presses cannot be dated.

## Build and test

```bash
bash tools/test_knot.sh          # C model/store tests + receiver tests
bash tools/validate.sh --all     # full gate incl. ESP-IDF 5.5.3 firmware build
receiver/macos/build_dmg.sh 0.1.0
```

ESP-IDF does not accept paths with spaces; build from a copy if needed.

## Origin and license

Fork of [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) (MIT), baseline
`1c9e8b12c9c11a48419ea6c322c8a5d386d8ff22`. The BLE transport (`main/buddy_ble*`, `main/buddy_line*`)
is extracted from the official BLE Buddy reference with a new service UUID and protocol.
Upstream demos remain in the tree for reference. MIT License, see [LICENSE](LICENSE).
