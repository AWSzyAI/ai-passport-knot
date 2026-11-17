"""Knot Notes Windows BLE receiver. No cloud service or Obsidian plugin required."""
from __future__ import annotations

import argparse
import asyncio
import contextlib
from datetime import datetime, timedelta, timezone
import hashlib
import json
import logging
import os
from pathlib import Path
import re
import sys
import tempfile
import time

SERVICE = "6e410001-b5a3-f393-e0a9-e50e24dcca9e"
RX = "6e410002-b5a3-f393-e0a9-e50e24dcca9e"
TX = "6e410003-b5a3-f393-e0a9-e50e24dcca9e"
CHINA = timezone(timedelta(hours=8))
DEFAULT_FOLDER = "Knot"           # ledger + 时间待核.md, inside the vault
DEFAULT_DIARY = "YYYY-MM-DD"      # Obsidian's own default when Daily notes has no format
DEFAULT_HEADING = "## Log"
LOG = logging.getLogger("knot")


class ProtocolError(ValueError):
    pass


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ProtocolError("Invalid integer field")
    return value


def validate_event(message):
    if message.get("v") != 1 or message.get("type") != "event":
        raise ProtocolError("Unsupported event protocol")
    device = message.get("device", "")
    if not isinstance(device, str) or not re.fullmatch(r"[0-9a-f]{12}", device):
        raise ProtocolError("Invalid device identity")
    event = {"v": 1, "type": "event", "device": device,
             "seq": integer(message.get("seq"), 1, 2**32-2),
             "boot": integer(message.get("boot"), 1, 2**32-1),
             "mono_us": integer(message.get("mono_us"), 0, 2**53-1),
             "utc_ms": message.get("utc_ms")}
    if event["utc_ms"] is not None:
        integer(event["utc_ms"], 1704067200000, 4102444800000)
    return event


def encode(message):
    return (json.dumps({"v": 1, **message}, separators=(",", ":")) + "\n").encode()


class Decoder:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        if len(self.buffer) > 8192:
            self.buffer.clear()
            raise ProtocolError("Notification buffer overflow")
        result = []
        while b"\n" in self.buffer:
            line, _, rest = self.buffer.partition(b"\n")
            self.buffer[:] = rest
            if len(line) > 1024:
                raise ProtocolError("Message too large")
            try:
                message = json.loads(line)
            except (ValueError, UnicodeError) as exc:
                raise ProtocolError("Malformed notification") from exc
            if not isinstance(message, dict) or message.get("v") != 1:
                raise ProtocolError("Unsupported message")
            result.append(message)
        return result


