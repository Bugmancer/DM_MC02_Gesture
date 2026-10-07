"""Exercise the local HTTP boundary without physical hardware."""
import http.client
import json
import os
from pathlib import Path
import shutil
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from backend import Controller
from server import create_server, UI_ASSETS


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.controller = Controller(data_dir=self.temp.name, ports_provider=lambda: [])
        self.server = create_server(self.controller, port=0, data_dir=self.temp.name)
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()

    def tearDown(self):
        self.server.shutdown()
        self.worker.join()
        self.controller.close()
        self.server.server_close()
        self.temp.cleanup()

    def request(self, path, payload=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=4)
        actual = {"X-Gesture-Token": self.server.token, "Content-Type": "application/json"}
        actual.update(headers or {})
        connection.request("GET" if payload is None else "POST", path,
                           None if payload is None else json.dumps(payload), actual)
        response = connection.getresponse()
        body = response.read()
        result = response.status, body, dict(response.getheaders())
        connection.close()
        return result

    def test_static_assets_and_session(self):
        for path in ("/", "/app.js", "/air-pen.js", "/styles.css", "/board.png", "/vendor/chart.umd.js", "/vendor/lucide.min.js"):
            status, body, headers = self.request(path)
            self.assertEqual(status, 200, path)
            self.assertGreater(len(body), 100)
            self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
            self.assertEqual(headers["Cache-Control"], "no-store")
        status, body, _ = self.request("/api/session.json")
        self.assertEqual(json.loads(body)["token"], self.server.token)

    def test_board_config_endpoint_is_authenticated_and_validated(self):
        payload = {"class_limit": 4, "demo_target": 6, "colors": ["#654321"] * 8, "rgb_hold_ms": 5000}
        self.request("/api/connect", {"demo": True})
        deadline = time.monotonic() + 2
        while "GUI_CONFIG" not in self.controller.snapshot()["protocol"]["capabilities"] and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(self.request("/api/board/config", payload, {"X-Gesture-Token": "invalid"})[0], 403)
        self.assertEqual(self.request("/api/board/config", {**payload, "class_limit": 9})[0], 400)
        self.assertEqual(self.request("/api/board/config", {**payload, "rgb_hold_ms": 150})[0], 400)
        self.assertEqual(self.request("/api/board/config", {**payload, "demo_target": 21})[0], 400)
        self.assertEqual(self.request("/api/board/config", payload)[0], 200)
        deadline = time.monotonic() + 2
        while self.controller.snapshot()["config_write"]["state"] == "pending" and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(self.controller.snapshot()["config"]["class_limit"], 4)
        self.assertEqual(self.controller.snapshot()["config"]["demo_target"], 6)
        self.assertEqual(self.controller.snapshot()["config"]["rgb_hold_ms"], 5000)

    def test_ui_identity_matches_html_state_and_mutations(self):
        identity = self.server.identity()
        self.assertRegex(identity["ui_revision"], r"^[a-f0-9]{20}$")
        self.assertTrue(identity["server_instance"])
        for route in ("/api/session.json", "/api/health", "/api/state"):
            status, body, _ = self.request(route)
            self.assertEqual(status, 200)
            payload = json.loads(body)
            for key, value in identity.items():
                self.assertEqual(payload[key], value, route)
        status, body, _ = self.request("/api/disconnect", {})
        self.assertEqual(status, 200)
        self.assertEqual({key: json.loads(body)["state"][key] for key in identity}, identity)
        for route in ("/", "/index.html"):
            status, body, _ = self.request(route)
            self.assertEqual(status, 200)
            self.assertNotIn(b"__GUI_REVISION__", body)
            self.assertIn(f'data-ui-revision="{identity["ui_revision"]}"'.encode(), body)
            self.assertIn(f'/app.js?v={identity["ui_revision"]}'.encode(), body)
        self.assertEqual(self.request("/app.js?v=" + identity["ui_revision"])[1],
                         self.request("/app.js")[1])

    def test_asset_digest_updates_without_restart_and_reuses_cache(self):
        assets = Path(self.temp.name) / "static"
        assets.mkdir()
        for name in UI_ASSETS:
            shutil.copy2(self.server.static_dir / name, assets / name)
        self.server.static_dir = assets.resolve()
        original = self.server.identity()
        with patch("pathlib.Path.read_bytes", side_effect=AssertionError("Digest cache was ignored")):
            self.assertEqual(self.server.identity(), original)
        script = assets / "app.js"
        previous = script.stat()
        script.write_bytes(script.read_bytes() + b"\n// Updated local interface.\n")
        os.utime(script, ns=(previous.st_atime_ns, previous.st_mtime_ns + 1_000_000))
        updated = self.server.identity()
        self.assertNotEqual(updated["ui_revision"], original["ui_revision"])
        self.assertEqual(updated["server_instance"], original["server_instance"])
        self.assertIn(updated["ui_revision"].encode(), self.request("/")[1])

    def test_server_restart_changes_instance_and_token_but_keeps_asset_revision(self):
        other = create_server(self.controller, port=0, data_dir=self.temp.name)
        try:
            self.assertEqual(other.identity()["ui_revision"], self.server.identity()["ui_revision"])
            self.assertNotEqual(other.server_instance, self.server.server_instance)
            self.assertNotEqual(other.token, self.server.token)
        finally:
            other.server_close()

    def test_mutations_require_local_host_origin_and_token(self):
        for headers in ({"Host": "external.example"}, {"Origin": "https://external.example"}, {"X-Gesture-Token": "wrong"}):
            self.assertEqual(self.request("/api/connect", {"demo": True}, headers)[0], 403)
        self.assertEqual(self.controller.snapshot()["connection"]["state"], "disconnected")

    def test_paths_and_invalid_requests(self):
        for path in ("/../server.py", "/%2e%2e/server.py", "/api/missing"):
            self.assertEqual(self.request(path)[0], 404)
        self.assertEqual(self.request("/api/download?file=../settings.json")[0], 400)
        self.assertEqual(self.request("/api/download?file=missing.csv")[0], 404)
        self.assertEqual(self.request("/api/state?after=oops")[0], 400)
        self.assertEqual(self.request("/api/state?pen_after=oops")[0], 400)
        self.assertEqual(self.request("/api/state?pen_after=-1")[0], 400)
        self.assertEqual(self.request("/api/command", ["arm"])[0], 400)
        self.assertEqual(self.request("/api/command", {"command": "bad"})[0], 400)

    def test_state_only_transmits_unseen_pen_corrections(self):
        for seq, down in enumerate((0, 1, 1, 0), start=1):
            self.controller._process_line(f"RAW,{seq},{seq * 5},0,0,9807,0,0,0,{down}")
        state = json.loads(self.request("/api/state")[1])
        self.assertEqual(len(state["pen_corrections"]), 1)
        for _ in range(4):
            delta = json.loads(self.request("/api/state?pen_after=1")[1])
            self.assertEqual(delta["pen_corrections"], [])
            self.assertEqual(delta["pen_correction_cursor"], 1)

    def test_demo_capture_download_and_hotkey_rejection(self):
        self.assertEqual(self.request("/api/connect", {"demo": True})[0], 200)
        deadline = time.monotonic() + 3
        while self.controller.snapshot()["connection"]["state"] != "connected" and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(self.controller.snapshot()["protocol"]["inventory"])
        self.assertEqual(self.request("/api/settings", {"hotkeys_enabled": True})[0], 400)
        metadata = {"user": "u01", "session": "test", "label": "unknown", "speed": "normal"}
        self.assertEqual(self.request("/api/capture/start", metadata)[0], 200)
        deadline = time.monotonic() + 2
        while self.controller.snapshot()["capture"]["raw_count"] < 8 and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(self.request("/api/capture/stop", {})[0], 200)
        captures = json.loads(self.request("/api/captures")[1])["captures"]
        self.assertEqual(len(captures), 1)
        self.assertTrue(captures[0]["simulation"])
        for name in captures[0]["files"].values():
            status, body, headers = self.request("/api/download?file=" + name)
            self.assertEqual(status, 200)
            if not name.endswith(".events.jsonl"):
                self.assertTrue(body)
            self.assertIn("attachment", headers["Content-Disposition"])
        self.assertEqual(self.request("/api/disconnect", {})[0], 200)

    def test_pose_endpoint_and_local_assets(self):
        self.request("/api/connect", {"demo": True})
        deadline = time.monotonic() + 2
        while not self.controller.snapshot()["pose"]["available"] and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(self.controller.snapshot()["pose"]["available"])
        self.assertEqual(self.request("/api/pose/reset", {})[0], 200)
        for path in ("/spatial.js", "/vendor/three.module.js", "/vendor/three.core.js", "/vendor/OrbitControls.js"):
            self.assertEqual(self.request(path)[0], 200, path)


if __name__ == "__main__":
    unittest.main()
