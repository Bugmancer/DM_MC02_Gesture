"""Pure host protocol tests; no serial port or native keyboard is opened."""

import argparse
import contextlib
import csv
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / "Tools" / "gesture_host.py"
SPEC = importlib.util.spec_from_file_location("gesture_host", MODULE_PATH)
host = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(host)


class ProtocolTests(unittest.TestCase):
    def test_raw_units_and_bounds(self):
        sample = host.parse_line("RAW,4294967295,500,-1000,0,9807,250,-500,0\r\n")
        self.assertEqual(sample["seq"], 0xFFFFFFFF)
        self.assertEqual(sample["ax"], -1.0)
        self.assertEqual(sample["az"], 9.807)
        self.assertEqual(sample["gy"], -0.5)
        self.assertNotIn("key_down", sample)
        for line in ("RAW,1,2,3", "RAW,-1,0,0,0,0,0,0,0", "RAW,1,0,nan,0,0,0,0,0"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                host.parse_line(line)

    def test_raw_physical_key_state(self):
        for field, expected in (("0", False), ("1", True)):
            sample = host.parse_line(f"RAW,1,500,-1000,0,9807,250,-500,0,{field}")
            self.assertIs(sample["key_down"], expected)
            self.assertEqual(sample["gz"], 0.0)
        for field in ("", "2", "-1", "-0", "01", "true", "false", "1.0", " 1", "1,0"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                host.parse_line(f"RAW,1,500,-1000,0,9807,250,-500,0,{field}")

    def test_event_status_unknown(self):
        event = host.parse_line("EVENT,100,8,125000,900000,650")
        self.assertEqual((event["id"], event["distance"], event["duration_ms"]), (8, 0.125, 650))
        status = host.parse_line("STATUS,10,1,3,1,200,4,0,5,6,750,45")
        self.assertEqual(status["max_read_us"], 45)
        self.assertEqual(status["sample_drops"], 4)
        self.assertEqual(host.parse_line("UNKNOWN,77,TOO_SHORT")["reason"], "TOO_SHORT")
        self.assertEqual(host.parse_line("STATE,ARMED")["fields"], ["ARMED"])
        with self.assertRaises(ValueError):
            host.parse_line("EVENT,0,9,0,0,0")

    def test_fragmented_and_overlong_input(self):
        framer = host.LineFramer(limit=12)
        self.assertEqual(framer.feed(b"STATE,"), [])
        self.assertEqual(framer.feed(b"IDLE\r\n"), ["STATE,IDLE"])
        self.assertEqual(framer.feed(b"x" * 100), [])
        self.assertEqual(framer.feed(b"tail\nHELLO\n"), ["HELLO"])
        self.assertEqual(framer.dropped, 1)

    def test_live_match_is_distinct_from_accepted_event(self):
        match = host.parse_line("MATCH,100,8,125000,900000,650")
        self.assertEqual(match["kind"], "MATCH")
        self.assertEqual((match["id"], match["distance"], match["duration_ms"]), (8, 0.125, 650))
        self.assertEqual(host.parse_line("MATCH,101,0,0,0,0")["id"], 0)
        for line in ("EVENT,101,0,0,0,0", "MATCH,101,9,0,0,0", "MATCH,101,1,0"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                host.parse_line(line)

    def test_sequence_gaps_rollover_reset_and_event_dedup(self):
        stats = host.CaptureStats()
        for sequence in (0xFFFFFFFE, 1, 1, 0, 1):
            stats.accept({"kind": "RAW", "seq": sequence})
        self.assertEqual(stats.raw_gaps, 2)
        self.assertEqual(stats.raw_duplicates, 1)
        self.assertEqual(stats.raw_resets, 1)
        event = {"kind": "EVENT", "t": 10, "id": 1}
        self.assertTrue(stats.accept(event))
        self.assertFalse(stats.accept(event))
        self.assertEqual(stats.events, 1)
        self.assertEqual(stats.duplicate_events, 1)

    def test_only_board_commands(self):
        self.assertEqual(host.validate_command("learn 8"), "learn 8")
        for command in ("arm\nstatus", "learn 0", "delete 9", "echo hello", "status; dir"):
            with self.subTest(command=command), self.assertRaises(argparse.ArgumentTypeError):
                host.validate_command(command)

    def test_no_implicit_arm_or_hotkeys(self):
        args = host.build_parser().parse_args(["--port", "COM7"])
        self.assertEqual(args.command, [])
        self.assertIsNone(args.hotkeys)
        self.assertFalse(args.keep_armed)
        self.assertFalse(args.disarm_on_exit)

    def test_board_config_and_actual_rgb(self):
        self.assertEqual(host.parse_line("CONFIG,4,2"), {"kind": "CONFIG", "class_limit": 4, "demo_target": 2})
        self.assertEqual(host.parse_line("CONFIG,8,20"), {"kind": "CONFIG", "class_limit": 8, "demo_target": 20})
        self.assertEqual(host.parse_line("DEMOLIMIT,20"), {"kind": "DEMOLIMIT", "demo_limit": 20})
        self.assertEqual(host.parse_line("COLOR,8,18A000"), {"kind": "COLOR", "id": 8, "color": "#18a000"})
        for line in ("CONFIG,0,1", "CONFIG,9,1", "CONFIG,8,0", "CONFIG,8,21", "CONFIG,8", "DEMOLIMIT,0", "DEMOLIMIT,21", "DEMOLIMIT,20,3", "COLOR,0,000000", "COLOR,1,FFF", "COLOR,1,GGGGGG"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                host.parse_line(line)

    def test_rgb_hold_timing_bounds_and_step(self):
        for duration in (100, 3000, 30000):
            self.assertEqual(host.parse_line(f"TIMING,{duration}"),
                             {"kind": "TIMING", "rgb_hold_ms": duration})
        for line in ("TIMING", "TIMING,0", "TIMING,99", "TIMING,30001",
                     "TIMING,150", "TIMING,NaN", "TIMING,3.0", "TIMING,100,200"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                host.parse_line(line)

    def test_optional_serial_buffers_are_capability_checked_and_nonfatal(self):
        self.assertIsNone(host.configure_serial_buffers(object()))
        calls = []
        port = SimpleNamespace(set_buffer_size=lambda **values: calls.append(values))
        self.assertIsNone(host.configure_serial_buffers(port))
        self.assertEqual(calls, [{"rx_size": 65536, "tx_size": 4096}])
        def fail(**values):
            raise OSError("driver rejected buffer sizing")
        self.assertEqual(host.configure_serial_buffers(SimpleNamespace(set_buffer_size=fail)), "driver rejected buffer sizing")


class HotkeyTests(unittest.TestCase):
    def test_white_list(self):
        self.assertEqual(host.validate_hotkey("Ctrl+SHIFT+k"), (0x11, 0x10, 0x4B))
        self.assertEqual(host.validate_hotkey("F12"), (0x7B,))
        self.assertEqual(host.validate_mapping({"8": "alt+left"}), {8: (0x12, 0x25)})
        for chord in ("", "ctrl", "ctrl++a", "ctrl+ctrl+a", "enter", "f13", "a+b", "ctrl+run.exe", ["a"]):
            with self.subTest(chord=chord), self.assertRaises(ValueError):
                host.validate_hotkey(chord)
        with self.assertRaises(ValueError):
            host.validate_mapping({"0": "a"})

    def test_release_in_reverse_order(self):
        chord = host.validate_hotkey("ctrl+alt+k")
        observed = []
        host.emit_hotkey(chord, lambda key, up: observed.append((key, up)))
        self.assertEqual(observed, [(0x11, False), (0x12, False), (0x4B, False),
                                    (0x4B, True), (0x12, True), (0x11, True)])
        self.assertEqual(observed, host.hotkey_events(chord))

    def test_release_after_press_failure(self):
        observed = []

        def fake_sender(key, up):
            observed.append((key, up))
            if key == 0x4B and not up:
                raise OSError("injected failure")

        with self.assertRaises(OSError):
            host.emit_hotkey(host.validate_hotkey("ctrl+k"), fake_sender)
        self.assertEqual(observed[-2:], [(0x4B, True), (0x11, True)])

    def test_one_failed_release_does_not_skip_remaining_releases(self):
        observed = []

        def fake_sender(key, up):
            observed.append((key, up))
            if key == 0x4B and up:
                raise OSError("injected release failure")

        with self.assertRaises(OSError):
            host.emit_hotkey(host.validate_hotkey("ctrl+k"), fake_sender)
        self.assertEqual(observed[-1], (0x11, True))


class CaptureTests(unittest.TestCase):
    def test_complete_capture_and_safe_exit_without_keyboard(self):
        class FakePort:
            in_waiting = 4096

            def __init__(self):
                self.writes = []
                self.read_once = False
                self.closed = False

            def reset_input_buffer(self):
                pass

            def write(self, data):
                self.writes.append(data)
                return len(data)

            def read(self, count):
                if self.read_once:
                    raise KeyboardInterrupt
                self.read_once = True
                return (b"RAW,1,5,1000,0,9807,0,0,0\nRAW,3,15,2000,0,9807,0,0,0\n"
                        b"EVENT,17,1,125000,500000,650\nUNKNOWN,20,DISTANCE\n"
                        b"STATE,ARMED\nSTATUS,25,1,1,1,3,0,0,1,0,100,10\n")

            def close(self):
                self.closed = True

        port = FakePort()
        module = SimpleNamespace(Serial=lambda *args, **kwargs: port, SerialException=OSError)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "samples.csv"
            args = host.build_parser().parse_args(["--port", "FAKE", "--output", str(output), "--label", "circle"])
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(host.capture(args, module, {}, None), 0)
            with output.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["az"], "9.807")
            self.assertEqual(rows[0]["label"], "circle")
            summary = json.loads(output.with_suffix(".summary.json").read_text())
            self.assertEqual(summary["raw_sequence_gaps"], 1)
            self.assertEqual(summary["recognized_events"], 1)
            self.assertEqual(summary["unknown_events"], 1)
            self.assertEqual(summary["reason"], "interrupted")
            self.assertEqual(summary["hotkeys_sent"], 0)
            lines = output.with_suffix(".events.jsonl").read_text().splitlines()
            self.assertEqual(len(lines), 4)
            self.assertEqual(json.loads(lines[0])["raw"], "EVENT,17,1,125000,500000,650")
            self.assertEqual(port.writes, [b"stream 0\n", b"status\n", b"list\n", b"stream 1\n", b"stream 0\n"])
            self.assertTrue(port.closed)

    def test_reset_failure_still_closes_and_records_error(self):
        class BrokenPort:
            closed = False

            def reset_input_buffer(self):
                raise OSError("disconnected")

            def write(self, data):
                raise OSError("disconnected")

            def close(self):
                self.closed = True

        port = BrokenPort()
        module = SimpleNamespace(Serial=lambda *args, **kwargs: port, SerialException=OSError)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "failed.csv"
            args = host.build_parser().parse_args(["--port", "FAKE", "--output", str(output)])
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(host.capture(args, module, {}, None), 1)
            summary = json.loads(output.with_suffix(".summary.json").read_text())
            self.assertEqual(summary["reason"], "error")
            self.assertEqual(len(summary["exit_command_errors"]), 1)
            self.assertTrue(port.closed)


if __name__ == "__main__":
    unittest.main()
