"""Record DM-MC02 Gesture data; optional explicit Windows hotkey mapping.

Example mapping: {"1": "ctrl+shift+k", "2": "alt+left", "3": "space"}.
Without --hotkeys, EVENT records never generate keyboard input.
"""

from __future__ import annotations

import argparse
from collections import deque
import csv
import ctypes
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import sys
import time


UINT32_MAX = 0xFFFFFFFF
STATUS_FIELDS = (
    "t", "armed", "classes", "calibrated", "samples", "sample_drops",
    "imu_errors", "unknown", "usb_drops", "max_feed_us", "max_read_us",
)
AXES = ("ax", "ay", "az", "gx", "gy", "gz")
CSV_FIELDS = (
    "host_utc", "host_monotonic_s", "user", "session", "label", "speed",
    "seq", "t", *AXES,
)
MODIFIERS = {"ctrl": 0x11, "alt": 0x12, "shift": 0x10, "win": 0x5B}
KEYS = {
    **{chr(code): code - 32 for code in range(ord("a"), ord("z") + 1)},
    **{str(number): 0x30 + number for number in range(10)},
    **{f"f{number}": 0x6F + number for number in range(1, 13)},
    "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28, "space": 0x20,
}
HOTKEY_EXAMPLE = {"1": "ctrl+shift+k", "2": "alt+left", "3": "space"}


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def configure_serial_buffers(connection):
    """Allow the bounded stroke smoother to run without exhausting Windows RX."""
    configure = getattr(connection, "set_buffer_size", None)
    if not callable(configure):
        return None
    try:
        configure(rx_size=65536, tx_size=4096)
    except Exception as error:
        return str(error)
    return None


def integer(value, minimum=0, maximum=UINT32_MAX):
    if not re.fullmatch(r"-?[0-9]+", value):
        raise ValueError(f"invalid integer: {value!r}")
    number = int(value)
    if not minimum <= number <= maximum:
        raise ValueError(f"integer out of range: {value!r}")
    return number


def parse_line(line):
    """Parse the version-1 ASCII wire protocol, without any side effects."""
    fields = line.rstrip("\r\n").split(",")
    kind = fields[0]
    if not re.fullmatch(r"[A-Z_]+", kind):
        raise ValueError("invalid record type")
    record = {"kind": kind}
    if kind == "RAW":
        if len(fields) not in (9, 10):
            raise ValueError("RAW requires sequence, timestamp, six axes and optional KEY state")
        record.update(seq=integer(fields[1]), t=integer(fields[2]))
        for axis, value in zip(AXES, fields[3:9]):
            record[axis] = integer(value, -0x80000000, 0x7FFFFFFF) / 1000.0
        if len(fields) == 10:
            if fields[9] not in ("0", "1"):
                raise ValueError("RAW KEY state must be 0 or 1")
            record["key_down"] = fields[9] == "1"
    elif kind in {"EVENT", "MATCH"}:
        if len(fields) != 6:
            raise ValueError(f"{kind} requires five numeric fields")
        record.update(
            t=integer(fields[1]), id=integer(fields[2], 0 if kind == "MATCH" else 1, 8),
            distance=integer(fields[3]) / 1000000.0,
            second_distance=integer(fields[4]) / 1000000.0,
            duration_ms=integer(fields[5]),
        )
    elif kind == "STATUS":
        if len(fields) != len(STATUS_FIELDS) + 1:
            raise ValueError("STATUS requires eleven numeric fields")
        record.update(zip(STATUS_FIELDS, map(integer, fields[1:])))
        if record["armed"] > 1 or record["calibrated"] > 1 or record["classes"] > 8:
            raise ValueError("invalid STATUS state")
    elif kind == "UNKNOWN":
        if len(fields) < 3 or not fields[2]:
            raise ValueError("UNKNOWN requires timestamp and reason")
        record.update(t=integer(fields[1]), reason=",".join(fields[2:]))
    elif kind == "CONFIG":
        if len(fields) != 3:
            raise ValueError("CONFIG requires class limit and demonstration target")
        record.update(class_limit=integer(fields[1], 1, 8), demo_target=integer(fields[2], 1, 20))
    elif kind == "DEMOLIMIT":
        if len(fields) != 2:
            raise ValueError("DEMOLIMIT requires maximum demonstration count")
        record["demo_limit"] = integer(fields[1], 1, 20)
    elif kind == "TIMING":
        if len(fields) != 2:
            raise ValueError("TIMING requires RGB hold duration")
        duration = integer(fields[1], 100, 30000)
        if duration % 100:
            raise ValueError("RGB hold duration must use 100 ms steps")
        record["rgb_hold_ms"] = duration
    elif kind == "COLOR":
        if len(fields) != 3 or not re.fullmatch(r"[0-9a-fA-F]{6}", fields[2]):
            raise ValueError("COLOR requires slot and six-digit RGB hex")
        record.update(id=integer(fields[1], 1, 8), color="#" + fields[2].lower())
    else:
        record["fields"] = fields[1:]
    return record


