"""Threaded board controller shared by the local HTTP UI and its tests."""

from __future__ import annotations

from collections import deque
import copy
import csv
from datetime import datetime, timezone
import importlib.util
import json
import logging
import math
from pathlib import Path
import queue
import re
import secrets
import threading
import time

from pose import PoseEstimator
from pen_motion import PenMotion


_host_spec = importlib.util.spec_from_file_location(
    "gesture_host", Path(__file__).resolve().parents[1] / "Tools" / "gesture_host.py"
)
_host = importlib.util.module_from_spec(_host_spec)
_host_spec.loader.exec_module(_host)
parse_line = _host.parse_line
LineFramer = _host.LineFramer
validate_hotkey = _host.validate_hotkey
WindowsKeyboard = _host.WindowsKeyboard
emit_hotkey = _host.emit_hotkey
AXES = _host.AXES
HANDSHAKE_TIMEOUT_SECONDS = 3.0
SERIAL_RETRY_DELAYS = (0.35, 0.7, 1.4)
SLOT_COLORS = (
    ("#ff0000", "\u7ea2"), ("#00ff00", "\u7eff"),
    ("#0000ff", "\u84dd"), ("#ffff00", "\u9ec4"),
    ("#00ffff", "\u9752"), ("#ff00ff", "\u54c1\u7ea2"),
    ("#ff5500", "\u6a59"), ("#ffffff", "\u767d"),
)


def _utc():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def _integer(value, low=0, high=0xFFFFFFFF):
    if isinstance(value, bool) or not re.fullmatch(r"[0-9]+", str(value)):
        raise ValueError("Expected an integer")
    result = int(value)
    if not low <= result <= high:
        raise ValueError(f"Expected a value from {low} to {high}")
    return result