def publish_new(path: Path, content: str):
    """Publish a fully fsynced file without ever replacing an existing note."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".knot-", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        if os.name == "nt":
            os.rename(temporary, path)  # Windows rename fails if destination exists.
        else:
            os.link(temporary, path)  # Atomic, no-replace publication on test hosts.
            os.unlink(temporary)
            directory = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)


LOG_ITEM = re.compile(r"^- (?:\d{4}-\d{2}-\d{2} )?(\d{1,2}):(\d{2})(?::(\d{2}))?(?!\d)")


def replace_atomically(path: Path, content: str):
    """Rewrite a note via fsynced temp file + rename; readers never see a half-written file."""
    fd, temporary = tempfile.mkstemp(prefix=".knot-", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)


def insert_log_line(text: str, line: str, seconds_of_day: int, heading: str = DEFAULT_HEADING) -> str:
    """Insert one bullet into the '## Log' section in time order. Existing lines are never edited."""
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    start = next((i for i, l in enumerate(lines) if l.strip() == heading), None)
    if start is None:
        think = next((i for i, l in enumerate(lines) if l.strip() == "## Think"), None)
        if think is None:
            while lines and lines[-1] == "":
                lines.pop()
            lines += ["", heading, "", line, ""]
        else:
            lines[think:think] = [heading, "", line, ""]
        return newline.join(lines)
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    position = None
    last_item = None
    for i in range(start + 1, end):
        if lines[i].startswith("- "):
            last_item = i
            match = LOG_ITEM.match(lines[i])
            if match:
                h, m, sec = int(match[1]), int(match[2]), int(match[3] or 0)
                if (h * 3600 + m * 60 + sec) > seconds_of_day and position is None:
                    position = i
    if position is None:
        if last_item is not None and lines[last_item].strip() == "-":
            position = last_item  # keep the empty placeholder bullet last
        elif last_item is not None:
            position = last_item + 1
        else:
            position = end
            while position - 1 > start and lines[position - 1].strip() == "":
                position -= 1
            if position == start + 1:  # bare heading: keep one blank line under it
                lines.insert(position, "")
                position += 1
    lines.insert(position, line)
    return newline.join(lines)


MINIMAL_DIARY = "---\ncreated: {day}\n---\n\n{heading}\n\n"


MOMENT_TOKEN = re.compile(r"\[([^\]]*)\]|YYYY|YY|MM|M|DD|D")


def render_moment(pattern: str, day) -> str:
    """Render the subset of moment.js tokens Obsidian daily-note formats commonly use."""
    values = {"YYYY": f"{day.year:04d}", "YY": f"{day.year % 100:02d}", "MM": f"{day.month:02d}",
              "M": str(day.month), "DD": f"{day.day:02d}", "D": str(day.day)}
    return MOMENT_TOKEN.sub(lambda m: m.group(1) if m.group(1) is not None else values[m.group(0)], pattern)


def detect_daily_notes(vault: Path) -> str:
    """Folder + format from Obsidian's core Daily notes settings, as one moment pattern."""
    with contextlib.suppress(OSError, ValueError, AttributeError):
        settings = json.loads((vault / ".obsidian" / "daily-notes.json").read_text(encoding="utf-8"))
        fmt = (settings.get("format") or DEFAULT_DIARY).strip()
        folder = (settings.get("folder") or "").strip().strip("/")
        return f"[{folder}]/{fmt}" if folder else fmt
    return DEFAULT_DIARY