class LineFramer:
    """Keep partial serial reads; discard overlong lines through the next LF."""

    def __init__(self, limit=512):
        self.limit = limit
        self.pending = bytearray()
        self.discarding = False
        self.dropped = 0

    def feed(self, data):
        lines = []
        for byte in data:
            if byte == 10:
                if not self.discarding and self.pending:
                    lines.append(self.pending.decode("ascii", errors="replace").rstrip("\r"))
                self.pending.clear()
                self.discarding = False
            elif not self.discarding:
                self.pending.append(byte)
                if len(self.pending) > self.limit:
                    self.pending.clear()
                    self.discarding = True
                    self.dropped += 1
        return lines


def validate_command(command):
    if command in {"list", "status", "arm", "disarm", "cancel", "save", "calibrate", "stream 0", "stream 1"}:
        return command
    if re.fullmatch(r"(?:learn|delete) [1-8]", command):
        return command
    raise argparse.ArgumentTypeError("unsupported board command")


def validate_hotkey(value):
    if not isinstance(value, str):
        raise ValueError("hotkeys must be strings such as ctrl+shift+k")
    tokens = tuple(part.strip().lower() for part in value.split("+"))
    if not tokens or len(tokens) > 5 or len(set(tokens)) != len(tokens):
        raise ValueError(f"invalid hotkey: {value!r}")
    if tokens[-1] not in KEYS or any(token not in MODIFIERS for token in tokens[:-1]):
        raise ValueError("use optional ctrl/alt/shift/win followed by one letter, digit, F1-F12, arrow or space")
    return tuple(MODIFIERS[token] if token in MODIFIERS else KEYS[token] for token in tokens)


def validate_mapping(mapping):
    if not isinstance(mapping, dict):
        raise ValueError("hotkey JSON must be an object with gesture IDs 1..8")
    result = {}
    for gesture_id, chord in mapping.items():
        if gesture_id not in tuple(str(value) for value in range(1, 9)):
            raise ValueError(f"invalid gesture ID: {gesture_id!r}")
        result[int(gesture_id)] = validate_hotkey(chord)
    return result


def hotkey_events(chord):
    return [(key, False) for key in chord] + [(key, True) for key in reversed(chord)]


def emit_hotkey(chord, sender):
    """sender(vk, key_up); release every attempted key, including on failure."""
    attempted = []
    release_error = None
    try:
        for key in chord:
            attempted.append(key)
            sender(key, False)
    finally:
        for key in reversed(attempted):
            try:
                sender(key, True)
            except OSError as error:
                if release_error is None:
                    release_error = error
        if release_error is not None:
            raise release_error


class WindowsKeyboard:
    def __init__(self):
        if sys.platform != "win32":
            raise OSError("--hotkeys requires Windows")
        word, dword, long = ctypes.c_uint16, ctypes.c_uint32, ctypes.c_int32
        pointer = ctypes.c_size_t

        class KeyboardInput(ctypes.Structure):
            _fields_ = [("vk", word), ("scan", word), ("flags", dword), ("time", dword), ("extra", pointer)]

        class MouseInput(ctypes.Structure):
            _fields_ = [("dx", long), ("dy", long), ("data", dword), ("flags", dword), ("time", dword), ("extra", pointer)]

        class HardwareInput(ctypes.Structure):
            _fields_ = [("message", dword), ("low", word), ("high", word)]

        class Payload(ctypes.Union):
            _fields_ = [("keyboard", KeyboardInput), ("mouse", MouseInput), ("hardware", HardwareInput)]

        class Input(ctypes.Structure):
            _fields_ = [("type", dword), ("payload", Payload)]

        self.input_type = Input
        self.api = ctypes.WinDLL("user32", use_last_error=True).SendInput
        self.api.argtypes = [ctypes.c_uint32, ctypes.POINTER(Input), ctypes.c_int]
        self.api.restype = ctypes.c_uint32

    def send(self, key, key_up):
        value = self.input_type()
        value.type = 1
        value.payload.keyboard.vk = key
        value.payload.keyboard.flags = (2 if key_up else 0) | (1 if key in (0x25, 0x26, 0x27, 0x28, 0x5B) else 0)
        if self.api(1, ctypes.byref(value), ctypes.sizeof(value)) != 1:
            raise OSError("SendInput failed; check the target application's privilege level")


