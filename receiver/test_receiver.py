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