class Writer:
    """Writes each press as '- YYYY-MM-DD HH:MM:SS ' into that day's diary Log section.

    A hidden ledger (.knot-ledger.tsv, synced with the vault) makes retries idempotent:
    'intent' is logged before the diary write and 'done' after it, so a crash in between is
    resolved by checking whether the line already exists.
    """
    def __init__(self, vault, folder=DEFAULT_FOLDER, diary_format=None, heading=DEFAULT_HEADING, today=None):
        self.vault = Path(vault).resolve()
        if not self.vault.is_dir():
            raise ValueError("Vault must already exist")
        self.root = (self.vault / folder).resolve()
        if not self.root.is_relative_to(self.vault) or self.root == self.vault:
            raise ValueError("Output must be a subfolder inside the vault")
        self.diary_format = diary_format or detect_daily_notes(self.vault)
        self.heading = heading
        self.ledger = self.root / ".knot-ledger.tsv"
        self.today = today or (lambda: datetime.now(CHINA).date())
        self.waiting = set()

    def diary_path(self, when):
        path = (self.vault / (render_moment(self.diary_format, when.date()) + ".md")).resolve()
        if not path.is_relative_to(self.vault):
            raise ProtocolError("Diary path escapes the vault")
        return path

    def _ledger(self):
        state = {}
        if self.ledger.exists():
            for row in self.ledger.read_text(encoding="utf-8").splitlines():
                parts = row.split("\t")
                if len(parts) >= 3 and parts[0] in ("intent", "done"):
                    if parts[0] == "done" or parts[1] not in state:
                        state[parts[1]] = (parts[0], parts[2])
        return state

    def _append_ledger(self, kind, identity, stamp):
        self.root.mkdir(parents=True, exist_ok=True)
        with self.ledger.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(f"{kind}\t{identity}\t{stamp}\n")
            handle.flush()
            os.fsync(handle.fileno())

    def save(self, message):
        """Return the ACK once durably written, or None to leave the event on the device."""
        event = validate_event(message)
        identity = f"{event['device']}-{event['seq']:08x}"
        ack = {"type": "ack", "device": event["device"], "seq": event["seq"]}
        when = None if event["utc_ms"] is None else datetime.fromtimestamp(event["utc_ms"] / 1000, CHINA)
        stamp = f"{when:%Y-%m-%d %H:%M:%S}" if when else "时间待核"
        state = self._ledger()
        previous = state.get(identity)
        if previous and previous[1] != stamp:
            raise ProtocolError(f"Conflicting event ID: {identity}")
        if previous and previous[0] == "done":
            return ack
        if when is None:
            target = self.root / "时间待核.md"
            line = f"- 时间待核（设备 {event['device']} #{event['seq']}，第 {event['boot']} 次开机） "
            if not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                replace_atomically(target, "# 时间待核的结绳记录\n\n设备当时未校时；按下的真实时间无法还原，需要手动回忆。\n\n")
        else:
            target = self.diary_path(when)
            line = f"- {stamp} "
            if not target.exists():
                if when.date() >= self.today():
                    if identity not in self.waiting:
                        LOG.info("%s 的日记还没建立；记录留在设备里，建好后自动补入", f"{when:%Y-%m-%d}")
                        self.waiting.add(identity)
                    return None
                target.parent.mkdir(parents=True, exist_ok=True)
                replace_atomically(target, MINIMAL_DIARY.format(day=f"{when:%Y-%m-%d}", heading=self.heading))
        text = target.read_text(encoding="utf-8")
        if previous:  # crashed between intent and done: was the line already written?
            written = sum(1 for l in text.splitlines() if l.rstrip() == line.rstrip() or l.startswith(line))
            done_same = sum(1 for k, (kind, st) in state.items() if kind == "done" and st == stamp)
            if written > done_same:
                self._append_ledger("done", identity, stamp)
                return ack
        else:
            self._append_ledger("intent", identity, stamp)
        if when is None:
            updated = text + ("" if text.endswith("\n") else "\n") + line + "\n"
        else:
            updated = insert_log_line(text, line, when.hour * 3600 + when.minute * 60 + when.second, self.heading)
        replace_atomically(target, updated)
        self._append_ledger("done", identity, stamp)
        self.waiting.discard(identity)
        return ack


