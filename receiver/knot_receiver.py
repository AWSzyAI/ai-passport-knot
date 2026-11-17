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
import shutil
import subprocess
import sys
import tempfile
import threading
import zlib
import base64
import time

SERVICE = "6e410001-b5a3-f393-e0a9-e50e24dcca9e"
RX = "6e410002-b5a3-f393-e0a9-e50e24dcca9e"
TX = "6e410003-b5a3-f393-e0a9-e50e24dcca9e"
CHINA = timezone(timedelta(hours=8))
DEFAULT_FOLDER = "Knot"           # ledger + 时间待核.md, inside the vault
DEFAULT_DIARY = "YYYY-MM-DD"      # Obsidian's own default when Daily notes has no format
DEFAULT_HEADING = "## Log"
LOG = logging.getLogger("knot")
KINDS = {"time", "start", "end", "voice"}
TAGS = {"start": "#START", "end": "#END"}
AUDIO_RATE = 16000
MAX_AUDIO_BYTES = 480000          # 60 s of 4-bit IMA ADPCM at 16 kHz
AUDIO_WINDOW = 48000              # bytes requested per aget
DEFAULT_WHISPER = "mlx-community/whisper-large-v3-turbo"


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
             "utc_ms": message.get("utc_ms"),
             "kind": message.get("kind", "time")}
    if event["utc_ms"] is not None:
        integer(event["utc_ms"], 1704067200000, 4102444800000)
    if event["kind"] not in KINDS:
        raise ProtocolError("Unknown event kind")
    if event["kind"] == "voice":
        event["audio_bytes"] = integer(message.get("audio_bytes"), 0, MAX_AUDIO_BYTES)
        event["audio_crc"] = integer(message.get("audio_crc"), 0, 2**32 - 1)
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
    def __init__(self, vault, folder=DEFAULT_FOLDER, diary_format=None, heading=DEFAULT_HEADING, today=None,
                 attachments=None):
        self.vault = Path(vault).resolve()
        if not self.vault.is_dir():
            raise ValueError("Vault must already exist")
        self.root = (self.vault / folder).resolve()
        if not self.root.is_relative_to(self.vault) or self.root == self.vault:
            raise ValueError("Output must be a subfolder inside the vault")
        self.diary_format = diary_format or detect_daily_notes(self.vault)
        self.heading = heading
        self.attachments = attachments
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

    def is_done(self, message) -> bool:
        event = validate_event(message)
        state = self._ledger().get(f"{event['device']}-{event['seq']:08x}")
        return bool(state and state[0] == "done")

    def save(self, message, text=None):
        """Return the ACK once durably written, or None to leave the event on the device.
        text: what follows the timestamp (voice transcript/embed); default is the kind's tag."""
        event = validate_event(message)
        if text is None:
            text = TAGS.get(event["kind"], "")
        extra = f"{text} " if text else ""
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
            line = f"- 时间待核（设备 {event['device']} #{event['seq']}，第 {event['boot']} 次开机） {extra}"
            if not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                replace_atomically(target, "# 时间待核的结绳记录\n\n设备当时未校时；按下的真实时间无法还原，需要手动回忆。\n\n")
        else:
            target = self.diary_path(when)
            line = f"- {stamp} {extra}"
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
            # Count lines for this second regardless of what the user appended after the stamp.
            prefix = (line[:-len(extra)] if extra else line).rstrip() if when is None else f"- {stamp}"
            written = sum(1 for l in text.splitlines() if l.startswith(prefix))
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


ADPCM_STEPS = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
ADPCM_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_decode(data: bytes):
    """IMA ADPCM, continuous mono stream, low nibble first; mirrors main/knot_adpcm.c."""
    from array import array
    out = array("h", bytes(4 * len(data)))
    predictor, index, n = 0, 0, 0
    steps, adjust = ADPCM_STEPS, ADPCM_INDEX
    for byte in data:
        for code in (byte & 0x0F, byte >> 4):
            step = steps[index]
            diff = step >> 3
            if code & 4: diff += step
            if code & 2: diff += step >> 1
            if code & 1: diff += step >> 2
            predictor = predictor - diff if code & 8 else predictor + diff
            predictor = 32767 if predictor > 32767 else -32768 if predictor < -32768 else predictor
            index += adjust[code & 7]
            index = 0 if index < 0 else 88 if index > 88 else index
            out[n] = predictor; n += 1
    return out


def write_wav(path: Path, pcm, rate=AUDIO_RATE):
    import wave
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1); handle.setsampwidth(2); handle.setframerate(rate)
        handle.writeframes(pcm.tobytes())


