"""Focused controller regression tests; no serial hardware or OS key injection."""

import csv
import json
import queue
import tempfile
import time
import unittest
from unittest import mock

from backend import Controller


STATUS = "STATUS,1000,1,2,1,10,0,0,0,0,420,90\n"
FIRMWARE = "FIRMWARE,gesture-20261005-r3,KEY_CAPTURE,NO_CALIBRATION,RANDOM_START,LIVE_MATCH,ONE_DEMO\n"
WARNING_FIRMWARE = "FIRMWARE,gesture-20261005-r4,KEY_CAPTURE,NO_CALIBRATION,RANDOM_START,LIVE_MATCH,ONE_DEMO,SIMILARITY_WARNING\n"


def until(predicate, timeout=2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.01)
    raise AssertionError("Condition was not reached")


class FakeSerial:
    def __init__(self):
        self.chunks = queue.Queue()
        self.writes = []
        self.closed = False
        self.reset = False
        self.waiting_error = None
        self.on_waiting_error = None

    @property
    def in_waiting(self):
        if self.waiting_error is not None:
            if self.on_waiting_error:
                self.on_waiting_error()
                self.on_waiting_error = None
            raise self.waiting_error
        return 4096 if not self.chunks.empty() else 0

    def reset_input_buffer(self):
        self.reset = True

    def read(self, size):
        try:
            chunk = self.chunks.get(timeout=0.01)
        except queue.Empty:
            return b""
        if isinstance(chunk, Exception):
            raise chunk
        return chunk

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def close(self):
        self.closed = True

    def feed(self, text):
        self.chunks.put(text.encode("ascii"))


class Keyboard:
    def __init__(self):
        self.keys = []

    def send(self, key, up):
        self.keys.append((key, up))