class InstanceLock:
    """OS lock lives outside iCloud, and releases automatically on process exit."""
    def __init__(self, vault):
        if os.name == "nt":
            base = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir()))
        elif sys.platform == "darwin":  # same path for Terminal runs and the launchd agent
            base = Path.home() / "Library" / "Application Support"
        else:
            base = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state"))
        state = Path(os.environ.get("KNOT_STATE_DIR", base)) / "KnotNotes"
        state.mkdir(parents=True, exist_ok=True)
        key = hashlib.sha256(str(Path(vault).resolve()).casefold().encode()).hexdigest()[:20]
        self.path = state / f"{key}.lock"

    def __enter__(self):
        self.handle = self.path.open("a+b")
        self.handle.seek(0, os.SEEK_END)
        if self.handle.tell() == 0:
            self.handle.write(b"0"); self.handle.flush()
        self.handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(self.handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.handle.close()
            raise RuntimeError("A receiver for this vault is already running")
        return self

    def __exit__(self, *_):
        self.handle.close()


async def sync_clock(send, receive):
    samples = []
    for token in range(3):
        start_utc = time.time_ns() // 1000000
        start = time.monotonic_ns()
        await send({"type": "ping", "token": token})
        pong = await receive("pong")
        elapsed = (time.monotonic_ns() - start) / 1e6
        if pong.get("token") != token or elapsed > 1000:
            raise ProtocolError("Clock probe mismatch or excessive delay")
        mono = integer(pong.get("mono_us"), 0, 2**53-1)
        samples.append((elapsed, mono, start_utc + round(elapsed / 2)))
    elapsed, mono, utc = min(samples)
    await send({"type": "sync", "mono_us": mono, "utc_ms": utc})
    await receive("synced")
    LOG.info("时钟已校准；最短往返 %.1f ms（不是准确度保证）", elapsed)



def normalize_pin(text):
    """Return the 6-digit passkey shown on the device, or None if the input is unusable."""
    digits = "".join(ch for ch in str(text) if ch.isdigit())
    if not 1 <= len(digits) <= 6:
        return None
    return digits.zfill(6)


def ask_pin(prompt=input):
    for _ in range(3):
        pin = normalize_pin(prompt("请输入设备屏幕上 Pairing PIN 的 6 位数字，回车确认："))
        if pin:
            return pin
        print("需要 6 位数字，请重输。")
    return None


async def ensure_paired_windows(device):
    """Pair with passkey entry. Bleak 3.0's WinRT pair() only offers Just Works, which the
    firmware rejects (display-only + MITM + Secure Connections), giving Insufficient Authentication."""
    from winrt.windows.devices.bluetooth import BluetoothLEDevice
    from winrt.windows.devices.enumeration import (
        DevicePairingKinds, DevicePairingProtectionLevel, DevicePairingResultStatus)
    address = int(device.address.replace(":", ""), 16)
    ble = await BluetoothLEDevice.from_bluetooth_address_async(address)
    if ble is None:
        raise OSError("Windows 找不到该蓝牙设备")
    try:
        pairing = ble.device_information.pairing
        if pairing.is_paired:
            return
        if not pairing.can_pair:
            raise OSError("Windows 报告该设备不可配对")
        custom = pairing.custom

        def requested(_sender, args):
            deferral = args.get_deferral()
            try:
                if args.pairing_kind == DevicePairingKinds.PROVIDE_PIN:
                    pin = ask_pin()
                    if pin:
                        args.accept(pin)
                elif args.pairing_kind == DevicePairingKinds.CONFIRM_ONLY:
                    args.accept()
                else:
                    LOG.warning("意外的配对方式 %s，已拒绝", args.pairing_kind)
            finally:
                deferral.complete()

        token = custom.add_pairing_requested(requested)
        try:
            LOG.info("首次配对：设备屏幕将显示 6 位 PIN")
            result = await custom.pair_async(
                DevicePairingKinds.PROVIDE_PIN | DevicePairingKinds.CONFIRM_ONLY,
                DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION)
        finally:
            custom.remove_pairing_requested(token)
        if result.status not in (DevicePairingResultStatus.PAIRED,
                                 DevicePairingResultStatus.ALREADY_PAIRED):
            raise OSError(f"配对失败：{result.status.name}")
        LOG.info("配对成功，Windows 已保存绑定")
    finally:
        ble.close()


async def session(device, writer, expected_id=None):
    from bleak import BleakClient
    incoming = asyncio.Queue(maxsize=128)
    decoder = Decoder()
    failure = asyncio.Event()

    def notification(_characteristic, data):
        try:
            for message in decoder.feed(data):
                incoming.put_nowait(message)
        except (ProtocolError, asyncio.QueueFull):
            failure.set()

    if sys.platform == "win32":
        await ensure_paired_windows(device)
    # Pairing is done above (or by the OS on other platforms); Bleak's own pair() is Just Works only.
    async with BleakClient(device, pair=sys.platform != "win32", timeout=60,
                           disconnected_callback=lambda _: failure.set()) as client:
        await client.start_notify(TX, notification)
        characteristic = client.services.get_characteristic(RX)
        if characteristic is None:
            raise ProtocolError("Missing receiver characteristic")

        async def send(message):
            payload = encode(message)
            size = characteristic.max_write_without_response_size
            for start in range(0, len(payload), size):
                await client.write_gatt_char(characteristic, payload[start:start+size], response=False)

        async def receive(kind):
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline and not failure.is_set():
                try:
                    message = await asyncio.wait_for(incoming.get(), 0.5)
                except asyncio.TimeoutError:
                    continue
                if message.get("type") == kind:
                    return message
                # Before sync, stale transmissions are ignored, never acknowledged.
            raise TimeoutError(f"Waiting for {kind}; device will retain pending events")

        await send({"type": "hello"})
        hello = await receive("hello")
        identity = hello.get("device", "")
        if not isinstance(identity, str) or not re.fullmatch(r"[0-9a-f]{12}", identity):
            raise ProtocolError("Invalid hello identity")
        if expected_id and identity != expected_id:
            raise ProtocolError("Connected device does not match selected identity")
        await sync_clock(send, receive)
        LOG.info("已连接 %s，待传 %s 条", identity, hello.get("pending"))
        # Reconnect hourly to renew the clock with the same handshake.
        deadline = time.monotonic() + 3600
        while client.is_connected and not failure.is_set() and time.monotonic() < deadline:
            try:
                message = await asyncio.wait_for(incoming.get(), 1)
            except asyncio.TimeoutError:
                continue
            if message.get("type") != "event":
                continue
            if message.get("device") != identity:
                raise ProtocolError("Event identity changed")
            ack = await asyncio.to_thread(writer.save, message)
            if ack is None:
                continue  # not written; the device keeps it and retransmits
            await send(ack)
            LOG.info("已写入 #%s；已确认本机保存，iCloud 状态未知", message["seq"])


async def run(args):
    from bleak import BleakScanner
    writer = Writer(args.vault, args.folder, args.diary_format, args.heading)
    retry = 1
    while True:
        try:
            LOG.info("扫描结绳设备；Ctrl+C 停止")
            found = await BleakScanner.discover(timeout=5, service_uuids=[SERVICE])
            devices = [d for d in found if (d.name or "").startswith("Knot-") and
                       (not args.address or d.address.casefold() == args.address.casefold())]
            if len(devices) > 1:
                raise ProtocolError("Multiple devices: use --address to select one")
            if not devices:
                await asyncio.sleep(5)
                continue
            await session(devices[0], writer, args.device_id)
            retry = 1
        except (OSError, ProtocolError, TimeoutError) as exc:
            LOG.warning("%s；%s 秒后重试", exc, retry)
            await asyncio.sleep(retry)
            retry = min(30, retry * 2)
        except Exception as exc:
            # BLE backend errors are platform-specific. Do not acknowledge failed writes.
            LOG.warning("蓝牙或文件操作失败：%s；%s 秒后重试", exc, retry)
            await asyncio.sleep(retry)
            retry = min(30, retry * 2)


def config_path() -> Path:
    if os.name == "nt":
        return Path(os.environ.get("APPDATA", Path.home())) / "KnotNotes" / "config.json"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "KnotNotes" / "config.json"
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "knot-notes" / "config.json"


def load_config(path: Path | None = None) -> dict:
    path = path or config_path()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return data


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, help="JSON with vault/folder/diary_format/heading/address")
    parser.add_argument("--vault", help="Obsidian vault folder")
    parser.add_argument("--folder", help=f"vault subfolder for the ledger (default {DEFAULT_FOLDER})")
    parser.add_argument("--diary-format", help="moment-style daily note path; default: read Obsidian Daily notes")
    parser.add_argument("--heading", help=f"section to insert into (default {DEFAULT_HEADING!r})")
    parser.add_argument("--address")
    parser.add_argument("--device-id")
    args = parser.parse_args(argv)
    config = load_config(args.config)
    for key, default in (("vault", None), ("folder", DEFAULT_FOLDER), ("diary_format", None),
                         ("heading", DEFAULT_HEADING), ("address", None), ("device_id", None)):
        if getattr(args, key) is None:
            setattr(args, key, config.get(key, default))
    if not args.vault:
        parser.error(f"no vault: pass --vault or set \"vault\" in {args.config or config_path()}")
    return args


def main(argv=None):
    args = parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    with InstanceLock(args.vault):
        try:
            asyncio.run(run(args))
        except KeyboardInterrupt:
            LOG.info("接收已停止；设备未确认的记录保留")


if __name__ == "__main__":
    main()