def attachment_folder(vault: Path, diary: Path, configured=None) -> Path:
    """Obsidian's 'Default location for new attachments' (or an explicit override)."""
    setting = configured
    if setting is None:
        with contextlib.suppress(OSError, ValueError, AttributeError):
            setting = json.loads((vault / ".obsidian" / "app.json").read_text(encoding="utf-8")).get("attachmentFolderPath")
    setting = (setting or "/").strip()
    if setting in ("/", ""):
        folder = vault
    elif setting == "./" or setting.startswith("./"):
        folder = diary.parent / setting[2:]
    else:
        folder = vault / setting
    folder = folder.resolve()
    if not folder.is_relative_to(vault):
        raise ProtocolError("Attachment folder escapes the vault")
    return folder


HALLUCINATIONS = ("请不吝点赞", "点赞 订阅", "订阅 转发", "打赏支持", "明镜与点点", "字幕由", "Amara.org",
                  "谢谢观看", "感谢观看", "字幕志愿者")


def speech_seconds(audio, rate=AUDIO_RATE, frame_ms=30, margin_db=12.0, floor_abs=0.002):
    """Seconds of frames at least margin_db above the noise floor (20th percentile frame level).
    Key clicks are tens of milliseconds; speech is sustained."""
    import numpy as np
    n = rate * frame_ms // 1000
    frames = len(audio) // n
    if frames == 0:
        return 0.0
    levels = np.sqrt(np.mean(audio[:frames * n].reshape(frames, n) ** 2, axis=1))
    floor = max(float(np.percentile(levels, 20)), floor_abs)
    return float(np.count_nonzero(levels > floor * 10 ** (margin_db / 20))) * frame_ms / 1000


def normalize_speech(audio, target_rms=0.08, min_speech_s=0.6, max_gain=30.0):
    """Bring quiet recordings to a normal level; None if there is no sustained speech to hear."""
    import numpy as np
    if not len(audio) or speech_seconds(audio) < min_speech_s:
        return None
    rms = float(np.sqrt(np.mean(audio * audio)))
    gain = min(max_gain, target_rms / rms)
    return np.clip(audio * gain, -1.0, 1.0).astype(np.float32)


def clean_segments(segments) -> str | None:
    """Drop segments Whisper marks as non-speech and its known subtitle-credit hallucinations."""
    kept = []
    for seg in segments:
        text = (seg.get("text") or "").strip()
        if not text:
            continue
        if seg.get("avg_logprob", 0) < -1.0:      # Whisper itself is guessing
            continue
        if seg.get("no_speech_prob", 0) > 0.6 and seg.get("avg_logprob", 0) < -0.5:
            continue
        if any(marker in text for marker in HALLUCINATIONS):
            continue
        kept.append(text)
    return " ".join(kept).strip() or None


class Transcriber:
    """Local speech-to-text with mlx-whisper on Apple Silicon. Never required: returns None if unavailable."""
    def __init__(self, model=DEFAULT_WHISPER):
        self.model, self.failed, self.lock = model, False, threading.Lock()

    def __call__(self, pcm):
        if self.failed or not len(pcm):
            return None
        with self.lock:
            try:
                import numpy as np
                import mlx_whisper
                audio = np.frombuffer(pcm.tobytes(), dtype=np.int16).astype(np.float32) / 32768.0
                audio = normalize_speech(audio)
                if audio is None:
                    return None  # near silence: Whisper would only hallucinate
                result = mlx_whisper.transcribe(audio, path_or_hf_repo=self.model, language="zh",
                                                initial_prompt="以下是普通话的句子，使用简体中文。",
                                                condition_on_previous_text=False)
                return clean_segments(result.get("segments", []))
            except ImportError:
                LOG.warning("未安装 mlx-whisper，录音不转写")
                self.failed = True
            except Exception as exc:  # model download, Metal, decode errors: keep the audio, skip text
                LOG.warning("转写失败：%s", exc)
            return None

    def warm_up(self):
        LOG.info("加载转写模型 %s（首次需下载约 1.6 GB）", self.model)
        started = time.monotonic()
        silence = __import__("array").array("h", bytes(AUDIO_RATE * 2))
        self(silence)
        if not self.failed:
            LOG.info("转写模型已就绪，用时 %.0f 秒", time.monotonic() - started)


def store_voice(writer, event, pcm, transcriber) -> str:
    """Save the recording next to the vault's attachments and return the diary text for it."""
    when = datetime.fromtimestamp(event["utc_ms"] / 1000, CHINA) if event["utc_ms"] else None
    diary = writer.diary_path(when) if when else writer.root / "时间待核.md"
    folder = attachment_folder(writer.vault, diary, writer.attachments)
    folder.mkdir(parents=True, exist_ok=True)
    stem = f"knot-{when:%Y%m%d-%H%M%S}" if when else "knot-unknown"
    stem += f"-{event['device'][-4:]}-{event['seq']}"
    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / f"{stem}.wav"
        write_wav(wav, pcm)
        target = folder / f"{stem}.m4a"
        converted = sys.platform == "darwin" and subprocess.run(
            ["afconvert", "-f", "m4af", "-d", "aac", "-b", "32000", str(wav), str(target)],
            capture_output=True).returncode == 0
        if not converted:
            target = folder / f"{stem}.wav"
            shutil.copyfile(wav, target)
    text = transcriber(pcm) if transcriber else None
    link = target.relative_to(writer.vault).as_posix()
    return f"🎙 {text} ![[{link}]]" if text else f"🎙 ![[{link}]]"


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