class CaptureStats:
    def __init__(self):
        self.raw_count = self.raw_gaps = self.raw_duplicates = self.raw_resets = 0
        self.events = self.unknown = self.malformed = self.duplicate_events = 0
        self.hotkeys = 0
        self.last_seq = None
        self.last_status = None
        self.seen_events = deque(maxlen=256)

    def accept(self, record):
        kind = record["kind"]
        if kind == "RAW":
            self.raw_count += 1
            if self.last_seq is not None:
                delta = (record["seq"] - self.last_seq) & UINT32_MAX
                if delta == 0:
                    self.raw_duplicates += 1
                elif delta >= 0x80000000:
                    self.raw_resets += 1
                else:
                    self.raw_gaps += delta - 1
            self.last_seq = record["seq"]
        elif kind == "EVENT":
            identity = (record["t"], record["id"])
            if identity in self.seen_events:
                self.duplicate_events += 1
                return False
            self.seen_events.append(identity)
            self.events += 1
        elif kind == "UNKNOWN":
            self.unknown += 1
        elif kind == "STATUS":
            self.last_status = record
        return True


def positive_duration(value):
    duration = float(value)
    if not math.isfinite(duration) or duration <= 0:
        raise argparse.ArgumentTypeError("duration must be positive and finite")
    return duration


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial device, for example COM7")
    parser.add_argument("--output", type=Path, default=Path("dataset.csv"))
    parser.add_argument("--label", default="unknown")
    parser.add_argument("--session", default=datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    parser.add_argument("--user", default="anonymous")
    parser.add_argument("--speed", choices=("slow", "normal", "fast", "unknown"), default="unknown")
    parser.add_argument("--duration", type=positive_duration, help="capture for this many seconds")
    parser.add_argument("--command", action="append", type=validate_command, default=[], help="startup board command; repeatable")
    parser.add_argument("--hotkeys", type=Path, help="explicitly enable Windows keyboard events from this JSON map")
    exit_mode = parser.add_mutually_exclusive_group()
    exit_mode.add_argument("--keep-armed", action="store_true", help="compatibility option; recognition is left running by default")
    exit_mode.add_argument("--disarm-on-exit", action="store_true", help="explicitly send disarm when exiting (legacy firmware only)")
    parser.add_argument("--print-hotkey-example", action="store_true")
    return parser


def capture(args, serial_module, mapping, keyboard):
    output = args.output.resolve()
    events_path = output.with_suffix(".events.jsonl")
    summary_path = output.with_suffix(".summary.json")
    if len({output, events_path, summary_path}) != 3:
        raise ValueError("output filename collides with an event or summary filename")
    for path in (output, events_path, summary_path):
        if path.exists():
            raise ValueError(f"refusing to overwrite {path}")
    output.parent.mkdir(parents=True, exist_ok=True)
    stats, framer = CaptureStats(), LineFramer()
    metadata = {key: getattr(args, key) for key in ("user", "session", "label", "speed")}
    start = time.monotonic()
    started_utc = utc_now()
    reason, failure, connection = "duration", None, None
    exit_commands_sent, exit_command_errors = [], []
    last_status = last_flush = start
    try:
        with output.open("x", encoding="utf-8", newline="") as csv_file, events_path.open("x", encoding="utf-8") as event_file:
            writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
            writer.writeheader()
            try:
                connection = serial_module.Serial(args.port, baudrate=115200, timeout=0.2, write_timeout=1)
                buffer_error = configure_serial_buffers(connection)
                if buffer_error:
                    print(f"Serial buffer sizing unavailable: {buffer_error}", file=sys.stderr)

                def command(text):
                    data = (text + "\n").encode("ascii")
                    if connection.write(data) != len(data):
                        raise OSError("incomplete serial command write")

                connection.reset_input_buffer()
                command("stream 0")
                command("status")
                command("list")
                command("stream 1")
                for text in args.command:
                    command(text)
                print(f"Recording to {output}; keyboard mapping {'enabled' if mapping else 'disabled'}", flush=True)
                while args.duration is None or time.monotonic() - start < args.duration:
                    data = connection.read(min(max(connection.in_waiting, 1), 4096))
                    for line in framer.feed(data):
                        host_utc, host_time = utc_now(), time.monotonic()
                        try:
                            record = parse_line(line)
                        except ValueError as error:
                            stats.malformed += 1
                            event_file.write(json.dumps({"host_utc": host_utc, "raw": line, "parse_error": str(error), **metadata}) + "\n")
                            continue
                        fresh = stats.accept(record)
                        if record["kind"] == "RAW":
                            writer.writerow({"host_utc": host_utc, "host_monotonic_s": f"{host_time:.6f}", **metadata, **{key: record[key] for key in ("seq", "t", *AXES)}})
                            continue
                        event_file.write(json.dumps({"host_utc": host_utc, "host_monotonic_s": host_time, "raw": line, "record": record, **metadata}) + "\n")
                        if record["kind"] == "EVENT" and fresh and record["id"] in mapping:
                            emit_hotkey(mapping[record["id"]], keyboard.send)
                            stats.hotkeys += 1
                        if record["kind"] == "STATUS":
                            print(f"STATUS armed={record['armed']} classes={record['classes']} raw={stats.raw_count} gaps={stats.raw_gaps} events={stats.events} unknown={stats.unknown} board_drops={record['sample_drops']} imu_errors={record['imu_errors']} usb_drops={record['usb_drops']} feed_max={record['max_feed_us']}us read_max={record['max_read_us']}us", flush=True)
                    now = time.monotonic()
                    if now - last_status >= 5:
                        command("status")
                        last_status = now
                    if now - last_flush >= 1:
                        csv_file.flush()
                        event_file.flush()
                        last_flush = now
            except KeyboardInterrupt:
                reason = "interrupted"
            except (serial_module.SerialException, OSError) as error:
                reason, failure = "error", str(error)
                print(f"Stopped: {error}; automatic reconnect is disabled.", file=sys.stderr)
            finally:
                if connection is not None:
                    for text in (["stream 0", "disarm"] if args.disarm_on_exit else ["stream 0"]):
                        try:
                            command(text)
                            exit_commands_sent.append(text)
                        except (serial_module.SerialException, OSError) as error:
                            exit_command_errors.append({"command": text, "error": str(error)})
                            print(f"Exit command {text!r} could not be delivered: {error}", file=sys.stderr)
                    try:
                        connection.close()
                    except (serial_module.SerialException, OSError) as error:
                        reason, failure = "error", str(error)
    finally:
        summary = {
            "started_utc": started_utc, "finished_utc": utc_now(),
            "elapsed_s": round(time.monotonic() - start, 3), "port": args.port,
            "reason": reason, "error": failure, **metadata,
            "acceleration_unit": "m/s^2", "gyro_unit": "rad/s", "board_time_unit": "ms",
            "raw_count": stats.raw_count, "raw_sequence_gaps": stats.raw_gaps,
            "raw_duplicates": stats.raw_duplicates, "raw_sequence_resets": stats.raw_resets,
            "recognized_events": stats.events, "unknown_events": stats.unknown,
            "duplicate_events": stats.duplicate_events, "malformed_lines": stats.malformed,
            "overlong_lines": framer.dropped, "partial_bytes_at_exit": len(framer.pending),
            "hotkeys_sent": stats.hotkeys, "last_board_status": stats.last_status,
            "exit_commands_sent": exit_commands_sent, "exit_command_errors": exit_command_errors,
        }
        with summary_path.open("x", encoding="utf-8") as summary_file:
            json.dump(summary, summary_file, indent=2)
            summary_file.write("\n")
        print(f"Finished: raw={stats.raw_count}, gaps={stats.raw_gaps}, events={stats.events}, unknown={stats.unknown}. Summary: {summary_path}", flush=True)
    return 1 if failure else 0


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.print_hotkey_example:
        print(json.dumps(HOTKEY_EXAMPLE, indent=2))
        return 0
    if not args.port:
        parser.error("--port is required")
    try:
        import serial
    except ImportError:
        parser.error("pyserial is missing; install Tools/requirements.txt")
    try:
        mapping = validate_mapping(json.loads(args.hotkeys.read_text(encoding="utf-8-sig"))) if args.hotkeys else {}
        keyboard = WindowsKeyboard() if mapping else None
        return capture(args, serial, mapping, keyboard)
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    sys.exit(main())
