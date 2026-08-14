#!/usr/bin/env python3
"""Local-only browser UI for ABVx Mac Companion."""

import argparse
import contextlib
import glob
import io
import json
import os
import secrets
import shlex
import subprocess
import tempfile
import threading
import urllib.parse
import webbrowser
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import abvx_companion as core

PROJECT_ROOT = Path(os.environ.get("ABVX_PROJECT_ROOT", Path(__file__).resolve().parent.parent)).expanduser().resolve()
UI_FILE = Path(__file__).resolve().parent / "companion_ui" / "index.html"
IDF_EXPORT = Path.home() / "esp/esp-idf-v5.4.2/export.sh"
MAX_IMPORT_BYTES = 128 * 1024 * 1024
MAX_JOB_OUTPUT = 24000
IMPORT_EXTENSIONS = {"book": {".txt", ".epub", ".fb2"}, "music": {".mp3"}}
IMPORT_LOCK = threading.Lock()
SD_OVERRIDE = None


class AppState:
    def __init__(self):
        self.token = secrets.token_urlsafe(24)
        self.lock = threading.Lock()
        self.job_name = ""
        self.job_state = "IDLE"
        self.job_returncode = None
        self.job_output = deque(maxlen=240)

    def snapshot(self):
        with self.lock:
            return {"name": self.job_name, "state": self.job_state,
                    "returncode": self.job_returncode,
                    "output": "".join(self.job_output)[-MAX_JOB_OUTPUT:]}

    def start(self, name, command):
        with self.lock:
            if self.job_state == "RUNNING":
                raise RuntimeError(f"{self.job_name} is already running")
            self.job_name, self.job_state, self.job_returncode = name, "RUNNING", None
            self.job_output.clear()
            self.job_output.append(f"{name} started\n")
        threading.Thread(target=self._run, args=(command,), daemon=True).start()

    def _run(self, command):
        returncode = -1
        try:
            process = subprocess.Popen(command, cwd=PROJECT_ROOT, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, bufsize=1)
            if process.stdout:
                for line in process.stdout:
                    with self.lock:
                        self.job_output.append(line)
            returncode = process.wait()
        except Exception as exc:
            with self.lock:
                self.job_output.append(f"ERROR: {exc}\n")
        with self.lock:
            self.job_returncode = returncode
            self.job_state = "DONE" if returncode == 0 else "FAILED"
            self.job_output.append(f"{self.job_name} {self.job_state.lower()} ({returncode})\n")


STATE = AppState()