def chunk_for_mtu(mtu) -> int:
    # Each audio line must fit one notification: ~58 bytes of JSON + base64(n) <= mtu - 3.
    return max(48, min(180, ((int(mtu or 23) - 3 - 64) // 4) * 3))


async def fetch_audio(send, incoming, failure, event, chunk):
    """Pull a voice event's audio in windows; re-request gaps; verify CRC-32."""
    total, seq = event["audio_bytes"], event["seq"]
    data, covered = bytearray(total), bytearray(total)
    offset, retries = 0, 0
    while offset < total:
        end = min(total, offset + AUDIO_WINDOW)
        await send({"type": "aget", "seq": seq, "off": offset, "len": end - offset, "n": chunk})
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and not failure.is_set():
            try:
                message = await asyncio.wait_for(incoming.get(), 0.5)
            except asyncio.TimeoutError:
                continue
            kind = message.get("type")
            if kind == "a" and message.get("seq") == seq:
                start = integer(message.get("off"), 0, total)
                try:
                    piece = base64.b64decode(message.get("d", ""), validate=True)
                except (ValueError, TypeError) as exc:
                    raise ProtocolError("Bad audio chunk") from exc
                if start + len(piece) > total:
                    raise ProtocolError("Audio chunk out of range")
                data[start:start + len(piece)] = piece
                covered[start:start + len(piece)] = b"\x01" * len(piece)
            elif kind == "aend" and message.get("seq") == seq:
                break
        if failure.is_set():
            raise OSError("Disconnected during audio transfer")
        gap = covered.find(0, offset, end)
        if gap < 0:
            offset, retries = end, 0
        else:
            retries += 1
            if retries > 5:
                raise TimeoutError("Audio transfer incomplete; device keeps the recording")
            offset = gap
    if zlib.crc32(data) != event["audio_crc"]:
        raise ProtocolError("Audio CRC mismatch; device keeps the recording")
    return bytes(data)


async def session(device, writer, expected_id=None, transcriber=None):
    from bleak import BleakClient
    incoming = asyncio.Queue(maxsize=8192)  # audio arrives as thousands of short lines
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
            event = validate_event(message)
            if event["kind"] == "voice" and not await asyncio.to_thread(writer.is_done, message):
                if event["audio_bytes"]:
                    LOG.info("接收录音 #%s（%.0f 秒）", event["seq"], event["audio_bytes"] / (AUDIO_RATE / 2))
                    audio = await fetch_audio(send, incoming, failure, event,
                                              chunk_for_mtu(getattr(client, "mtu_size", 23)))
                    pcm = adpcm_decode(audio)
                    text = await asyncio.to_thread(store_voice, writer, event, pcm, transcriber)
                else:
                    text = "🎙（录音中断，没有音频）"
                ack = await asyncio.to_thread(writer.save, message, text)
            else:
                ack = await asyncio.to_thread(writer.save, message)
            if ack is None:
                continue  # not written; the device keeps it and retransmits
            await send(ack)
            LOG.info("已写入 #%s；已确认本机保存，iCloud 状态未知", message["seq"])


async def run(args):
    from bleak import BleakScanner
    writer = Writer(args.vault, args.folder, args.diary_format, args.heading, attachments=args.attachments)
    transcriber = Transcriber(args.whisper_model) if args.transcribe else None
    if transcriber:  # download/load the model now, not while a recording waits
        threading.Thread(target=transcriber.warm_up, daemon=True).start()
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
            await session(devices[0], writer, args.device_id, transcriber)
            LOG.info("设备已断开，重新扫描")
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
    parser.add_argument("--no-transcribe", action="store_true", help="keep voice memos as audio only")
    args = parser.parse_args(argv)
    config = load_config(args.config)
    for key, default in (("vault", None), ("folder", DEFAULT_FOLDER), ("diary_format", None),
                         ("heading", DEFAULT_HEADING), ("address", None), ("device_id", None)):
        if getattr(args, key) is None:
            setattr(args, key, config.get(key, default))
    args.attachments = config.get("attachments")
    args.whisper_model = config.get("whisper_model", DEFAULT_WHISPER)
    args.transcribe = bool(config.get("transcribe", True)) and not args.no_transcribe
    if not args.vault:
        parser.error(f"no vault: pass --vault or set \"vault\" in {args.config or config_path()}")
    return args


def main(argv=None):
    args = parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    for noisy in ("httpx", "huggingface_hub", "filelock"):
        logging.getLogger(noisy).setLevel(logging.WARNING)
    os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
    with InstanceLock(args.vault):
        try:
            asyncio.run(run(args))
        except KeyboardInterrupt:
            LOG.info("接收已停止；设备未确认的记录保留")


if __name__ == "__main__":
    main()