class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.serial = FakeSerial()
        self.keyboard = Keyboard()
        self.controller = Controller(
            self.temp.name, serial_factory=lambda *args, **kwargs: self.serial,
            ports_provider=lambda: [{"device": "COM_TEST", "description": "Fake board"}],
            keyboard_factory=lambda: self.keyboard,
        )

    def tearDown(self):
        self.controller.close()
        self.temp.cleanup()

    def connect_real(self):
        self.controller.connect("COM_TEST")
        self.serial.feed(STATUS)
        until(lambda: self.controller.snapshot()["connection"]["state"] == "connected")

    def feed_status(self):
        self.serial.feed(STATUS)
        until(lambda: "t" in self.controller.snapshot()["status"])

    def test_split_lines_inventory_and_shutdown_commands(self):
        self.connect_real()
        self.serial.feed("HELLO,DM_MC02_GES")
        self.serial.feed("TURE,1\r\nINFO,1,7,1,0,1,0,0,0\nSLOT,1,3,Wave\n")
        self.serial.feed("CAPTURE,MODE,KEY\n")
        for number in range(2, 9):
            self.serial.feed(f"SLOT,{number},0,\n")
        self.feed_status()
        self.serial.feed("RAW,1,5,100,200,9810,0,0,0\nRAW,2,10,200,")
        self.serial.feed("300,9810,0,0,0\n")
        snapshot = until(lambda: self.controller.snapshot() if self.controller.snapshot()["counters"]["raw"] == 2 else None)
        self.assertTrue(snapshot["protocol"]["inventory"])
        self.assertEqual(snapshot["slots"][0]["state"], "saved")
        self.assertEqual(snapshot["slots"][7]["state"], "empty")
        self.assertAlmostEqual(snapshot["waveform"][0]["az"], 9.81)
        self.controller.command("learn 8")
        until(lambda: b"learn 8\n" in self.serial.writes)
        self.controller.disconnect()
        self.assertTrue(self.serial.closed)
        self.assertEqual(self.serial.writes[-2:], [b"disarm\n", b"stream 0\n"])
        self.assertTrue(self.serial.reset)

    def test_old_firmware_does_not_invent_empty_slots(self):
        self.connect_real()
        self.serial.feed("ERROR,BAD_COMMAND\n" + STATUS)
        until(lambda: self.controller.snapshot()["status"]["classes"] == 2)
        self.assertTrue(all(s["state"] == "unknown" for s in self.controller.snapshot()["slots"]))
        with self.assertRaisesRegex(ValueError, "KEY-controlled"):
            self.controller.command("learn", 3)
        self.assertNotIn(b"learn 3\n", self.serial.writes)

    def test_training_save_failure_and_inventory_refresh(self):
        self.connect_real()
        self.serial.feed("TRAIN,BEGIN,3\nTRAIN,DEMO,3,1\nTRAIN,REJECT,too short\n")
        until(lambda: self.controller.snapshot()["training"]["collected"] == 1)
        self.assertEqual(self.controller.snapshot()["training"]["message"], "too short")
        self.serial.feed("TRAIN,DEMO,3,3\nTRAIN,READY,3\nERROR,SAVE,FLASH_UNAVAILABLE\n")
        until(lambda: self.controller.snapshot()["training"]["state"] == "ready")
        self.assertTrue(any(e["kind"] == "ERROR" for e in self.controller.snapshot()["events"]))
        prior_lists = self.serial.writes.count(b"list\n")
        self.serial.feed("SAVED,3,1024\nSTATE,IDLE\n")
        until(lambda: self.serial.writes.count(b"list\n") > prior_lists)
        self.assertEqual(self.controller.snapshot()["training"]["state"], "idle")
        self.serial.feed("DELETE,CONFIRM,3\n")
        until(lambda: self.controller.snapshot()["pending_delete"] == 3)
        self.serial.feed("STATE,IDLE\n")
        until(lambda: self.controller.snapshot()["pending_delete"] is None)

    def test_firmware_identity_and_legacy_calibration_error(self):
        self.connect_real()
        self.assertIsNone(self.controller.snapshot()["protocol"]["firmware"])
        self.assertEqual(self.controller.snapshot()["training"]["required"], 3)
        self.serial.feed("ERROR,LEARN,NOT_CALIBRATED\n")
        until(lambda: self.controller.snapshot()["protocol"]["requires_calibration"])
        self.serial.feed("HELLO,DM_MC02_GESTURE,1\n" + FIRMWARE + STATUS)
        until(lambda: self.controller.snapshot()["protocol"]["firmware"])
        snapshot = self.controller.snapshot()
        self.assertFalse(snapshot["protocol"]["requires_calibration"])
        self.assertIn("RANDOM_START", snapshot["protocol"]["capabilities"])
        self.assertEqual(snapshot["training"]["required"], 1)
        count = sum(e["kind"] == "FIRMWARE" for e in snapshot["events"])
        self.serial.feed(FIRMWARE + FIRMWARE)
        until(lambda: self.serial.chunks.empty())
        self.assertEqual(sum(e["kind"] == "FIRMWARE" for e in self.controller.snapshot()["events"]), count)

    def test_one_demo_ready_keeps_actual_count_and_optional_capture(self):
        self.connect_real()
        self.serial.feed(FIRMWARE + "TRAIN,BEGIN,3\nTRAIN,DEMO,3,1\nTRAIN,READY,3,1\n")
        until(lambda: self.controller.snapshot()["training"]["state"] == "ready")
        self.assertEqual(self.controller.snapshot()["training"]["collected"], 1)
        self.serial.feed("CAPTURE,BEGIN,3\nINFO,1,7,1,1,3,1,1,0\n")
        until(lambda: self.controller.snapshot()["training"]["capturing"])
        self.assertEqual(self.controller.snapshot()["training"]["collected"], 1)
        self.assertEqual(self.controller.snapshot()["training"]["state"], "ready")
        self.serial.feed("CAPTURE,END,3\nTRAIN,REJECT,too short\n")
        until(lambda: self.controller.snapshot()["training"]["message"] == "too short")
        self.assertEqual(self.controller.snapshot()["training"]["collected"], 1)
        self.assertFalse(self.controller.snapshot()["training"]["capturing"])
        self.serial.feed("TRAIN,DEMO,3,2\nTRAIN,READY,3,2\n")
        until(lambda: self.controller.snapshot()["training"]["collected"] == 2)
        self.assertEqual(self.controller.snapshot()["training"]["state"], "ready")

    def test_live_match_never_emits_hotkeys_and_clears_on_expiry(self):
        self.connect_real()
        self.controller.save_slot(1, "Wave", "ctrl+k")
        self.controller.set_hotkeys(True)
        self.serial.feed(FIRMWARE + "MATCH,1200,1,125000,900000,500\n")
        until(lambda: self.controller.snapshot()["live_match"]["id"] == 1)
        self.assertEqual(self.controller.snapshot()["live_match"]["distance"], 0.125)
        self.assertEqual(self.controller.snapshot()["counters"]["events"], 0)
        self.assertEqual(self.keyboard.keys, [])
        with self.controller._lock:
            self.controller._last_match = time.monotonic() - 1
        self.assertEqual(self.controller.snapshot()["live_match"]["id"], 0)
        self.serial.feed("MATCH,1300,2,120000,800000,600\nMATCH,1400,0,0,0,0\n")
        until(lambda: self.controller.snapshot()["live_match"]["t"] == 1400)
        self.assertEqual(self.controller.snapshot()["live_match"]["id"], 0)
        self.assertEqual(self.controller.snapshot()["recent_match"]["id"], 2)
        with self.controller._lock:
            self.controller._last_confident_match = time.monotonic() - 0.36
        self.assertIsNone(self.controller.snapshot()["recent_match"])
        self.assertEqual(self.keyboard.keys, [])
        self.serial.feed("MATCH,1500,1,120000,800000,600\nSTATE,IDLE\nSTATE,ARMED\n")
        until(lambda: self.controller.snapshot()["live_match"]["t"] == 1500)
        self.assertIsNone(self.controller.snapshot()["recent_match"])
        self.controller.disconnect()
        self.assertIsNone(self.controller.snapshot()["live_match"]["t"])
        self.assertIsNone(self.controller.snapshot()["recent_match"])

    def test_similarity_warning_preserves_ready_and_save(self):
        self.connect_real()
        self.serial.feed(WARNING_FIRMWARE + "SLOT,1,3,Original\nTRAIN,BEGIN,2\n"
                         "TRAIN,DEMO,2,1\nTRAIN,WARN,SIMILAR,1,120000,170000\n"
                         "TRAIN,READY,2,1\nINFO,1,7,1,1,2,1,1,0\n")
        until(lambda: self.controller.snapshot()["training"]["state"] == "ready")
        training = self.controller.snapshot()["training"]
        self.assertEqual(training["collected"], 1)
        self.assertEqual(training["message"], "")
        self.assertEqual(training["warning"], {"kind": "SIMILAR", "slot": 1,
                                             "distance": 0.12, "limit": 0.17})
        self.serial.feed("CAPTURE,BEGIN,2\nCAPTURE,END,2\nTRAIN,REJECT,LOW QUALITY\n")
        until(lambda: self.controller.snapshot()["training"]["message"] == "LOW QUALITY")
        self.assertEqual(self.controller.snapshot()["training"]["warning"], training["warning"])
        self.controller.command("save")
        until(lambda: b"save\n" in self.serial.writes)
        self.serial.feed("SAVED,2,14128\nSTATE,IDLE\n")
        until(lambda: self.controller.snapshot()["training"]["state"] == "idle")
        self.assertIsNone(self.controller.snapshot()["training"]["warning"])
        self.assertEqual(self.controller.snapshot()["slots"][0]["templates"], 3)
        self.assertEqual(self.keyboard.keys, [])

    def test_similarity_warning_validation_and_trial_reset(self):
        self.connect_real()
        with self.controller._lock:
            for line in ("TRAIN,WARN,SIMILAR,1,100,170000", "TRAIN,BEGIN,2",
                         "TRAIN,DEMO,2,1", "TRAIN,WARN,SIMILAR,2,100,170000",
                         "TRAIN,WARN,SIMILAR,9,100,170000", "TRAIN,WARN,SIMILAR,1,180000,170000",
                         "TRAIN,WARN,SIMILAR,1,NaN,170000", "TRAIN,WARN,SIMILAR,1,100",
                         "TRAIN,WARN,SIMILAR,1,170000,170000", "TRAIN,READY,2,1"):
                self.controller._process_line(line)
        snapshot = self.controller.snapshot()
        self.assertEqual(snapshot["counters"]["malformed"], 6)
        self.assertEqual(snapshot["training"]["warning"]["slot"], 1)
        self.serial.feed("STATE,IDLE\nTRAIN,BEGIN,3\n")
        until(lambda: self.controller.snapshot()["training"]["slot"] == 3)
        self.assertIsNone(self.controller.snapshot()["training"]["warning"])

    def test_hotkeys_explicit_real_fresh_unique_and_epoch_scoped(self):
        self.connect_real()
        self.controller.save_slot(1, "Wave", "ctrl+k")
        self.feed_status()
        self.serial.feed("EVENT,1100,1,100,900,720\n")
        until(lambda: self.controller.snapshot()["counters"]["events"] == 1)
        self.assertEqual(self.keyboard.keys, [])
        self.controller.set_hotkeys(True)
        self.serial.feed("EVENT,900,1,100,900,720\nEVENT,1200,1,100,900,720\nEVENT,1200,1,100,900,720\n")
        until(lambda: self.controller.snapshot()["counters"]["duplicate_events"] == 1)
        self.assertEqual(len(self.keyboard.keys), 4)
        self.serial.feed("TRAIN,BEGIN,3\nTRAIN,DEMO,3,2\nHELLO,DM_MC02_GESTURE,1\n" + STATUS + "EVENT,1300,1,100,900,720\n")
        until(lambda: self.controller.snapshot()["connection"]["epoch"] == 2)
        self.assertFalse(self.controller.snapshot()["hotkeys_enabled"])
        self.assertEqual(self.controller.snapshot()["training"]["state"], "idle")
        self.assertEqual(len(self.keyboard.keys), 4)

    def test_disconnect_error_clears_live_state_and_hotkeys_during_retry(self):
        self.connect_real()
        self.feed_status()
        self.controller.set_hotkeys(True)
        self.serial.feed("TRAIN,BEGIN,2\nTRAIN,DEMO,2,1\n")
        until(lambda: self.controller.snapshot()["training"]["collected"] == 1)
        self.serial.chunks.put(OSError("Device unplugged"))
        until(lambda: self.controller.snapshot()["connection"]["state"] == "reconnecting")
        self.assertFalse(self.controller.snapshot()["hotkeys_enabled"])
        self.assertEqual(self.controller.snapshot()["training"]["state"], "idle")
        self.assertTrue(self.serial.closed)

    def test_stale_windows_handle_recovers_with_fresh_status_and_no_mutation_replay(self):
        replacement = FakeSerial()
        opened = []

        def factory(*args, **kwargs):
            opened.append(args[0])
            return self.serial if len(opened) == 1 else replacement

        self.controller._serial_factory = factory
        with mock.patch("backend.SERIAL_RETRY_DELAYS", (0.02, 0.02, 0.02)), \
                mock.patch("backend.logging.getLogger"):
            self.connect_real()
            self.controller.save_slot(1, "Wave", "ctrl+k")
            self.controller.set_hotkeys(True)
            self.controller.capture_start()
            self.serial.feed("RAW,1,5,0,0,9807,0,0,0,1\nTRAIN,BEGIN,2\nTRAIN,DEMO,2,1\n")
            until(lambda: self.controller.snapshot()["training"]["collected"] == 1)
            before = self.controller.snapshot()

            def pending_at_failure():
                with self.controller._lock:
                    for command in ("save", "learn 2", "delete 1", "arm"):
                        self.controller._queue_command(command)
                    self.controller._pending_delete = 1

            self.serial.on_waiting_error = pending_at_failure
            self.serial.waiting_error = OSError(
                "ClearCommError failed (PermissionError(13, 'Device does not recognize this command', None, 22))")
            until(lambda: b"status\n" in replacement.writes)
            state = self.controller.snapshot()
            self.assertTrue(self.serial.closed)
            self.assertEqual(state["connection"]["state"], "reconnecting")
            self.assertEqual(state["connection"]["retry_attempt"], 1)
            self.assertGreater(state["connection"]["epoch"], before["connection"]["epoch"])
            self.assertGreater(state["pen_epoch"], before["pen_epoch"])
            self.assertEqual(state["pen_samples"], [])
            self.assertFalse(state["hotkeys_enabled"])
            self.assertIsNone(state["pending_delete"])
            self.assertEqual(state["training"]["state"], "idle")
            self.assertFalse(state["capture"]["active"])
            self.assertEqual(state["capture"]["reason"], "connection_lost")
            self.assertEqual(replacement.writes, [b"stream 0\n", b"disarm\n", b"status\n", b"list\n"])
            replacement.feed("STATUS,invalid\nEVENT,1200,1,100,900,720\n")
            until(lambda: self.controller.snapshot()["counters"]["events"] == 1)
            self.assertEqual(self.controller.snapshot()["connection"]["state"], "reconnecting")
            self.assertNotIn(b"stream 1\n", replacement.writes)
            replacement.feed(STATUS + "EVENT,1300,1,100,900,720\n")
            until(lambda: b"stream 1\n" in replacement.writes)
            state = self.controller.snapshot()
            self.assertEqual(state["connection"]["state"], "connected")
            self.assertIsNone(state["connection"]["error"])
            self.assertIsNone(state["connection"]["error_detail"])
            self.assertEqual(state["connection"]["retry_attempt"], 0)
            self.assertEqual(self.keyboard.keys, [])
            self.assertTrue(all(command not in replacement.writes for command in
                                (b"save\n", b"learn 2\n", b"delete 1\n", b"arm\n")))

    def test_disconnect_interrupts_retry_delay(self):
        factory = mock.Mock(return_value=self.serial)
        self.controller._serial_factory = factory
        with mock.patch("backend.SERIAL_RETRY_DELAYS", (5, 5, 5)), \
                mock.patch("backend.logging.getLogger"):
            self.connect_real()
            self.serial.waiting_error = OSError("ClearCommError failed")
            until(lambda: self.controller.snapshot()["connection"]["state"] == "reconnecting")
            start = time.monotonic()
            self.controller.disconnect()
            self.assertLess(time.monotonic() - start, 0.5)
            self.assertEqual(factory.call_count, 1)
            self.assertEqual(self.controller.snapshot()["connection"]["state"], "disconnected")
            self.assertFalse(self.controller._worker)

    def test_recovery_exhausts_three_fresh_handles_and_explains_failure(self):
        connections = []

        def factory(*args, **kwargs):
            connection = FakeSerial()
            connection.waiting_error = OSError("ClearCommError failed")
            connections.append(connection)
            return connection

        self.controller._serial_factory = factory
        with mock.patch("backend.SERIAL_RETRY_DELAYS", (0.01, 0.01, 0.01)), \
                mock.patch("backend.logging.getLogger"):
            self.controller.connect("COM_TEST")
            until(lambda: self.controller.snapshot()["connection"]["state"] == "error")
            state = self.controller.snapshot()
            self.assertEqual(len(connections), 4)
            self.assertTrue(all(connection.closed for connection in connections))
            self.assertEqual(state["connection"]["retry_attempt"], 3)
            self.assertIn("\u81ea\u52a8\u91cd\u8fde\u672a\u6210\u529f", state["connection"]["error"])
            self.assertIn("ClearCommError", state["connection"]["error_detail"])
            self.assertEqual(sum(event["kind"] == "RECONNECTING" for event in state["events"]), 3)
            self.assertTrue(all(b"stream 1\n" not in connection.writes for connection in connections))

    def test_access_denied_open_is_not_retried(self):
        factory = mock.Mock(side_effect=PermissionError(13, "Access is denied"))
        self.controller._serial_factory = factory
        with mock.patch("backend.logging.getLogger"):
            self.controller.connect("COM_TEST")
            until(lambda: self.controller.snapshot()["connection"]["state"] == "error")
        self.assertEqual(factory.call_count, 1)
        self.assertIn("\u4e32\u53e3\u88ab\u5360\u7528", self.controller.snapshot()["connection"]["error"])

    def test_recovery_follows_only_exact_unique_usb_serial_identity(self):
        identity = "USB VID:PID=0483:5740 SER=BOARD001"
        self.controller._ports_provider = lambda: [{"device": "COM_NEW", "hwid": identity}]
        self.assertEqual(self.controller._recovery_port("COM_TEST", identity), "COM_NEW")
        self.controller._ports_provider = lambda: [{"device": "COM_TEST", "hwid": "USB VID:PID=0483:5740 SER=OTHER"}]
        with self.assertRaises(FileNotFoundError):
            self.controller._recovery_port("COM_TEST", identity)
        self.controller._ports_provider = lambda: [{"device": "COM_NEW", "hwid": "USB VID:PID=0483:5740"}]
        with self.assertRaises(FileNotFoundError):
            self.controller._recovery_port("COM_TEST", "USB VID:PID=0483:5740")

    def test_demo_learning_requires_save_and_never_emits_hotkeys(self):
        self.controller.connect(demo=True)
        until(lambda: self.controller.snapshot()["protocol"]["inventory"])
        with self.assertRaises(ValueError):
            self.controller.set_hotkeys(True)
        self.controller.command("learn 3")
        until(lambda: self.controller.snapshot()["training"]["state"] == "recording")
        until(lambda: self.controller.snapshot()["counters"]["raw"] > 20)
        self.assertEqual(self.controller.snapshot()["training"]["collected"], 0)
        for count in range(1, 4):
            self.controller.command("demo key 1")
            until(lambda: self.controller.snapshot()["training"]["capturing"])
            with self.controller._lock:
                self.controller._demo_key_started = time.monotonic() - 0.7
            self.controller.command("demo key 0")
            until(lambda: self.controller.snapshot()["training"]["collected"] == count)
            self.assertEqual(self.controller.snapshot()["training"]["state"], "ready")
        snapshot = self.controller.snapshot()
        self.assertEqual(snapshot["training"]["state"], "ready")
        self.assertEqual(snapshot["slots"][2]["state"], "empty")
        self.controller.command("save")
        until(lambda: self.controller.snapshot()["slots"][2]["state"] == "saved")
        self.assertTrue(self.controller.snapshot()["connection"]["simulation"])
        self.assertEqual(self.keyboard.keys, [])

    def test_demo_one_recording_can_save_and_optional_hold_blocks_save(self):
        self.controller.connect(demo=True)
        until(lambda: self.controller.snapshot()["protocol"]["inventory"])
        self.controller.command("learn 3")
        until(lambda: self.controller.snapshot()["training"]["state"] == "recording")
        self.controller.command("demo key 1")
        until(lambda: self.controller.snapshot()["training"]["capturing"])
        with self.controller._lock:
            self.controller._demo_key_started = time.monotonic() - 0.7
        self.controller.command("demo key 0")
        until(lambda: self.controller.snapshot()["training"]["state"] == "ready")
        self.controller.command("demo key 1")
        until(lambda: self.controller.snapshot()["training"]["capturing"])
        self.controller.command("save")
        until(lambda: self.controller.snapshot()["training"]["message"] == "SAVE, NOT_READY")
        self.assertEqual(self.controller.snapshot()["slots"][2]["state"], "empty")
        with self.controller._lock:
            self.controller._demo_key_started = time.monotonic()
        self.controller.command("demo key 0")
        until(lambda: not self.controller.snapshot()["training"]["capturing"])
        self.controller.command("save")
        until(lambda: self.controller.snapshot()["slots"][2]["state"] == "saved")
        self.assertEqual(self.controller.snapshot()["slots"][2]["templates"], 1)

    def test_demo_rejects_overlong_hold_only_after_release(self):
        self.controller.connect(demo=True)
        until(lambda: self.controller.snapshot()["protocol"]["manual_training"])
        self.controller.command("learn 3")
        until(lambda: self.controller.snapshot()["training"]["state"] == "recording")
        self.controller.command("demo key 1")
        until(lambda: self.controller.snapshot()["training"]["capturing"])
        with self.controller._lock:
            self.controller._demo_key_started = time.monotonic() - 3
        self.controller.command("status")
        until(lambda: self.controller._commands.empty())
        self.assertTrue(self.controller.snapshot()["training"]["capturing"])
        self.assertEqual(self.controller.snapshot()["training"]["collected"], 0)
        self.controller.command("demo key 0")
        until(lambda: self.controller.snapshot()["training"]["message"] == "too long")
        self.assertFalse(self.controller.snapshot()["training"]["capturing"])
        self.controller.command("cancel")
        until(lambda: self.controller.snapshot()["training"]["state"] == "idle")
        self.controller.command("demo key 0")
        self.assertEqual(self.controller.snapshot()["training"]["collected"], 0)

    def test_real_key_protocol_and_calibration_feedback(self):
        self.connect_real()
        with self.assertRaisesRegex(ValueError, "demo device"):
            self.controller.command("demo key 1")
        self.serial.feed("CAPTURE,MODE,KEY\nTRAIN,BEGIN,3\nCAPTURE,WAIT,3\n")
        until(lambda: self.controller.snapshot()["protocol"]["manual_training"])
        self.assertFalse(self.controller.snapshot()["training"]["capturing"])
        self.serial.feed("CAPTURE,BEGIN,3\n")
        until(lambda: self.controller.snapshot()["training"]["capturing"])
        self.serial.feed("CAPTURE,END,3\nTRAIN,DEMO,3,1\nCAPTURE,WAIT,3\n")
        until(lambda: self.controller.snapshot()["training"]["collected"] == 1)
        self.assertFalse(self.controller.snapshot()["training"]["capturing"])
        self.serial.feed("CALIBRATION,FAILED\n")
        until(lambda: not self.controller.snapshot()["status"]["calibrated"])
        self.serial.feed("CALIBRATION,OK\n")
        until(lambda: self.controller.snapshot()["status"]["calibrated"])
        self.serial.feed("STATE,IDLE\n")
        until(lambda: self.controller.snapshot()["training"]["state"] == "idle")
        self.assertFalse(self.controller.snapshot()["training"]["capturing"])

    def test_capture_preserves_every_sample_and_metadata(self):
        self.connect_real()
        self.controller.capture_start({"label": "wave", "speed": "fast"})
        for number in range(1, 13):
            self.serial.feed(f"RAW,{number},{number * 5},100,200,9810,0,0,0\n")
        self.serial.feed("EVENT,1000,1,100,900,720\nUNKNOWN,1001,distance\n")
        until(lambda: self.controller.snapshot()["counters"]["unknown"] == 1)
        self.controller.capture_stop()
        captures = self.controller.list_captures()
        self.assertEqual(len(captures), 1)
        capture = captures[0]
        self.assertEqual(capture["raw_count"], 12)
        self.assertEqual(capture["event_count"], 1)
        self.assertEqual(capture["unknown_count"], 1)
        self.assertFalse(capture["simulation"])
        with (self.controller.capture_dir / capture["files"]["csv"]).open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 12)
        self.assertEqual(rows[0]["label"], "wave")
        events = (self.controller.capture_dir / capture["files"]["events"]).read_text(encoding="utf-8").splitlines()
        self.assertEqual(json.loads(events[1])["record"]["kind"], "UNKNOWN")

    def test_settings_persist_and_cursor_filters_events(self):
        self.controller.save_slot(8, "Jump", "shift+space")
        reopened = Controller(self.temp.name)
        self.assertEqual(reopened.snapshot()["slots"][7]["hotkey"], "shift+space")
        reopened.close()
        self.connect_real()
        cursor = self.controller.snapshot()["cursor"]
        self.serial.feed("ERROR,SAVE,NOT_READY\n")
        until(lambda: self.controller.snapshot()["cursor"] > cursor)
        events = self.controller.snapshot(cursor)["events"]
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["kind"], "ERROR")
        with self.assertRaises(ValueError):
            self.controller.command("learn 9")
        with self.assertRaises(ValueError):
            self.controller.connect("NOT_A_PORT")
        with self.assertRaises(ValueError):
            self.controller.save_slot(1, "Wave", "ctrl+unsupported")

    def test_handshake_requires_status_and_never_streams_before_it(self):
        with mock.patch("backend.HANDSHAKE_TIMEOUT_SECONDS", 0.12):
            self.controller.connect("COM_TEST")
            until(lambda: b"status\n" in self.serial.writes)
            self.assertEqual(self.controller.snapshot()["connection"]["state"], "connecting")
            self.serial.feed("STATUS,invalid\nINFO,1,7,1,0,1,0,0,0\nRAW,1,5,0,0,9810,0,0,0\n")
            snapshot = until(lambda: self.controller.snapshot() if self.controller.snapshot()["connection"]["state"] == "error" else None)
            self.assertIn("handshake timed out", snapshot["connection"]["error_detail"])
            self.assertIn("\u677f\u7aef\u672a\u54cd\u5e94", snapshot["connection"]["error"])
            self.assertNotIn(b"stream 1\n", self.serial.writes)
            until(lambda: self.serial.closed)

    def test_old_firmware_status_completes_handshake_and_starts_stream(self):
        self.controller.connect("COM_TEST")
        self.serial.feed("ERROR,BAD_COMMAND\n" + STATUS)
        until(lambda: self.controller.snapshot()["connection"]["state"] == "connected")
        until(lambda: b"stream 1\n" in self.serial.writes)
        self.assertFalse(self.controller.snapshot()["protocol"]["inventory"])
        self.assertTrue(all(slot["state"] == "unknown" for slot in self.controller.snapshot()["slots"]))

    def test_status_disarmed_disables_hotkeys_without_state_record(self):
        self.connect_real()
        self.controller.save_slot(1, "Wave", "ctrl+k")
        self.controller.set_hotkeys(True)
        self.serial.feed("STATUS,1100,0,2,1,10,0,0,0,0,420,90\nEVENT,1200,1,100,900,720\n")
        until(lambda: self.controller.snapshot()["status"]["armed"] == 0)
        self.assertFalse(self.controller.snapshot()["hotkeys_enabled"])
        self.assertEqual(self.keyboard.keys, [])

    def test_uncalibrated_board_can_learn_and_emit_explicit_hotkeys(self):
        self.controller.connect("COM_TEST")
        self.serial.feed("STATUS,1000,1,2,0,10,0,0,0,0,420,90\nCAPTURE,MODE,KEY\n")
        until(lambda: self.controller.snapshot()["protocol"]["manual_training"])
        self.controller.command("learn 3")
        until(lambda: b"learn 3\n" in self.serial.writes)
        self.controller.save_slot(1, "Wave", "ctrl+k")
        self.controller.set_hotkeys(True)
        self.serial.feed("EVENT,1200,1,100,900,720\n")
        until(lambda: self.controller.snapshot()["counters"]["hotkeys"] == 1)
        self.assertEqual(len(self.keyboard.keys), 4)
        self.assertFalse(self.controller.snapshot()["status"]["calibrated"])

    def test_pen_history_preserves_samples_between_polls_and_is_bounded(self):
        for number in range(1, 1201):
            self.controller._process_line(f"RAW,{number},{number * 5},0,0,9807,0,0,400")
        state = self.controller.snapshot()
        samples = state["pen_samples"]
        self.assertEqual(len(samples), 256)
        self.assertEqual(state["pen_cursor"], 300)
        self.assertEqual(samples[-1]["seq"], 300)
        self.assertTrue(all(b["seq"] - a["seq"] == 1 and b["t"] - a["t"] == 20
                            for a, b in zip(samples, samples[1:])))
        self.assertNotEqual(samples[0]["q"], samples[-1]["q"])
        samples[-1]["q"][0] = 99
        self.assertNotEqual(self.controller.snapshot()["pen_samples"][-1]["q"][0], 99)

    def test_pen_keeps_physical_key_edges_between_http_polls(self):
        for number, down in enumerate((0, 1, 1, 0, 0), start=1):
            self.controller._process_line(f"RAW,{number},{number * 5},0,0,9807,0,0,100,{down}")
        state = self.controller.snapshot()
        self.assertEqual([sample["key_down"] for sample in state["pen_samples"]], [False, True, False])
        self.assertEqual([sample["t"] for sample in state["pen_samples"]], [5, 10, 20])
        self.assertTrue(all(len(sample["p"]) == 3 for sample in state["pen_samples"]))
        self.assertEqual(state["key"], {"available": True, "down": False})
        self.controller._process_line("RAW,6,30,0,0,9807,0,0,100")
        self.assertEqual(self.controller.snapshot()["key"], {"available": False, "down": None})
        self.assertIsNone(self.controller.snapshot()["pen_samples"][-1]["key_down"])

    def test_pen_missing_raw_breaks_stroke_and_stale_key_is_unavailable(self):
        self.controller._process_line("RAW,1,5,0,0,9807,0,0,0,0")
        self.controller._process_line("RAW,2,10,0,0,9807,0,0,0,1")
        before = self.controller.snapshot()
        self.controller._process_line("RAW,4,20,0,0,9807,0,0,0,1")
        after = self.controller.snapshot()
        self.assertGreater(after["pen_epoch"], before["pen_epoch"])
        self.assertEqual(len(after["pen_samples"]), 1)
        self.controller._last_pose_sample -= 0.6
        self.assertEqual(self.controller.snapshot()["key"], {"available": False, "down": None})
        self.controller.disconnect()
        self.assertEqual(self.controller.snapshot()["key"], {"available": False, "down": None})

    def test_demo_key_reports_raw_state_outside_learning_without_training(self):
        self.controller.connect(demo=True)
        until(lambda: self.controller.snapshot()["key"]["available"])
        self.assertFalse(self.controller.snapshot()["key"]["down"])
        self.controller.command("demo key 1")
        until(lambda: self.controller.snapshot()["key"]["down"] is True)
        self.assertEqual(self.controller.snapshot()["training"]["state"], "idle")
        self.controller.command("demo key 0")
        until(lambda: self.controller.snapshot()["key"]["down"] is False)
        self.assertEqual(self.controller.snapshot()["training"]["collected"], 0)

    def test_pen_world_translation_does_not_accumulate_while_key_is_up(self):
        for number in range(1, 201):
            self.controller._process_line(f"RAW,{number},{number * 5},0,0,10807,0,0,0,0")
        self.assertEqual(self.controller.snapshot()["pen_motion"]["position_m"], [0.0, 0.0, 0.0])
        for number in range(201, 401):
            self.controller._process_line(f"RAW,{number},{number * 5},0,0,10807,0,0,0,1")
        held = self.controller.snapshot()["pen_motion"]["position_m"]
        self.assertGreater(held[2], .2)
        for number in range(401, 801):
            self.controller._process_line(f"RAW,{number},{number * 5},0,0,10807,0,0,0,0")
        self.assertEqual(self.controller.snapshot()["pen_motion"]["position_m"], held)
        self.controller.reset_pose()
        self.assertEqual(self.controller.snapshot()["pen_motion"]["position_m"], held)

    def test_pen_history_ignores_invalid_stale_and_duplicate_data(self):
        self.controller._process_line("RAW,1,5,0,0,9807,0,0,0")
        self.controller._process_line("RAW,2,25,0,0,9807,0,0,100")
        before = self.controller.snapshot()
        for line in ("RAW,2,25,0,0,9807,0,0,100", "RAW,1,5,0,0,9807,0,0,0",
                     "RAW,3,45,0,0,999999,0,0,0"):
            self.controller._process_line(line)
        state = self.controller.snapshot()
        self.assertEqual(state["pen_samples"], before["pen_samples"])
        self.assertEqual(state["pen_cursor"], before["pen_cursor"])

    def test_pen_history_starts_new_epoch_after_gaps_zero_and_reboot(self):
        self.controller._process_line("RAW,1,5,0,0,9807,0,0,0")
        before = self.controller.snapshot()
        self.controller._process_line("RAW,2,500,0,0,9807,0,0,0")
        self.assertEqual(self.controller.snapshot()["pen_cursor"], before["pen_cursor"])
        self.controller._process_line("RAW,3,505,0,0,9807,0,0,0")
        after_gap = self.controller.snapshot()
        self.assertGreater(after_gap["pen_epoch"], before["pen_epoch"])
        self.assertEqual(len(after_gap["pen_samples"]), 1)
        zeroed = self.controller.reset_pose()
        self.assertGreater(zeroed["pen_epoch"], after_gap["pen_epoch"])
        self.assertEqual(zeroed["pen_samples"], [])
        self.controller._process_line("RAW,4,510,0,0,9807,0,0,0")
        self.assertGreater(self.controller.snapshot()["pen_cursor"], after_gap["pen_cursor"])
        self.controller._process_line("HELLO,DM_MC02_GESTURE,1")
        rebooted = self.controller.snapshot()
        self.assertGreater(rebooted["pen_epoch"], zeroed["pen_epoch"])
        self.assertEqual(rebooted["pen_samples"], [])

    def test_pen_history_handles_clock_wrap_and_translation_limit(self):
        for seq, tick in enumerate((0xFFFFFFF0, 4, 24), start=1):
            self.controller._process_line(f"RAW,{seq},{tick},0,0,9807,0,0,100")
        state = self.controller.snapshot()
        self.assertEqual(len(state["pen_samples"]), 3)
        self.controller._pose._position_limited = True
        self.controller._process_line("RAW,4,44,0,0,9807,0,0,100")
        limited = self.controller.snapshot()
        self.assertTrue(limited["pose"]["position_limited"])
        self.assertEqual(limited["pen_cursor"], state["pen_cursor"] + 1)
        self.assertEqual(limited["pen_epoch"], state["pen_epoch"])

    def test_pose_consumes_raw_and_clears_on_disconnect(self):
        self.connect_real()
        for number in range(1, 21):
            self.serial.feed(f"RAW,{number},{number * 5},0,0,9807,0,0,100\n")
        until(lambda: self.controller.snapshot()["counters"]["raw"] == 20)
        self.assertTrue(self.controller.snapshot()["pose"]["available"])
        self.controller.reset_pose()
        self.assertEqual(self.controller.snapshot()["pose"]["position_m"], [0.0, 0.0, 0.0])
        self.controller.disconnect()
        self.assertFalse(self.controller.snapshot()["pose"]["available"])


if __name__ == "__main__":
    unittest.main()