def usb_ports():
    return sorted(set(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")))


def safe_filename(name):
    candidate = Path(name).name
    if not candidate or candidate in {".", ".."}:
        raise RuntimeError("filename is empty")
    if any(ch in candidate for ch in "\\/"):
        raise RuntimeError("invalid filename")
    if candidate.startswith("."):
        raise RuntimeError("dotfile is not supported")
    if len(candidate) > 120:
        raise RuntimeError("filename is too long")
    return candidate



def validate_import_request(kind, filename):
    if kind not in IMPORT_EXTENSIONS:
        raise RuntimeError("unsupported import type")
    safe = safe_filename(filename)
    ext = Path(safe).suffix.lower()
    if ext not in IMPORT_EXTENSIONS[kind]:
        allowed = ", ".join(sorted(IMPORT_EXTENSIONS[kind]))
        raise RuntimeError(f"unsupported extension: {ext} (expected: {allowed})")
    return safe

def validate_payload_json(payload):
    if not isinstance(payload, dict):
        raise RuntimeError("invalid JSON payload")
    return payload


def device_status():
    result = {"sd": {"ready": False, "path": "", "error": "not detected"},
              "usb_ports": usb_ports(), "idf_ready": IDF_EXPORT.is_file(),
              "firmware": {"ready": False, "path": "", "size": 0},
              "job": STATE.snapshot()}
    firmware = PROJECT_ROOT / "build/cardputer-abvx-minimal.bin"
    if firmware.is_file():
        result["firmware"] = {"ready": True, "path": str(firmware), "size": firmware.stat().st_size}
    try:
        sd = core.resolve_sd(SD_OVERRIDE)
        usage = os.statvfs(sd)
        total, free = usage.f_blocks * usage.f_frsize, usage.f_bavail * usage.f_frsize
        result["sd"] = {"ready": True, "path": str(sd), "total": total,
                        "used": total - free, "free": free,
                        "music": len(core.visible_files(sd / "music", ".mp3")),
                        "books": len(core.visible_files(sd / "books", ".txt")),
                        "notes": len(core.visible_files(sd / "notes", ".txt")), "error": ""}
    except Exception as exc:
        result["sd"]["error"] = str(exc)
    job_running = result["job"]["state"] == "RUNNING"
    result["ready_build"] = IDF_EXPORT.is_file() and not job_running
    result["ready_flash"] = bool(result["usb_ports"]) and result["ready_build"] and not job_running
    result["ready_import"] = result["sd"]["ready"] and not job_running
    return result


def idf_command(action, port=None):
    export = shlex.quote(str(IDF_EXPORT))
    command = f"source {export} >/dev/null && idf.py build" if action == "build" else \
              f"source {export} >/dev/null && idf.py -p {shlex.quote(port)} flash"
    return ["/bin/zsh", "-lc", command]


class Handler(BaseHTTPRequestHandler):
    server_version = "ABVxCompanion/0.1"

    def _headers(self, content_type):
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data:")

    def _host_ok(self):
        return self.headers.get("Host", "").split(":", 1)[0] in ("127.0.0.1", "localhost")

    def _json(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False).encode()
        self.send_response(status)
        self._headers("application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _error(self, status, message):
        self._json(status, {"ok": False, "error": str(message)})

    def _read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > 4096:
            raise RuntimeError("request body too large")
        raw = self.rfile.read(length) if length else b"{}"
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            raise RuntimeError("invalid JSON body")

    def do_GET(self):
        if not self._host_ok():
            self._error(403, "invalid host")
            return
        path = urllib.parse.urlsplit(self.path).path
        if path == "/":
            body = UI_FILE.read_text(encoding="utf-8").replace("__ABVX_TOKEN__", STATE.token).encode()
            self.send_response(200)
            self._headers("text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/api/status":
            self._json(200, {"ok": True, **device_status()})
        elif path == "/api/job":
            self._json(200, {"ok": True, **STATE.snapshot()})
        elif path == "/favicon.ico":
            self.send_response(204)
            self.end_headers()
        else:
            self._error(404, "not found")

    def do_POST(self):
        if not self._host_ok() or not secrets.compare_digest(self.headers.get("X-ABVX-Token", ""), STATE.token):
            self._error(403, "forbidden")
            return
        route = urllib.parse.urlsplit(self.path)
        try:
            if route.path == "/api/import":
                self._import_file(urllib.parse.parse_qs(route.query))
            elif route.path == "/api/time-sync":
                payload = validate_payload_json(self._read_json())
                if "url" in payload and not isinstance(payload["url"], str):
                    raise RuntimeError("invalid URL type")
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    core.sync_time(payload.get("url", "http://192.168.4.1"))
                self._json(200, {"ok": True, "message": output.getvalue().strip() or "Time synchronized"})
            elif route.path == "/api/build":
                if not IDF_EXPORT.is_file():
                    raise RuntimeError(f"ESP-IDF export not found: {IDF_EXPORT}")
                STATE.start("BUILD", idf_command("build"))
                self._json(202, {"ok": True, "message": "Build started"})
            elif route.path == "/api/flash":
                payload = validate_payload_json(self._read_json())
                if payload.get("port") not in usb_ports():
                    raise RuntimeError("selected USB port is not available")
                port = payload.get("port", "")
                if payload.get("confirm") is not True:
                    raise RuntimeError("flash confirmation required")
                if not IDF_EXPORT.is_file():
                    raise RuntimeError(f"ESP-IDF export not found: {IDF_EXPORT}")
                STATE.start("FLASH", idf_command("flash", port))
                self._json(202, {"ok": True, "message": f"Flash started on {port}"})
            elif route.path == "/api/shutdown":
                if STATE.snapshot()["state"] == "RUNNING":
                    raise RuntimeError("wait for the active build/flash job")
                self._json(200, {"ok": True, "message": "Companion stopped"})
                threading.Thread(target=self.server.shutdown, daemon=True).start()
            else:
                self._error(404, "not found")
        except Exception as exc:
            self._error(400, exc)

    def _import_file(self, query):
        kind = query.get("kind", [""])[0]
        filename = validate_import_request(kind, query.get("filename", [""])[0])
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_IMPORT_BYTES:
            raise RuntimeError("invalid file size")
        if not IMPORT_LOCK.acquire(blocking=False):
            raise RuntimeError("another import is running")
        try:
            sd = core.resolve_sd(SD_OVERRIDE)
            if not sd.is_dir():
                raise RuntimeError(f"SD is not a directory: {sd}")
            with tempfile.TemporaryDirectory(prefix="abvx-import-") as directory:
                source, remaining = Path(directory) / filename, length
                with source.open("wb") as output:
                    while remaining:
                        chunk = self.rfile.read(min(1024 * 1024, remaining))
                        if not chunk:
                            raise RuntimeError("upload ended early")
                        output.write(chunk)
                        remaining -= len(chunk)
                log = io.StringIO()
                with contextlib.redirect_stdout(log):
                    core.add_books(sd, [str(source)]) if kind == "book" else core.add_music(sd, [str(source)])
            self._json(200, {"ok": True, "message": log.getvalue().strip()})
        finally:
            IMPORT_LOCK.release()

    def log_message(self, format_string, *args):
        return


def main():
    global SD_OVERRIDE
    parser = argparse.ArgumentParser(description="ABVx Mac Companion local UI")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-open", action="store_true")
    parser.add_argument("--sd", help="explicit mounted SD root (also useful for testing)")
    args = parser.parse_args()
    SD_OVERRIDE = args.sd
    if not UI_FILE.is_file():
        raise SystemExit(f"ERROR: UI file missing: {UI_FILE}")
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{args.port}"
    print(f"ABVx Companion: {url}\nPress Ctrl+C to stop.")
    if not args.no_open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
