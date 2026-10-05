"""Loopback-only HTTP shell for the DM-MC02 companion application."""
from __future__ import annotations

import argparse
import json
import logging
from logging.handlers import RotatingFileHandler
import mimetypes
import os
from pathlib import Path
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlsplit

from backend import Controller

ROOT = Path(__file__).resolve().parent
APP_ID = "dm-mc02-gesture-gui"


class LocalServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, address, controller, data_dir=None):
        self.controller = controller
        self.data_dir = Path(data_dir or ROOT / "data").resolve()
        self.token = secrets.token_urlsafe(32)
        super().__init__(address, Handler)

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_port}"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        if args and str(args[1] if len(args) > 1 else "").startswith(("4", "5")):
            logging.warning(format, *args)

    def reply(self, code, data, content_type="application/json; charset=utf-8", filename=None):
        if not isinstance(data, bytes):
            data = json.dumps(data, ensure_ascii=False, allow_nan=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; object-src 'none'; frame-ancestors 'none'")
        if filename:
            from urllib.parse import quote
            self.send_header("Content-Disposition", "attachment; filename*=UTF-8''" + quote(filename))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def valid_host(self):
        valid = {f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}"}
        if self.headers.get("Host") not in valid:
            self.close_connection = True
            self.reply(403, {"ok": False, "error": "Local host required"})
            return False
        return True

    def do_GET(self):
        if not self.valid_host():
            return
        route = urlsplit(self.path)
        query = parse_qs(route.query)
        try:
            if route.path == "/api/health":
                return self.reply(200, {"app": APP_ID, "pid": os.getpid(), "url": self.server.url})
            if route.path == "/api/session.json":
                return self.reply(200, {"token": self.server.token, "port": self.server.server_port})
            if route.path == "/api/state":
                after = int(query.get("after", ["0"])[0])
                return self.reply(200, self.server.controller.snapshot(max(0, after)))
            if route.path == "/api/ports":
                return self.reply(200, {"ports": self.server.controller.list_ports()})
            if route.path == "/api/captures":
                return self.reply(200, {"captures": self.server.controller.list_captures()})
            if route.path == "/api/download":
                name = query.get("file", [""])[0]
                if not name or Path(name).name != name or any(c in name for c in ("/", "\\", ":")):
                    raise ValueError("Invalid capture filename")
                base = (self.server.data_dir / "captures").resolve()
                path = (base / name).resolve()
                if not path.is_relative_to(base) or path.suffix not in {".csv", ".jsonl", ".json"} or not path.is_file():
                    return self.reply(404, {"ok": False, "error": "Capture not found"})
                return self.reply(200, path.read_bytes(), "application/octet-stream", name)
            if route.path.startswith("/api/"):
                return self.reply(404, {"ok": False, "error": "Unknown endpoint"})
            name = unquote(route.path).lstrip("/") or "index.html"
            base = (ROOT / "static").resolve()
            path = (base / name).resolve()
            if not path.is_relative_to(base) or not path.is_file():
                return self.reply(404, {"ok": False, "error": "File not found"})
            kind = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
            if path.suffix == ".js":
                kind = "text/javascript"
            if kind.startswith("text/"):
                kind += "; charset=utf-8"
            self.reply(200, path.read_bytes(), kind)
        except (ValueError, OSError, RuntimeError) as error:
            self.reply(400, {"ok": False, "error": str(error)})

    def do_POST(self):
        if not self.valid_host():
            return
        origin = self.headers.get("Origin")
        allowed = {self.server.url, f"http://localhost:{self.server.server_port}"}
        if origin and origin not in allowed:
            self.close_connection = True
            return self.reply(403, {"ok": False, "error": "Origin rejected"})
        if not secrets.compare_digest(self.headers.get("X-Gesture-Token", ""), self.server.token):
            self.close_connection = True
            return self.reply(403, {"ok": False, "error": "Session token required"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length < 0 or length > 16384:
                raise ValueError("Request too large")
            payload = json.loads(self.rfile.read(length) or b"{}")
            if not isinstance(payload, dict):
                raise ValueError("Object required")
            controller = self.server.controller
            route = urlsplit(self.path).path
            if route == "/api/connect":
                controller.connect(payload.get("port", ""), demo=payload.get("demo", False) is True)
            elif route == "/api/disconnect":
                controller.disconnect()
            elif route == "/api/command":
                controller.command(payload.get("command", ""))
            elif route == "/api/settings":
                if "hotkeys_enabled" in payload:
                    if not isinstance(payload["hotkeys_enabled"], bool):
                        raise ValueError("Boolean required")
                    controller.set_hotkeys(payload["hotkeys_enabled"])
                else:
                    controller.save_slot(payload.get("slot"), payload.get("name", ""), payload.get("hotkey", ""))
            elif route == "/api/capture/start":
                controller.capture_start(payload)
            elif route == "/api/capture/stop":
                controller.capture_stop()
            elif route == "/api/pose/reset":
                controller.reset_pose()
            elif route == "/api/shutdown":
                self.reply(200, {"ok": True})
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            else:
                return self.reply(404, {"ok": False, "error": "Unknown endpoint"})
            self.reply(200, {"ok": True, "state": controller.snapshot()})
        except (ValueError, TypeError, KeyError, OSError, RuntimeError) as error:
            self.reply(400, {"ok": False, "error": str(error)})


def create_server(controller, port=8765, data_dir=None):
    last_error = None
    for candidate in ([0] if port == 0 else range(port, port + 20)):
        try:
            return LocalServer(("127.0.0.1", candidate), controller, data_dir)
        except OSError as error:
            last_error = error
    raise last_error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--data-dir", type=Path, default=ROOT / "data")
    args = parser.parse_args()
    data_dir = args.data_dir.resolve()
    data_dir.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, handlers=[RotatingFileHandler(data_dir / "gui.log", maxBytes=2000000, backupCount=2, encoding="utf-8")])
    controller = Controller(data_dir=data_dir)
    server = create_server(controller, args.port, data_dir)
    metadata = data_dir / "server.json"
    metadata.write_text(json.dumps({"app": APP_ID, "pid": os.getpid(), "url": server.url}), encoding="utf-8")
    logging.info("Listening at %s", server.url)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        controller.close()
        server.server_close()
        try:
            if json.loads(metadata.read_text(encoding="utf-8")).get("pid") == os.getpid():
                metadata.unlink()
        except (OSError, ValueError):
            pass


if __name__ == "__main__":
    main()
