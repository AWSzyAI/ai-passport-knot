import asyncio
from datetime import date, datetime, timedelta, timezone
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import knot_receiver
from knot_receiver import Decoder, InstanceLock, ProtocolError, Writer, ask_pin, encode, normalize_pin, session


def event(seq=1, utc_ms=1794844800123):
    return dict(v=1, type="event", device="abcdef123456", seq=seq, boot=2,
                mono_us=1000000, utc_ms=utc_ms)


DIARY = """---
type: journal
---

## Log

<!-- HH:mm 做了什么；时间不记得就不补。 -->
- 10:40 到达工位，日光浴中。
- 11:43 配置工作站ing
- 

## Think
"""


def china(h, m, s, day=17):
    return int(datetime(2026, 11, day, h, m, s, tzinfo=timezone(timedelta(hours=8))).timestamp() * 1000)


class WriterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.today = date(2026, 11, 17)
        self.writer = Writer(self.tmp.name, "结绳", "[日志]/YYYY/MM/YYYY-MM-DD", today=lambda: self.today)
        self.diary = Path(self.tmp.name) / "日志/2026/11/2026-11-17.md"
        self.diary.parent.mkdir(parents=True)
        self.diary.write_text(DIARY, encoding="utf-8")

    def log(self):
        text = self.diary.read_text(encoding="utf-8")
        return [l for l in text.split("## Log")[1].split("## Think")[0].splitlines() if l.startswith("- ")]

    def test_inserted_in_time_order_with_full_datetime(self):
        self.writer.save(event(1, china(10, 58, 3)))
        self.writer.save(event(2, china(9, 5, 0)))
        self.writer.save(event(3, china(12, 0, 59)))
        self.assertEqual(self.log(), ["- 2026-11-17 09:05:00 ", "- 10:40 到达工位，日光浴中。",
                                      "- 2026-11-17 10:58:03 ", "- 11:43 配置工作站ing",
                                      "- 2026-11-17 12:00:59 ", "- "])

    def test_retry_is_idempotent_and_keeps_user_text(self):
        ack = self.writer.save(event(1, china(11, 0, 0)))
        text = self.diary.read_text(encoding="utf-8").replace("- 2026-11-17 11:00:00 ", "- 2026-11-17 11:00:00 我的思考")
        self.diary.write_text(text, encoding="utf-8")
        self.assertEqual(Writer(self.tmp.name, "结绳", "[日志]/YYYY/MM/YYYY-MM-DD", today=lambda: self.today).save(event(1, china(11, 0, 0))), ack)
        self.assertIn("- 2026-11-17 11:00:00 我的思考", self.log())
        self.assertEqual(sum(l.startswith("- 2026-11-17 11:00:00") for l in self.log()), 1)

    def test_same_second_is_two_lines(self):
        self.writer.save(event(1, china(11, 0, 0))); self.writer.save(event(2, china(11, 0, 0)))
        self.assertEqual(sum(l.startswith("- 2026-11-17 11:00:00") for l in self.log()), 2)

    def test_crash_between_intent_and_done(self):
        with patch("knot_receiver.replace_atomically", side_effect=OSError("disk")):
            with self.assertRaises(OSError): self.writer.save(event(1, china(11, 0, 0)))
        self.assertEqual(len(self.log()), 3)
        self.writer.save(event(1, china(11, 0, 0)))
        self.assertEqual(len(self.log()), 4)
        # Crash after writing the line but before 'done': retry must not duplicate it.
        self.writer._append_ledger("intent", "abcdef123456-00000002", "2026-11-17 11:30:00")
        self.diary.write_text(self.diary.read_text(encoding="utf-8").replace("- \n\n## Think", "- 2026-11-17 11:30:00 \n- \n\n## Think"), encoding="utf-8")
        self.assertIsNotNone(self.writer.save(event(2, china(11, 30, 0))))
        self.assertEqual(sum(l.startswith("- 2026-11-17 11:30:00") for l in self.log()), 1)

    def test_unknown_does_not_use_upload_time(self):
        self.writer.save(event(utc_ms=None))
        self.assertIn("时间待核", (self.writer.root / "时间待核.md").read_text(encoding="utf-8"))
        self.assertEqual(len(self.log()), 3)

    def test_today_without_diary_waits_past_day_gets_minimal(self):
        self.today = date(2026, 11, 18)
        self.assertIsNone(self.writer.save(event(1, china(8, 0, 0, day=18))))
        self.assertFalse((Path(self.tmp.name) / "日志/2026/11/2026-11-18.md").exists())
        self.assertIsNotNone(self.writer.save(event(2, china(23, 59, 59, day=16))))
        past = (Path(self.tmp.name) / "日志/2026/11/2026-11-16.md").read_text(encoding="utf-8")
        self.assertIn("## Log\n\n- 2026-11-16 23:59:59 ", past)

    def test_detects_obsidian_daily_notes(self):
        vault = Path(self.tmp.name)
        (vault / ".obsidian").mkdir()
        (vault / ".obsidian/daily-notes.json").write_text('{"folder": "Journal/", "format": "YYYY/[W]MM/D"}', encoding="utf-8")
        writer = Writer(vault, "Knot", today=lambda: self.today)
        self.assertEqual(writer.diary_path(datetime(2026, 3, 5, tzinfo=timezone.utc)), (vault / "Journal/2026/W03/5.md").resolve())
        (vault / ".obsidian/daily-notes.json").write_text('{}', encoding="utf-8")
        self.assertEqual(Writer(vault, "Knot").diary_path(datetime(2026, 3, 5, tzinfo=timezone.utc)).name, "2026-03-05.md")
        with self.assertRaises(ProtocolError):
            Writer(vault, "Knot", "[../x]/YYYY").diary_path(datetime(2026, 3, 5, tzinfo=timezone.utc))

    def test_config_and_cli(self):
        cfg = Path(self.tmp.name) / "c.json"
        cfg.write_text(json.dumps({"vault": self.tmp.name, "folder": "K", "heading": "## 日志"}), encoding="utf-8")
        args = knot_receiver.parse_args(["--config", str(cfg), "--folder", "CLI"])
        self.assertEqual((args.vault, args.folder, args.heading, args.diary_format), (self.tmp.name, "CLI", "## 日志", None))

    def test_china_midnight(self):
        (Path(self.tmp.name) / "日志/2026/11/2026-11-16.md").write_text(DIARY, encoding="utf-8")
        before = int(datetime(2026,11,16,15,59,59,tzinfo=timezone.utc).timestamp()*1000)
        self.writer.save(event(1,before)); self.writer.save(event(2,before+1000))
        self.assertIn("- 2026-11-16 23:59:59 ", (Path(self.tmp.name)/"日志/2026/11/2026-11-16.md").read_text(encoding="utf-8"))
        self.assertIn("- 2026-11-17 00:00:00 ", self.log())

    def test_diary_without_log_section(self):
        self.diary.write_text("# 日记\n\n## Think\n想法\n", encoding="utf-8")
        self.writer.save(event(1, china(11, 0, 0)))
        self.assertEqual(self.diary.read_text(encoding="utf-8"),
                         "# 日记\n\n## Log\n\n- 2026-11-17 11:00:00 \n\n## Think\n想法\n")

    def test_conflicting_replay_never_overwrites(self):
        self.writer.save(event(1, china(11, 0, 0)))
        with self.assertRaises(ProtocolError):
            self.writer.save(event(1, china(11, 0, 1)))

    def test_validation(self):
        for bad in [dict(event(),device="../escape"), dict(event(),seq=True),
                    dict(event(),utc_ms=-1),dict(event(),mono_us=2**60)]:
            with self.assertRaises(ProtocolError): self.writer.save(bad)
        with self.assertRaises(ValueError): Writer(self.tmp.name, "../escape")

    def test_exclusive_instance(self):
        os.environ['KNOT_STATE_DIR']=self.tmp.name
        with InstanceLock(self.tmp.name):
            with self.assertRaises(RuntimeError):
                with InstanceLock(self.tmp.name): pass


class VoiceAndTagTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.vault = Path(self.tmp.name)
        (self.vault / ".obsidian").mkdir()
        (self.vault / ".obsidian/app.json").write_text('{"attachmentFolderPath": "a"}', encoding="utf-8")
        self.diary = self.vault / "日志/2026/11/2026-11-17.md"
        self.diary.parent.mkdir(parents=True); self.diary.write_text(DIARY, encoding="utf-8")
        self.writer = Writer(self.vault, "结绳", "[日志]/YYYY/MM/YYYY-MM-DD", today=lambda: date(2026, 11, 17))

    def test_tags(self):
        self.writer.save(dict(event(1, china(11, 0, 0)), kind="start"))
        self.writer.save(dict(event(2, china(11, 30, 0)), kind="end"))
        text = self.diary.read_text(encoding="utf-8")
        self.assertIn("- 2026-11-17 11:00:00 #START \n", text)
        self.assertIn("- 2026-11-17 11:30:00 #END \n", text)
        with self.assertRaises(ProtocolError): self.writer.save(dict(event(3), kind="bogus"))
        with self.assertRaises(ProtocolError): self.writer.save(dict(event(3), kind="voice", audio_bytes=10**7, audio_crc=0))

    def test_adpcm_matches_firmware_encoder(self):
        import math, os
        vector = os.environ.get("KNOT_ADPCM_VECTOR")
        if not vector or not Path(vector).exists():
            self.skipTest("run via tools/test_knot.sh to get the C-encoded vector")
        pcm = knot_receiver.adpcm_decode(Path(vector).read_bytes())
        ref = [8000 * math.sin(2 * math.pi * 440 * i / 16000) + 3000 * math.sin(2 * math.pi * 1700 * i / 16000) for i in range(16000)]
        sig = sum(int(r) ** 2 for r in ref[200:]); err = sum((int(r) - p) ** 2 for r, p in zip(ref[200:], pcm[200:]))
        self.assertGreater(10 * math.log10(sig / err), 20)

    def test_chunk_fits_mtu(self):
        for mtu in (23, 185, 247, 512):
            n = knot_receiver.chunk_for_mtu(mtu)
            line = encode(dict(type="a", seq=4294967294, off=479999, d="x" * (4 * -(-n // 3))))
            self.assertTrue(48 <= n <= 180)
            if mtu >= 185: self.assertLessEqual(len(line), mtu - 3)

    def run_voice(self, audio, crc=None, drop_first=False, bytes_=None):
        import zlib, base64
        crc = zlib.crc32(audio) if crc is None else crc
        msg = dict(event(5, china(12, 0, 0)), kind="voice", audio_bytes=len(audio) if bytes_ is None else bytes_, audio_crc=crc)
        clients = []
        class FakeClient:
            mtu_size = 185
            def __init__(self, *a, **k):
                self.is_connected = True; self.services = self; self.decoder = Decoder()
                self.max_write_without_response_size = 20; self.acks = []; self.dropped = not drop_first; clients.append(self)
            async def __aenter__(self): return self
            async def __aexit__(self, *a): self.is_connected = False
            def get_characteristic(self, _): return self
            async def start_notify(self, _, cb): self.cb = cb
            def notify(self, m):
                data = encode(m)
                for i in range(0, len(data), 13): self.cb(None, bytearray(data[i:i+13]))
            async def write_gatt_char(self, _, data, response):
                for m in self.decoder.feed(data):
                    if m["type"] == "hello": self.notify(dict(type="hello", device="abcdef123456", pending=1))
                    elif m["type"] == "ping": self.notify(dict(type="pong", token=m["token"], mono_us=11000000))
                    elif m["type"] == "sync": self.notify(dict(type="synced")); self.notify(msg)
                    elif m["type"] == "aget":
                        for off in range(m["off"], m["off"] + m["len"], m["n"]):
                            if not self.dropped: self.dropped = True; continue   # lose one chunk once
                            piece = audio[off:min(off + m["n"], m["off"] + m["len"])]
                            self.notify(dict(type="a", seq=m["seq"], off=off, d=base64.b64encode(piece).decode()))
                        self.notify(dict(type="aend", seq=m["seq"], off=m["off"] + m["len"]))
                    elif m["type"] == "ack": self.acks.append(m); self.is_connected = False
        heard = []
        stub = lambda pcm: heard.append(len(pcm)) or "测试转写"
        async def go():
            with patch.dict(sys.modules, {"bleak": types.SimpleNamespace(BleakClient=FakeClient)}):
                await asyncio.wait_for(session(object(), self.writer, transcriber=stub), 5)
        try:
            asyncio.run(go())
        except ProtocolError:
            pass
        return clients[0], heard

    def test_voice_end_to_end_with_lost_chunk(self):
        audio = bytes(range(256)) * 400  # 102400 bytes, crosses several aget windows
        client, heard = self.run_voice(audio, drop_first=True)
        self.assertEqual(len(client.acks), 1)
        self.assertEqual(heard, [len(audio) * 2])
        line = next(l for l in self.diary.read_text(encoding="utf-8").splitlines() if l.startswith("- 2026-11-17 12:00:00"))
        self.assertTrue(line.startswith("- 2026-11-17 12:00:00 🎙 测试转写 ![[a/knot-20261117-120000-3456-5."), line)
        self.assertEqual(len(list((self.vault / "a").glob("knot-*"))), 1)

    def test_voice_crc_mismatch_not_acked(self):
        client, heard = self.run_voice(b"\x11" * 3000, crc=1)
        self.assertEqual(client.acks, []); self.assertEqual(heard, [])
        self.assertNotIn("12:00:00", self.diary.read_text(encoding="utf-8"))

    def test_interrupted_voice(self):
        client, _ = self.run_voice(b"", bytes_=0, crc=0)
        self.assertEqual(len(client.acks), 1)
        self.assertIn("- 2026-11-17 12:00:00 🎙（录音中断，没有音频） ", self.diary.read_text(encoding="utf-8"))


class TranscriptCleanupTests(unittest.TestCase):
    def test_clean_segments(self):
        segs = [dict(text=" 今天开会 ", no_speech_prob=0.1, avg_logprob=-0.3),
                dict(text="请不吝点赞 订阅 转发 打赏支持明镜与点点栏目", no_speech_prob=0.2, avg_logprob=-0.2),
                dict(text="嗯", no_speech_prob=0.9, avg_logprob=-0.7), dict(text="对方说准备了的船果", no_speech_prob=0.0, avg_logprob=-2.85)]
        self.assertEqual(knot_receiver.clean_segments(segs), "今天开会")
        self.assertIsNone(knot_receiver.clean_segments(segs[1:]))

    def test_normalize(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy not installed")
        self.assertIsNone(knot_receiver.normalize_speech(np.zeros(16000, dtype=np.float32)))
        rng = np.random.default_rng(0)
        noise = (0.003 * rng.standard_normal(16000 * 4)).astype(np.float32)
        clicks = noise.copy(); clicks[16000:16000 + 600] += 0.2; clicks[48000:48000 + 600] -= 0.2
        self.assertIsNone(knot_receiver.normalize_speech(clicks))          # key clicks only
        speech = noise.copy(); speech[16000:40000] += (0.03 * np.sin(np.arange(24000) / 7)).astype(np.float32)
        out = knot_receiver.normalize_speech(speech)
        self.assertIsNotNone(out)
        self.assertAlmostEqual(float(np.sqrt(np.mean(out * out))), 0.08, delta=0.02)


class ProtocolTests(unittest.TestCase):
    def test_fragmentation_and_coalescing(self):
        payload = encode(event()) + encode(event(2))
        for size in [1,7,20,244,len(payload)]:
            decoder = Decoder(); messages=[]
            for i in range(0,len(payload),size): messages += decoder.feed(payload[i:i+size])
            self.assertEqual(messages,[event(),event(2)])

    def test_bad_notifications(self):
        for data in [b"{bad}\n", b"[]\n", b"x"*9000]:
            with self.assertRaises(ProtocolError): Decoder().feed(data)


class PinTests(unittest.TestCase):
    def test_normalize(self):
        self.assertEqual(normalize_pin("012345"),"012345")
        self.assertEqual(normalize_pin(" 12 345 "),"012345")
        self.assertIsNone(normalize_pin(""))
        self.assertIsNone(normalize_pin("1234567"))
    def test_ask_retries_then_gives_up(self):
        answers=iter(["abc","98765"])
        with patch("builtins.print"):
            self.assertEqual(ask_pin(lambda _: next(answers)),"098765")
            self.assertIsNone(ask_pin(lambda _: ""))

class LoopTests(unittest.IsolatedAsyncioTestCase):
    async def test_handshake_sync_replay_and_ack(self):
        """Exercise the actual async session via fake GATT with 20-byte chunks."""
        with tempfile.TemporaryDirectory() as tmp:
            writer=Writer(tmp,"结绳",today=lambda: date(2027,1,1))
            clients=[]
            class FakeClient:
                def __init__(self,*args,**kwargs):
                    self.is_connected=True; self.services=self; self.decoder=Decoder()
                    self.max_write_without_response_size=20; self.acks=[]; clients.append(self)
                async def __aenter__(self): return self
                async def __aexit__(self,*args): self.is_connected=False
                def get_characteristic(self,_): return self
                async def start_notify(self,_,cb): self.cb=cb
                def notify(self,msg):
                    data=encode(msg)
                    for i in range(0,len(data),13): self.cb(None,bytearray(data[i:i+13]))
                async def write_gatt_char(self,_,data,response):
                    for m in self.decoder.feed(data):
                        if m['type']=='hello': self.notify(dict(type='hello',device='abcdef123456',pending=1))
                        elif m['type']=='ping': self.notify(dict(type='pong',token=m['token'],mono_us=11000000))
                        elif m['type']=='sync':
                            self.notify(dict(type='synced')); self.notify(event()); self.notify(event())
                        elif m['type']=='ack':
                            self.acks.append(m)
                            if len(self.acks)==2: self.is_connected=False
            paired=[]
            async def fake_pair(device): paired.append(device)
            with patch.dict(sys.modules,{'bleak':types.SimpleNamespace(BleakClient=FakeClient)}), \
                 patch.object(knot_receiver,'ensure_paired_windows',fake_pair):
                await asyncio.wait_for(session(object(),writer),3)
            self.assertEqual(len(clients[0].acks),2)
            self.assertEqual(len(paired),1 if sys.platform=='win32' else 0)
            self.assertEqual(writer.ledger.read_text(encoding='utf-8').count('done'),1)

if __name__=='__main__': unittest.main()