class Controller:
    """Own all serial IO in one worker; snapshots never wait for a serial read."""

    def __init__(self, data_dir=None, serial_factory=None, ports_provider=None,
                 keyboard_factory=None):
        self.data_dir = Path(data_dir or Path(__file__).resolve().parent / "data")
        self.capture_dir = self.data_dir / "captures"
        self._serial_factory = serial_factory
        self._ports_provider = ports_provider
        self._keyboard_factory = keyboard_factory or WindowsKeyboard
        self._lock = threading.RLock()
        self._lifecycle = threading.Lock()
        self._stop = threading.Event()
        self._commands = queue.Queue(maxsize=64)
        self._worker = None
        self._serial = None
        self._keyboard = None
        self._cursor = 0
        self._events = deque(maxlen=150)
        self._waveform = deque(maxlen=300)
        self._seen_events = deque(maxlen=256)
        self._settings = self._load_settings()
        self._capture = None
        self._last_capture = {"active": False}
        self._epoch = 0
        self._hotkey_fence = None
        self._demo_key_started = None
        self._pose = PoseEstimator()
        self._pen_motion = PenMotion()
        self._last_pose_sample = None
        self._pen_samples = deque(maxlen=256)
        self._pen_cursor = 0
        self._pen_epoch = 0
        self._connection = {"state": "disconnected", "port": "", "demo": False,
                            "simulation": False, "error": None, "error_detail": None,
                            "retry_attempt": 0, "retry_limit": len(SERIAL_RETRY_DELAYS), "epoch": 0}
        self._reset_live()

    def _load_settings(self):
        defaults = {str(i): {"name": f"\u52a8\u4f5c {i:02d}", "hotkey": ""} for i in range(1, 9)}
        path = self.data_dir / "settings.json"
        try:
            saved = json.loads(path.read_text(encoding="utf-8"))
            for key, item in saved.get("slots", {}).items():
                if key not in defaults or not isinstance(item, dict):
                    continue
                name, hotkey = item.get("name", defaults[key]["name"]), item.get("hotkey", "")
                if not isinstance(name, str) or not 1 <= len(name.strip()) <= 48:
                    continue
                if hotkey:
                    validate_hotkey(hotkey)
                defaults[key] = {"name": name.strip(), "hotkey": hotkey}
        except (OSError, ValueError, TypeError, AttributeError):
            pass
        return defaults

    def _reset_live(self):
        self._status = {"armed": 0, "calibrated": 0, "classes": 0, "samples": 0,
                        "sample_drops": 0, "imu_errors": 0, "unknown": 0,
                        "usb_drops": 0, "max_feed_us": 0, "max_read_us": 0}
        self._protocol = {"version": None, "inventory": False, "manual_training": False,
                          "firmware": None, "capabilities": [], "requires_calibration": False}
        self._slots = [{"id": i, "state": "unknown", "templates": None,
                        "board_name": ""} for i in range(1, 9)]
        self._training = {"state": "idle", "slot": None, "collected": 0,
                          "required": 3, "message": "", "capturing": False, "warning": None}
        self._live_match = {"id": 0, "distance": None, "second_distance": None,
                            "duration_ms": 0, "t": None}
        self._last_match = None
        self._recent_match = None
        self._last_confident_match = None
        self._pending_delete = None
        self._hotkeys_enabled = False
        self._hotkey_fence = None
        self._seen_events.clear()
        self._waveform.clear()
        self._last_wave_seq = None
        self._counters = {"raw": 0, "events": 0, "unknown": 0, "malformed": 0,
                          "hotkeys": 0, "duplicate_events": 0, "raw_gaps": 0}
        self._last_raw_seq = None
        self._board_clock = None
        self._demo_key_started = None
        self._demo_key_down = False
        self._key = {"available": False, "down": None}
        self._pose.reset()
        self._pen_motion.reset()
        self._last_pose_sample = None
        self._reset_pen()

    def _reset_pen(self):
        self._pen_samples.clear()
        self._pen_epoch += 1
        self._last_pen_t = None
        self._last_pen_wall = None
        self._last_pen_key = None
        self._last_pen_raw_seq = None

    def _append_pen_sample(self, record):
        tick = record["t"]
        key_down = record.get("key_down")
        self._key = {"available": isinstance(key_down, bool), "down": key_down}
        now = time.monotonic()
        delta = None if self._last_pen_t is None else (tick - self._last_pen_t) & 0xFFFFFFFF
        missing_raw = (self._last_pen_raw_seq is not None and
                       (record["seq"] - self._last_pen_raw_seq) & 0xFFFFFFFF != 1)
        if delta is not None and (delta > 100 or now - self._last_pen_wall > 0.5 or missing_raw):
            self._reset_pen()
            delta = None
        self._last_pen_raw_seq = record["seq"]
        # KEY transitions bypass 50 Hz decimation so a complete short stroke
        # between HTTP polls still has separate press and release boundaries.
        if delta is not None and delta < 20 and key_down == self._last_pen_key:
            return
        # Both views use one gravity-aligned trajectory and the same origin.
        motion = self._pose.world_motion()
        pen = self._pen_motion.snapshot()
        corrections = self._pen_motion.corrections() if key_down is False and self._last_pen_key is True else []
        corrected_trail = next((item["points"] for item in reversed(corrections)
                                if item["stroke_id"] == pen["stroke_id"]), None)
        self._pose.set_translation(pen["position_m"], pen["velocity_m_s"],
                                   corrected_trail)
        self._pen_cursor += 1
        self._pen_samples.append({"seq": self._pen_cursor, "t": tick,
                                  "q": motion["quaternion"], "p": pen["position_m"],
                                  "stroke_id": pen["stroke_id"],
                                  "key_down": key_down})
        self._last_pen_t, self._last_pen_wall = tick, now
        self._last_pen_key = key_down

    def _log(self, kind, message, **extra):
        self._cursor += 1
        event = {"seq": self._cursor, "time": _utc(), "kind": kind,
                 "message": message, "simulation": self._connection["simulation"],
                 "epoch": self._epoch, **extra}
        self._events.append(event)
        return event

    def list_ports(self):
        if self._ports_provider:
            ports = self._ports_provider()
        else:
            try:
                from serial.tools import list_ports
            except ImportError:
                return []
            ports = list_ports.comports()
        return [{"device": p["device"], "description": p.get("description", ""),
                 "hwid": p.get("hwid", "")} if isinstance(p, dict) else
                {"device": p.device, "description": p.description or "",
                 "hwid": p.hwid or ""} for p in ports]

    def connect(self, port="", demo=False):
        identity = ""
        if not isinstance(demo, bool):
            raise ValueError("demo must be a boolean")
        if not demo:
            if not isinstance(port, str) or not port.strip():
                raise ValueError("Select a serial port first")
            selected = next((item for item in self.list_ports() if item["device"] == port), None)
            if selected is None:
                raise ValueError("Serial port is unavailable; refresh the port list")
            identity = selected["hwid"]
        with self._lifecycle:
            self._disconnect()
            with self._lock:
                self._reset_live()
                self._epoch += 1
                self._connection = {"state": "connecting", "port": "DEMO" if demo else port,
                                    "demo": demo, "simulation": demo, "error": None,
                                    "error_detail": None, "retry_attempt": 0,
                                    "retry_limit": len(SERIAL_RETRY_DELAYS),
                                    "epoch": self._epoch}
                self._stop = threading.Event()
                self._commands = queue.Queue(maxsize=64)
                self._worker = threading.Thread(target=self._run_demo if demo else self._run_serial,
                                                args=() if demo else (port, identity), daemon=True,
                                                name="gesture-board")
                self._worker.start()
            return self.snapshot()

    def disconnect(self):
        with self._lifecycle:
            self._disconnect()
        return self.snapshot()

    def _disconnect(self):
        self._stop.set()
        worker = self._worker
        if worker and worker is not threading.current_thread():
            worker.join(timeout=3)
            if worker.is_alive():
                raise RuntimeError("Serial worker is still stopping; try again shortly")
        with self._lock:
            self._worker = None
            self._finish_capture("disconnect")
            self._reset_live()
            self._connection.update(state="disconnected", error=None, error_detail=None,
                                    retry_attempt=0)

    def close(self):
        self.disconnect()

    def snapshot(self, after=0):
        after = _integer(after, 0, 2**63 - 1)
        with self._lock:
            slots = [{**slot, **self._settings[str(slot["id"])],
                      "color": SLOT_COLORS[slot["id"] - 1][0],
                      "color_name": SLOT_COLORS[slot["id"] - 1][1]} for slot in self._slots]
            pose = self._pose.snapshot()
            key = dict(self._key)
            if self._last_pose_sample is not None and time.monotonic() - self._last_pose_sample > 0.5:
                pose["status"] = "gap"
                key = {"available": False, "down": None}
            pen_motion = self._pen_motion.snapshot()
            pose["position_m"] = pen_motion["position_m"].copy()
            pose["velocity_m_s"] = pen_motion["velocity_m_s"].copy()
            pose["position_limited"] = pen_motion["reason"] in {"position_limit", "stroke_capacity"}
            pen_motion["available"] = bool(key["available"] and pose["available"] and
                                          self._last_pose_sample is not None and
                                          time.monotonic() - self._last_pose_sample <= 0.5)
            live_match = dict(self._live_match)
            if self._last_match is None or time.monotonic() - self._last_match > 0.8:
                live_match["id"] = 0
            recent_match = None
            if (self._status["armed"] and self._last_confident_match is not None
                    and time.monotonic() - self._last_confident_match <= 0.35):
                recent_match = self._recent_match
            return copy.deepcopy({
                "connection": self._connection, "status": self._status,
                "protocol": self._protocol, "slots": slots, "training": self._training,
                "pending_delete": self._pending_delete, "hotkeys_enabled": self._hotkeys_enabled,
                "counters": self._counters, "waveform": list(self._waveform),
                "events": [event for event in self._events if event["seq"] > after],
                "cursor": self._cursor, "capture": self._capture_state(),
                "pose": pose, "live_match": live_match, "recent_match": recent_match,
                "pen_samples": list(self._pen_samples), "pen_cursor": self._pen_cursor,
                "pen_epoch": self._pen_epoch,
                "pen_corrections": self._pen_motion.corrections(),
                "key": key,
                "pen_motion": pen_motion,
            })

    def reset_pose(self):
        with self._lock:
            self._pose.zero()
            self._pen_motion.reset()
            self._reset_pen()
            self._log("POSE", "Origin reset")
        return self.snapshot()

    def command(self, name, slot=None):
        if not isinstance(name, str):
            raise ValueError("Command must be a string")
        name = name.strip().lower()
        if slot is not None:
            name = f"{name} {_integer(slot, 1, 8)}"
        if name not in {"list", "status", "arm", "disarm", "cancel", "save",
                        "calibrate", "stream 0", "stream 1", "demo key 0", "demo key 1"} and not re.fullmatch(
                            r"(?:learn|delete) [1-8]", name):
            raise ValueError("Unsupported board command")
        with self._lock:
            if self._connection["state"] != "connected":
                raise ValueError("Connect a board or the simulator first")
            if name.startswith("demo key ") and not self._connection["demo"]:
                raise ValueError("Simulated KEY is available only on the demo device")
            if name.startswith("learn ") and not self._protocol["manual_training"]:
                raise ValueError("Update board firmware to support KEY-controlled learning")
            if name in {"disarm", "cancel", "calibrate"} or name.startswith(("learn ", "delete ")):
                self._hotkeys_enabled = False
            self._queue_command(name)
            self._log("COMMAND", name)
        return self.snapshot()

    def _queue_command(self, name):
        try:
            self._commands.put_nowait(name)
        except queue.Full as error:
            raise ValueError("Too many pending commands; wait for the board") from error

    def save_slot(self, slot, name, hotkey=""):
        slot = _integer(slot, 1, 8)
        if not isinstance(name, str) or not 1 <= len(name.strip()) <= 48:
            raise ValueError("Gesture name must contain 1 to 48 characters")
        if not isinstance(hotkey, str) or len(hotkey) > 80:
            raise ValueError("Invalid hotkey")
        hotkey = hotkey.strip().lower()
        if hotkey:
            validate_hotkey(hotkey)
        with self._lock:
            updated = {**self._settings, str(slot): {"name": name.strip(), "hotkey": hotkey}}
            self.data_dir.mkdir(parents=True, exist_ok=True)
            temporary = self.data_dir / "settings.tmp"
            temporary.write_text(json.dumps({"version": 1, "slots": updated},
                                            ensure_ascii=False, indent=2), encoding="utf-8")
            temporary.replace(self.data_dir / "settings.json")
            self._settings = updated
        return self.snapshot()

    def set_hotkeys(self, enabled):
        if not isinstance(enabled, bool):
            raise ValueError("enabled must be a boolean")
        with self._lock:
            if enabled:
                if self._connection["state"] != "connected" or self._connection["demo"]:
                    raise ValueError("Hotkeys require a real connected board")
                if "t" not in self._status:
                    raise ValueError("Wait for the board status before enabling hotkeys")
                if self._keyboard is None:
                    self._keyboard = self._keyboard_factory()
                stamp, received = self._board_clock
                self._hotkey_fence = (stamp + int((time.monotonic() - received) * 1000)) & 0xFFFFFFFF
            self._hotkeys_enabled = enabled
            self._log("HOTKEYS", "enabled" if enabled else "disabled")
        return self.snapshot()

    def action(self, payload):
        if not isinstance(payload, dict):
            raise ValueError("Expected a JSON object")
        action = payload.get("action", payload.get("command"))
        if action == "connect":
            return self.connect(payload.get("port", ""), payload.get("demo", False))
        if action == "disconnect":
            return self.disconnect()
        if action == "slot":
            return self.save_slot(payload.get("slot", payload.get("id")), payload.get("name"), payload.get("hotkey", ""))
        if action == "hotkeys":
            return self.set_hotkeys(payload.get("enabled"))
        if action == "capture_start":
            return self.capture_start(payload.get("metadata", {}))
        if action == "capture_stop":
            return self.capture_stop()
        if action == "command":
            return self.command(payload.get("name", payload.get("command")), payload.get("slot"))
        return self.command(action, payload.get("slot"))

    @staticmethod
    def _write(connection, command):
        data = (command + "\n").encode("ascii")
        if connection.write(data) != len(data):
            raise OSError("Incomplete serial command write")

    def _sync_commands(self):
        for command in ("stream 0", "disarm", "status", "list"):
            self._queue_command(command)

    @staticmethod
    def _close_serial(connection, orderly=False):
        if connection is None:
            return
        if orderly:
            for command in ("disarm", "stream 0"):
                try:
                    Controller._write(connection, command)
                except Exception:
                    pass
        try:
            connection.close()
        except Exception:
            pass

    @staticmethod
    def _retryable_serial_error(error, opened, retrying):
        detail = str(error).lower()
        # Windows wraps an invalid USB handle as PermissionError(13) here;
        # it is different from access denied when another process owns a port.
        if "clearcommerror" in detail:
            return True
        if "handshake timed out" in detail:
            return retrying
        if (getattr(error, "winerror", None) == 5 or
                getattr(error, "errno", None) == 13 or
                "permissionerror(13" in detail or "access is denied" in detail):
            return False
        return isinstance(error, OSError) and (opened or retrying)

    @staticmethod
    def _serial_error_message(error, exhausted=False):
        detail = str(error).lower()
        if "pyserial is missing" in detail:
            return "\u7f3a\u5c11\u4e32\u53e3\u7ec4\u4ef6\uff0c\u8bf7\u7528 start_gui.ps1 \u542f\u52a8\u3002"
        if "handshake timed out" in detail and not exhausted:
            return "\u4e32\u53e3\u5df2\u6253\u5f00\uff0c\u4f46\u677f\u7aef\u672a\u54cd\u5e94\u3002\u8bf7\u590d\u4f4d\u677f\u5b50\u540e\u91cd\u65b0\u8fde\u63a5\u3002"
        if not Controller._retryable_serial_error(error, True, True):
            if (getattr(error, "winerror", None) == 5 or
                    getattr(error, "errno", None) == 13 or "permissionerror(13" in detail):
                return "\u4e32\u53e3\u88ab\u5360\u7528\u6216\u8bbf\u95ee\u88ab\u62d2\u7edd\uff0c\u8bf7\u5173\u95ed\u5176\u4ed6\u4e32\u53e3\u8f6f\u4ef6\u540e\u91cd\u8bd5\u3002"
        if exhausted:
            return "USB \u8fde\u63a5\u4e2d\u65ad\uff0c\u81ea\u52a8\u91cd\u8fde\u672a\u6210\u529f\u3002\u8bf7\u91cd\u63d2 USB \u6216\u590d\u4f4d\u677f\u5b50\uff0c\u7136\u540e\u91cd\u65b0\u8fde\u63a5\u3002"
        return "\u4e32\u53e3\u8fde\u63a5\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5 USB \u8fde\u63a5\u540e\u91cd\u8bd5\u3002"

    def _recovery_port(self, port, identity):
        if not identity:
            return port
        ports = self.list_ports()
        same_port = next((item for item in ports if item["device"] == port), None)
        if same_port and same_port["hwid"] == identity:
            return port
        # Follow a renamed port only with the same unique USB serial identity.
        if "USB " in identity.upper() and re.search(r"\bSER=\S+", identity, re.IGNORECASE):
            matches = [item["device"] for item in ports if item["hwid"] == identity]
            if len(matches) == 1:
                return matches[0]
        raise FileNotFoundError("The selected USB board has not reappeared")

    def _run_serial(self, port, identity=""):
        connection = None
        attempts = 0
        try:
            factory = self._serial_factory
            if factory is None:
                try:
                    import serial
                except ImportError as error:
                    raise OSError("pyserial is missing; run the supplied launcher to install it") from error
                factory = serial.Serial
            while not self._stop.is_set():
                try:
                    if attempts:
                        port = self._recovery_port(port, identity)
                    connection = factory(port, baudrate=115200, timeout=0.05, write_timeout=0.25)
                    connection.reset_input_buffer()
                    with self._lock:
                        self._serial = connection
                        self._connection["port"] = port
                        self._log("CONNECTING", f"{port}: waiting for board status")
                        self._sync_commands()
                    self._serial_session(connection)
                    break
                except Exception as error:
                    opened = connection is not None
                    self._close_serial(connection)
                    connection = None
                    if self._stop.is_set():
                        break
                    retry = (attempts < len(SERIAL_RETRY_DELAYS) and
                             self._retryable_serial_error(error, opened, attempts > 0))
                    message = ("USB \u8fde\u63a5\u4e2d\u65ad\uff0c\u6b63\u5728\u91cd\u65b0\u8fde\u63a5\u3002" if retry else
                               self._serial_error_message(error, exhausted=attempts > 0))
                    logging.getLogger(__name__).warning("Serial connection failed on %s: %s", port, error,
                                                        exc_info=True)
                    with self._lock:
                        self._serial = None
                        self._finish_capture("connection_lost")
                        self._commands = queue.Queue(maxsize=64)
                        self._reset_live()
                        self._epoch += 1
                        self._connection.update(state="reconnecting" if retry else "error",
                                                error=message, error_detail=str(error), epoch=self._epoch,
                                                retry_attempt=attempts + 1 if retry else attempts)
                        self._log("RECONNECTING" if retry else "ERROR", message, detail=str(error))
                    if not retry:
                        break
                    delay = SERIAL_RETRY_DELAYS[attempts]
                    attempts += 1
                    if self._stop.wait(delay):
                        break
        except Exception as error:
            with self._lock:
                message = self._serial_error_message(error)
                self._log("ERROR", message, detail=str(error))
                self._connection.update(state="error", error=message, error_detail=str(error))
                self._reset_live()
        finally:
            self._close_serial(connection, orderly=True)
            with self._lock:
                self._serial = None
                self._hotkeys_enabled = False
                self._finish_capture("disconnect")
                if self._connection["state"] != "error":
                    self._connection["state"] = "disconnected"

    def _serial_session(self, connection):
        framer = LineFramer()
        stream_at = time.monotonic() + 0.35
        handshake_deadline = time.monotonic() + HANDSHAKE_TIMEOUT_SECONDS
        last_status = last_flush = time.monotonic()
        while not self._stop.is_set():
            for _ in range(16):
                if self._stop.is_set():
                    return
                try:
                    command = self._commands.get_nowait()
                except queue.Empty:
                    break
                self._write(connection, command)
            data = connection.read(min(max(connection.in_waiting, 1), 4096))
            for line in framer.feed(data):
                if self._process_line(line):
                    stream_at = time.monotonic() + 0.35
                    handshake_deadline = time.monotonic() + HANDSHAKE_TIMEOUT_SECONDS
            now = time.monotonic()
            with self._lock:
                connected = self._connection["state"] == "connected"
            if not connected and now >= handshake_deadline:
                raise OSError("Board handshake timed out: no valid STATUS received within 3 seconds")
            if connected and stream_at is not None and now >= stream_at:
                self._write(connection, "stream 1")
                stream_at = None
            if now - last_status >= (3 if connected else 0.75):
                self._write(connection, "status")
                last_status = now
            if now - last_flush >= 1:
                with self._lock:
                    self._flush_capture()
                last_flush = now

    def _process_line(self, line):
        with self._lock:
            try:
                record = parse_line(line)
                self._accept(record, line)
                return record["kind"] == "HELLO"
            except (ValueError, KeyError, TypeError, IndexError) as error:
                self._counters["malformed"] += 1
                self._log("MALFORMED", f"{error}: {line[:160]}")
                return False

    def _accept(self, record, raw=""):
        kind, fields = record["kind"], record.get("fields", [])
        self._capture_record(record, raw)
        if kind == "HELLO":
            self._reset_live()
            self._epoch += 1
            self._connection["epoch"] = self._epoch
            while not self._commands.empty():
                try:
                    self._commands.get_nowait()
                except queue.Empty:
                    break
            if not self._connection["demo"]:
                self._connection["state"] = "reconnecting" if self._connection["retry_attempt"] else "connecting"
                self._sync_commands()
            self._log("HELLO", ", ".join(fields))
        elif kind == "RAW":
            self._counters["raw"] += 1
            if self._pose.feed(record):
                if (self._last_pen_raw_seq is not None and
                        ((record["seq"] - self._last_pen_raw_seq) & 0xFFFFFFFF) != 1):
                    self._pen_motion.invalidate("sample_gap")
                motion = self._pose.world_motion()
                self._pen_motion.feed(record["t"], motion["body_acceleration_m_s2"],
                                      motion["gyroscope_rad_s"], motion["quaternion"],
                                      motion["stationary"], record.get("key_down"))
                self._last_pose_sample = time.monotonic()
                self._append_pen_sample(record)
            elif (self._last_pen_raw_seq is None or
                  0 < ((record["seq"] - self._last_pen_raw_seq) & 0xFFFFFFFF) < 0x80000000):
                self._pen_motion.invalidate("invalid_sample")
            self._board_clock = (record["t"], time.monotonic())
            seq = record["seq"]
            if self._last_raw_seq is not None:
                delta = (seq - self._last_raw_seq) & 0xFFFFFFFF
                if 1 < delta < 0x80000000:
                    self._counters["raw_gaps"] += delta - 1
            self._last_raw_seq = seq
            if self._last_wave_seq is None or ((seq - self._last_wave_seq) & 0xFFFFFFFF) >= 4:
                self._waveform.append({key: record[key] for key in ("seq", "t", *AXES)})
                self._last_wave_seq = seq
        elif kind == "STATUS":
            self._status = {key: value for key, value in record.items() if key != "kind"}
            self._board_clock = (record["t"], time.monotonic())
            if not record["armed"]:
                self._hotkeys_enabled = False
                self._live_match["id"] = 0
                self._recent_match = None
                self._last_confident_match = None
            if self._connection["state"] in {"connecting", "reconnecting"}:
                self._connection.update(state="connected", error=None, error_detail=None, retry_attempt=0)
                self._log("CONNECTED", self._connection["port"])
        elif kind == "FIRMWARE":
            if not fields or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", fields[0]):
                raise ValueError("Invalid firmware identity")
            if any(not re.fullmatch(r"[A-Z_]{1,32}", value) for value in fields[1:]):
                raise ValueError("Invalid firmware capability")
            changed = self._protocol["firmware"] != fields[0] or self._protocol["capabilities"] != fields[1:]
            self._protocol.update(firmware=fields[0], capabilities=fields[1:])
            self._training["required"] = 1 if "ONE_DEMO" in fields else 3
            if "KEY_CAPTURE" in fields:
                self._protocol["manual_training"] = True
            if changed:
                self._log(kind, ", ".join(fields))
        elif kind == "INFO":
            if len(fields) != 8:
                raise ValueError("INFO requires eight fields")
            version, ready_mask, display_ok, training, selected, progress, ready, deleting = map(_integer, fields)
            _integer(selected, 1, 8)
            _integer(progress, 0, 3)
            if any(flag > 1 for flag in (display_ok, training, ready, deleting)):
                raise ValueError("Invalid INFO flag")
            self._protocol.update(version=version, ready_mask=ready_mask,
                                  display_ok=bool(display_ok))
            if not training or selected != self._training["slot"]:
                self._training["warning"] = None
            self._training.update(state="ready" if training and ready else "recording" if training else "idle",
                                  slot=selected if training else None, collected=progress if training else 0)
            if not training:
                self._training["capturing"] = False
            self._pending_delete = selected if deleting else None
        elif kind == "CAPTURE":
            if fields == ["MODE", "KEY"]:
                self._protocol["manual_training"] = True
            elif len(fields) == 2 and fields[0] in {"WAIT", "BEGIN", "END"}:
                slot = _integer(fields[1], 1, 8)
                if self._training["state"] != "idle" and self._training["slot"] == slot:
                    self._training["capturing"] = fields[0] == "BEGIN"
                    if fields[0] == "BEGIN":
                        self._training["message"] = ""
            else:
                raise ValueError("Invalid CAPTURE event")
            self._log(kind, ", ".join(fields))
        elif kind == "SLOT":
            if len(fields) != 3:
                raise ValueError("SLOT requires ID, template count and name")
            slot, templates = _integer(fields[0], 1, 8), _integer(fields[1], 0, 16)
            self._slots[slot - 1].update(state="saved" if templates else "empty",
                                         templates=templates, board_name=fields[2])
            self._protocol["inventory"] = all(slot["state"] != "unknown" for slot in self._slots)
        elif kind == "STATE":
            if fields == ["ARMED"]:
                self._status["armed"] = 1
            elif fields == ["IDLE"]:
                self._status["armed"] = 0
                self._training.update(state="idle", slot=None, collected=0, message="", capturing=False, warning=None)
                self._demo_key_started = None
                self._pending_delete = None
                self._hotkeys_enabled = False
                self._live_match["id"] = 0
                self._recent_match = None
                self._last_confident_match = None
            self._log(kind, ", ".join(fields))
        elif kind == "TRAIN":
            if not fields:
                raise ValueError("Empty training event")
            mode = fields[0]
            if mode in {"BEGIN", "DEMO", "READY"}:
                allowed_lengths = {"BEGIN": {2}, "DEMO": {3}, "READY": {2, 3}}
                if len(fields) not in allowed_lengths[mode]:
                    raise ValueError("Invalid training event fields")
                slot = _integer(fields[1], 1, 8)
                if mode == "BEGIN":
                    self._training.update(state="recording", slot=slot, collected=0, message="", capturing=False, warning=None)
                    self._status["armed"] = 0
                    self._pending_delete = None
                    self._hotkeys_enabled = False
                    self._live_match["id"] = 0
                    self._recent_match = None
                    self._last_confident_match = None
                elif mode == "DEMO":
                    self._training.update(slot=slot, collected=_integer(fields[2], 0, 3), capturing=False, message="", warning=None)
                else:
                    count = _integer(fields[2], 1, 3) if len(fields) == 3 else 3
                    self._training.update(state="ready", slot=slot, collected=count, capturing=False)
            elif mode == "REJECT":
                self._training["message"] = ", ".join(fields[1:])
            elif mode == "WARN":
                if len(fields) != 5 or fields[1] != "SIMILAR":
                    raise ValueError("Invalid training warning")
                slot = _integer(fields[2], 1, 8)
                distance, limit = _integer(fields[3]) / 1000000.0, _integer(fields[4]) / 1000000.0
                if (self._training["state"] == "idle" or not self._training["collected"] or
                        slot == self._training["slot"] or not 0 <= distance <= limit <= 1 or limit == 0):
                    raise ValueError("Invalid similarity warning context")
                self._training["warning"] = {"kind": "SIMILAR", "slot": slot,
                                              "distance": distance, "limit": limit}
            self._log(kind, ", ".join(fields), training=copy.deepcopy(self._training))
        elif kind == "DELETE":
            if len(fields) == 2 and fields[0] == "CONFIRM":
                self._pending_delete = _integer(fields[1], 1, 8)
                self._status["armed"] = 0
                self._hotkeys_enabled = False
            self._log(kind, ", ".join(fields))
        elif kind in {"SAVED", "DELETED"}:
            slot = _integer(fields[0], 1, 8)
            self._training.update(state="idle", slot=None, collected=0, message="", capturing=False, warning=None)
            self._pending_delete = None
            self._status["armed"] = 0
            self._hotkeys_enabled = False
            self._slots[slot - 1].update(state="unknown", templates=None)
            self._protocol["inventory"] = False
            self._queue_command("list")
            self._log(kind, ", ".join(fields), id=slot)
        elif kind == "MATCH":
            self._live_match = {key: value for key, value in record.items() if key != "kind"}
            self._last_match = time.monotonic()
            if record["id"]:
                self._recent_match = dict(self._live_match)
                self._last_confident_match = self._last_match
        elif kind == "EVENT":
            identity = (record["t"], record["id"])
            if identity in self._seen_events:
                self._counters["duplicate_events"] += 1
                return
            self._seen_events.append(identity)
            self._counters["events"] += 1
            slot = self._settings[str(record["id"])]
            self._log(kind, slot["name"], **{key: value for key, value in record.items() if key != "kind"})
            fence = self._hotkey_fence
            fresh = fence is not None and 0 < ((record["t"] - fence) & 0xFFFFFFFF) < 0x80000000
            if (self._hotkeys_enabled and not self._connection["demo"] and fresh
                    and self._status["armed"] and slot["hotkey"]):
                try:
                    emit_hotkey(validate_hotkey(slot["hotkey"]), self._keyboard.send)
                    self._counters["hotkeys"] += 1
                    self._log("HOTKEY", slot["hotkey"], id=record["id"])
                except (OSError, ValueError) as error:
                    self._hotkeys_enabled = False
                    self._log("ERROR", str(error))
        elif kind == "UNKNOWN":
            self._counters["unknown"] += 1
            self._log(kind, record["reason"], t=record["t"])
        elif kind == "ERROR":
            self._log(kind, ", ".join(fields))
            if "NOT_CALIBRATED" in fields or "CALIBRATION_REQUIRED" in fields:
                self._protocol["requires_calibration"] = True
            if fields and fields[0].startswith("SAVE"):
                self._training["message"] = ", ".join(fields)
        elif kind == "CALIBRATION":
            if fields not in (["OK"], ["FAILED"]):
                raise ValueError("Invalid calibration result")
            self._status["calibrated"] = int(fields == ["OK"])
            self._log(kind, fields[0])
        else:
            self._log(kind, ", ".join(fields))

    def _demo_inventory(self):
        training = self._training
        self._accept(parse_line("FIRMWARE,demo-gesture-20261005-r5,KEY_CAPTURE,NO_CALIBRATION,RANDOM_START,LIVE_MATCH,ONE_DEMO,SIMILARITY_WARNING,RAW_KEY"))
        self._accept(parse_line(f"INFO,1,7,1,{int(training['state'] != 'idle')},"
                               f"{self._pending_delete or training['slot'] or 1},{training['collected']},"
                               f"{int(training['state'] == 'ready')},{int(self._pending_delete is not None)}"))
        for slot in self._demo_slots:
            count = self._demo_saved.get(slot, 0)
            self._accept(parse_line(f"SLOT,{slot},{count}," + (f"Demo {slot}" if count else "")))
        self._accept(parse_line("CAPTURE,MODE,KEY"))
        if training["state"] != "idle" and training["collected"] < 3:
            state = "BEGIN" if training["capturing"] else "WAIT"
            self._accept(parse_line(f"CAPTURE,{state},{training['slot']}"))

    def _demo_status(self, stamp):
        self._accept(parse_line(f"STATUS,{stamp},{self._status['armed']},{len(self._demo_saved)},"
                               f"1,{self._demo_seq},0,0,{self._counters['unknown']},0,420,90"))

    def _demo_command(self, command):
        name, _, arg = command.partition(" ")
        if name == "demo":
            down = arg == "key 1"
            pressed, released = down and not self._demo_key_down, not down and self._demo_key_down
            self._demo_key_down = down
            if self._training["state"] not in {"recording", "ready"} or self._training["collected"] >= 3:
                return
            slot = self._training["slot"]
            if pressed and self._demo_key_started is None:
                self._demo_key_started = time.monotonic()
                self._accept(parse_line(f"CAPTURE,BEGIN,{slot}"))
            elif released and self._demo_key_started is not None:
                duration = time.monotonic() - self._demo_key_started
                self._demo_key_started = None
                self._accept(parse_line(f"CAPTURE,END,{slot}"))
                if duration < 0.18 or duration > 2.4:
                    reason = "too short" if duration < 0.18 else "too long"
                    self._accept(parse_line(f"TRAIN,REJECT,{reason}"))
                else:
                    count = self._training["collected"] + 1
                    self._accept(parse_line(f"TRAIN,DEMO,{slot},{count}"))
                    self._accept(parse_line(f"TRAIN,READY,{slot},{count}"))
                if self._training["collected"] < 3:
                    self._accept(parse_line(f"CAPTURE,WAIT,{slot}"))
        elif name in {"list", "status"}:
            self._demo_inventory() if name == "list" else self._demo_status(self._demo_seq * 5)
        elif name == "learn":
            if self._training["state"] != "idle" or self._pending_delete:
                self._accept(parse_line("ERROR,LEARN,BUSY"))
                return
            self._accept(parse_line("STATE,IDLE"))
            self._accept(parse_line(f"TRAIN,BEGIN,{arg}"))
            self._accept(parse_line(f"CAPTURE,WAIT,{arg}"))
        elif name == "delete":
            if self._training["state"] != "idle" or self._pending_delete:
                self._accept(parse_line("ERROR,DELETE,BUSY"))
                return
            if int(arg) not in self._demo_saved:
                self._accept(parse_line("ERROR,DELETE,EMPTY_SLOT"))
                return
            self._accept(parse_line("STATE,IDLE"))
            self._accept(parse_line(f"DELETE,CONFIRM,{arg}"))
        elif name == "save":
            if self._pending_delete:
                slot = self._pending_delete
                self._demo_saved.pop(slot, None)
                self._accept(parse_line(f"DELETED,{slot},1024"))
                self._accept(parse_line("STATE,IDLE"))
            elif self._training["state"] == "ready" and not self._training["capturing"]:
                slot = self._training["slot"]
                self._demo_saved[slot] = self._training["collected"]
                self._accept(parse_line(f"SAVED,{slot},1024"))
                self._accept(parse_line("STATE,IDLE"))
            else:
                self._accept(parse_line("ERROR,SAVE,NOT_READY"))
        elif name == "arm":
            if self._training["state"] != "idle" or self._pending_delete:
                self._accept(parse_line("ERROR,ARM,BUSY"))
            elif not self._demo_saved:
                self._accept(parse_line("ERROR,ARM,NO_CLASSES"))
            else:
                self._accept(parse_line("STATE,ARMED"))
        elif name in {"disarm", "cancel", "calibrate"}:
            self._accept(parse_line("STATE,IDLE"))
            if name == "calibrate":
                self._accept(parse_line("CALIBRATION,OK"))
        elif name == "stream":
            self._demo_stream = arg == "1"

    def _run_demo(self):
        try:
            with self._lock:
                self._connection["state"] = "connected"
                self._demo_saved = {1: 3, 2: 3}
                self._demo_slots = range(1, 9)
                self._demo_seq = 0
                self._demo_stream = True
                self._accept(parse_line("HELLO,DEMO_SIMULATOR,1"))
                self._demo_inventory()
                self._demo_status(0)
                self._log("SIMULATION", "Demo data; no physical board and no real keyboard output")
            last_status = last_event = last_match = time.monotonic()
            while not self._stop.wait(0.02):
                with self._lock:
                    for _ in range(16):
                        try:
                            command = self._commands.get_nowait()
                        except queue.Empty:
                            break
                        self._demo_command(command)
                    now = time.monotonic()
                    for _ in range(4):
                        self._demo_seq += 1
                        tick = self._demo_seq * 0.005
                        amplitude = 0.75 if self._training["capturing"] else 0.35
                        pitch = amplitude * math.sin(tick * 1.6)
                        rate = amplitude * 1.6 * math.cos(tick * 1.6)
                        acceleration = 0.8 * math.sin(tick * 1.4)
                        # Rotate gravity and a world-X translation into body axes.
                        axes = [-9.80665 * math.sin(pitch) + acceleration * math.cos(pitch), 0.0,
                                9.80665 * math.cos(pitch) + acceleration * math.sin(pitch),
                                0.0, rate, 0.0]
                        if self._demo_stream:
                            self._accept({"kind": "RAW", "seq": self._demo_seq,
                                          "t": self._demo_seq * 5, "key_down": self._demo_key_down,
                                          **dict(zip(AXES, axes))})
                    if now - last_event >= 2.5 and self._status["armed"]:
                        count = self._counters["events"] + self._counters["unknown"]
                        if count % 3 == 2:
                            self._accept(parse_line(f"UNKNOWN,{self._demo_seq * 5},distance"))
                        elif self._demo_saved:
                            slot = sorted(self._demo_saved)[count % len(self._demo_saved)]
                            self._accept(parse_line(f"EVENT,{self._demo_seq * 5},{slot},110000,690000,720"))
                        last_event = now
                    if now - last_match >= 0.2 and self._status["armed"]:
                        choices = sorted(self._demo_saved)
                        slot = choices[int(tick) % len(choices)] if choices and int(tick * 2) % 5 else 0
                        self._accept(parse_line(f"MATCH,{self._demo_seq * 5},{slot},110000,690000,720"))
                        last_match = now
                    if now - last_status >= 1:
                        self._demo_status(self._demo_seq * 5)
                        self._flush_capture()
                        last_status = now
        except Exception as error:
            with self._lock:
                self._log("ERROR", str(error))
                self._connection.update(state="error", error=str(error))
        finally:
            with self._lock:
                self._hotkeys_enabled = False
                self._finish_capture("disconnect")
                if self._connection["state"] != "error":
                    self._connection["state"] = "disconnected"

    def _capture_state(self):
        if self._capture is None:
            return self._last_capture
        capture = self._capture
        return {"active": True, "id": capture["id"], "started_utc": capture["started_utc"],
                "simulation": capture["simulation"], "raw_count": capture["stats"].raw_count,
                "event_count": capture["stats"].events, "metadata": capture["metadata"]}

    def capture_start(self, metadata=None):
        metadata = metadata or {}
        if not isinstance(metadata, dict):
            raise ValueError("Capture metadata must be a JSON object")
        clean = {}
        for key, default in (("user", "anonymous"), ("session", "local"),
                             ("label", "unknown"), ("speed", "unknown")):
            value = metadata.get(key, default)
            if not isinstance(value, str) or not 1 <= len(value.strip()) <= 80:
                raise ValueError(f"{key} must contain 1 to 80 characters")
            clean[key] = value.strip()
        if clean["speed"] not in {"slow", "normal", "fast", "unknown"}:
            raise ValueError("speed must be slow, normal, fast or unknown")
        with self._lock:
            if self._connection["state"] != "connected":
                raise ValueError("Connect before starting a capture")
            if self._capture:
                raise ValueError("A capture is already running")
            self.capture_dir.mkdir(parents=True, exist_ok=True)
            capture_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "_" + secrets.token_hex(3)
            files = {"csv": f"{capture_id}.csv", "events": f"{capture_id}.events.jsonl",
                     "summary": f"{capture_id}.summary.json"}
            csv_file = (self.capture_dir / files["csv"]).open("x", encoding="utf-8", newline="")
            try:
                event_file = (self.capture_dir / files["events"]).open("x", encoding="utf-8")
            except Exception:
                csv_file.close()
                raise
            writer = csv.DictWriter(csv_file, fieldnames=(*_host.CSV_FIELDS, "simulation"))
            writer.writeheader()
            self._capture = {"id": capture_id, "started_utc": _utc(), "start": time.monotonic(),
                             "simulation": self._connection["demo"], "metadata": clean,
                             "stats": _host.CaptureStats(), "csv": csv_file, "events": event_file,
                             "writer": writer, "files": files}
            self._log("CAPTURE", f"Started {capture_id}")
        return self.snapshot()

    def _capture_record(self, record, raw):
        capture = self._capture
        if capture is None:
            return
        capture["stats"].accept(record)
        common = {"host_utc": _utc(), "host_monotonic_s": time.monotonic(),
                  **capture["metadata"], "simulation": capture["simulation"]}
        if record["kind"] == "RAW":
            capture["writer"].writerow({**common, **{key: record[key] for key in ("seq", "t", *AXES)}})
        else:
            capture["events"].write(json.dumps({**common, "epoch": self._epoch, "raw": raw, "record": record},
                                              ensure_ascii=False) + "\n")

    def _flush_capture(self):
        if self._capture:
            self._capture["csv"].flush()
            self._capture["events"].flush()

    def capture_stop(self):
        with self._lock:
            self._finish_capture("user")
        return self.snapshot()

    def _finish_capture(self, reason):
        capture = self._capture
        if capture is None:
            return
        self._capture = None
        capture["csv"].close()
        capture["events"].close()
        stats = capture["stats"]
        summary = {"id": capture["id"], "started_utc": capture["started_utc"], "ended_utc": _utc(),
                   "duration_s": round(time.monotonic() - capture["start"], 3), "reason": reason,
                   "simulation": capture["simulation"], "metadata": capture["metadata"],
                   "raw_count": stats.raw_count, "event_count": stats.events, "unknown_count": stats.unknown,
                   "raw_gaps": stats.raw_gaps, "raw_resets": stats.raw_resets,
                   "duplicate_events": stats.duplicate_events, "last_status": stats.last_status,
                   "files": capture["files"]}
        (self.capture_dir / capture["files"]["summary"]).write_text(
            json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
        self._last_capture = {"active": False, **summary}
        self._log("CAPTURE", f"Saved {capture['id']}")

    def list_captures(self):
        captures = []
        for path in sorted(self.capture_dir.glob("*.summary.json"), reverse=True):
            try:
                value = json.loads(path.read_text(encoding="utf-8"))
                if isinstance(value, dict) and isinstance(value.get("id"), str):
                    captures.append(value)
            except (OSError, ValueError):
                continue
        return captures[:200]
