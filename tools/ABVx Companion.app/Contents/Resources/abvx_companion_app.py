#!/usr/bin/env python3
"""Local-only browser UI for ABVx Mac Companion."""

import argparse
import contextlib
import glob
import io
import json
import os
import secrets
import sys
import datetime
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
from needle_intent_adapter import build_intent_adapter

PROJECT_ROOT = Path(os.environ.get("ABVX_PROJECT_ROOT", Path(__file__).resolve().parent.parent)).expanduser().resolve()
UI_FILE = Path(__file__).resolve().parent / "companion_ui" / "index.html"
IDF_EXPORT = Path.home() / "esp/esp-idf-v5.4.2/export.sh"
BACKUP_ROOT = Path.home() / "ABVxCompanionBackup"
BACKUP_STATE = BACKUP_ROOT / ".state" / "sync-status.json"
BACKUP_TRACKS = BACKUP_ROOT / "Tracks"
BACKUP_NOTES = BACKUP_ROOT / "Notes"
BACKUP_VOICE = BACKUP_ROOT / "Voice"
TRACKS = "tracks"
NOTES = "notes"
VOICE = "voice"
MAX_IMPORT_BYTES = 128 * 1024 * 1024
MAX_JOB_OUTPUT = 24000
IMPORT_EXTENSIONS = {"book": {".txt", ".epub", ".fb2"}, "music": {".mp3"}}
SYNC_KEYS = (TRACKS, NOTES, VOICE)
IMPORT_LOCK = threading.Lock()
SD_OVERRIDE = None
INTENT_ADAPTER = build_intent_adapter(os.environ.get("ABVX_INTENT_ADAPTER", "rule_based"))


def now_stamp():
    return datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"


def ensure_backup_catalog():
    for path in (BACKUP_TRACKS, BACKUP_NOTES, BACKUP_VOICE, BACKUP_STATE.parent):
        path.mkdir(parents=True, exist_ok=True)


def default_sync_state():
    return {key: {"state": "IDLE", "last_sync": None, "last_file": "", "last_error": ""}
            for key in SYNC_KEYS}


def load_sync_state():
    if not BACKUP_STATE.is_file():
        return default_sync_state()
    try:
        data = json.loads(BACKUP_STATE.read_text(encoding="utf-8"))
    except Exception:
        return default_sync_state()
    result = default_sync_state()
    for key in SYNC_KEYS:
        value = data.get(key) if isinstance(data, dict) else None
        if isinstance(value, dict):
            for field in result[key]:
                result[key][field] = value.get(field, result[key][field])
    return result


def save_sync_state(state):
    data = json.dumps(state, ensure_ascii=False, indent=2)
    BACKUP_STATE.write_text(data, encoding="utf-8")


class AppState:
    def __init__(self):
        self.token = secrets.token_urlsafe(24)
        self.lock = threading.Lock()
        self.job_name = ""
        self.job_state = "IDLE"
        self.job_returncode = None
        self.job_output = deque(maxlen=240)
        self.job_queue = deque(maxlen=10)
        self.sync = load_sync_state()
        self.pending_intent = None

    def snapshot(self):
        with self.lock:
            return {
                "name": self.job_name,
                "state": self.job_state,
                "returncode": self.job_returncode,
                "output": "".join(self.job_output)[-MAX_JOB_OUTPUT:],
                "queue": list(self.job_queue),
                "sync": {k: v.copy() for k, v in self.sync.items()},
                "pending_intent": dict(self.pending_intent) if isinstance(self.pending_intent, dict) else None,
            }

    def set_pending_intent(self, pending):
        with self.lock:
            self.pending_intent = dict(pending) if isinstance(pending, dict) else None

    def clear_pending_intent(self):
        with self.lock:
            self.pending_intent = None

    def set_sync(self, key, state, *, last_file="", last_error="", done=False):
        if key not in SYNC_KEYS:
            return
        with self.lock:
            entry = self.sync.setdefault(key, {"state": "IDLE", "last_sync": None, "last_file": "", "last_error": ""})
            normalized_state = str(state).strip().upper()
            if normalized_state in ("DONE", "FAILED", "PENDING", "IDLE"):
                entry["state"] = normalized_state
            else:
                entry["state"] = state
            if last_file:
                entry["last_file"] = last_file
            if normalized_state == "FAILED":
                entry["last_error"] = "operation failed"
                if last_error:
                    entry["last_error"] = last_error
            elif last_error:
                entry["last_error"] = last_error
            elif normalized_state in ("DONE", "IDLE"):
                entry["last_error"] = ""
            if done:
                entry["last_sync"] = now_stamp()
            save_sync_state(self.sync)

    def set_sync_fail(self, key, *, last_file="", last_error=""):
        self.set_sync(key, "FAILED", last_file=last_file, done=True, last_error=last_error)

    def set_sync_pending(self, key, *, last_file=""):
        self.set_sync(key, "PENDING", last_file=last_file, done=False, last_error="")

    def set_sync_done(self, key, *, last_file="", last_error=""):
        self.set_sync(key, "DONE", last_file=last_file, done=True, last_error=last_error)

    def start(self, name, command):
        with self.lock:
            if self.job_state == "RUNNING":
                raise RuntimeError(f"{self.job_name} is already running")
            self.job_name, self.job_state, self.job_returncode = name, "RUNNING", None
            self.job_output.clear()
            self.job_output.append(f"{name} started\n")
            self.job_queue.append({
                "name": name,
                "state": "RUNNING",
                "returncode": None
            })
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
            if self.job_queue:
                self.job_queue[-1]["state"] = self.job_state
                self.job_queue[-1]["returncode"] = returncode
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
    result["intent_adapter"] = INTENT_ADAPTER.descriptor()
    result["backup"] = {
        "ready": BACKUP_ROOT.is_dir(),
        "path": str(BACKUP_ROOT),
        "tracks": len([path for path in BACKUP_TRACKS.iterdir() if path.is_file() and not path.name.startswith(".")]) if BACKUP_TRACKS.is_dir() else 0,
        "notes": len([path for path in BACKUP_NOTES.iterdir() if path.is_file() and not path.name.startswith(".")]) if BACKUP_NOTES.is_dir() else 0,
        "voice": len([path for path in BACKUP_VOICE.iterdir() if path.is_file() and not path.name.startswith(".")]) if BACKUP_VOICE.is_dir() else 0,
    }
    with STATE.lock:
        result["sync"] = dict(STATE.sync)
    firmware = PROJECT_ROOT / "build/cardputer-abvx-minimal.bin"
    if firmware.is_file():
        result["firmware"] = {"ready": True, "path": str(firmware), "size": firmware.stat().st_size}
    try:
        sd = core.resolve_sd(SD_OVERRIDE)
        usage = os.statvfs(sd)
        total, free = usage.f_blocks * usage.f_frsize, usage.f_bavail * usage.f_frsize
        activities_count = len(core.visible_files(sd / "activities", ".json")) + \
                          len(core.visible_files(sd / "activities", ".txt")) + \
                          len(core.visible_files(sd / "activities", ".gpx"))
        result["sd"] = {"ready": True, "path": str(sd), "total": total,
                        "used": total - free, "free": free,
                        "music": len(core.visible_files(sd / "music", ".mp3")),
                        "books": len(core.visible_files(sd / "books", ".txt")),
                        "notes": len(core.visible_files(sd / "notes", ".txt")),
                        "activities": activities_count,
                        "error": ""}
    except Exception as exc:
        result["sd"]["error"] = str(exc)
    job_running = result["job"]["state"] == "RUNNING"
    result["ready_build"] = IDF_EXPORT.is_file() and not job_running
    result["ready_flash"] = bool(result["usb_ports"]) and result["ready_build"] and not job_running
    result["ready_import"] = result["sd"]["ready"] and not job_running
    return result


def pipeline_command(section):
    sd = core.resolve_sd(SD_OVERRIDE)
    script = PROJECT_ROOT / "tools" / "cardputer_local_pipeline.py"
    return [sys.executable, str(script), f"sync-{section}", "--deploy", "--sd", str(sd)]


def time_sync_command(url="http://192.168.4.1"):
    script = PROJECT_ROOT / "tools" / "abvx_companion.py"
    return [sys.executable, str(script), "sync-time", "--url", url]


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
        try:
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
            elif path == "/api/activities":
                sd = core.resolve_sd(SD_OVERRIDE)
                items = core.list_activities(sd)
                self._json(200, {"ok": True, "count": len(items), "items": items})
            elif path == "/api/activity":
                query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
                activity_id = query.get("id", [""])[0]
                if not activity_id:
                    self._error(400, "missing id")
                    return
                sd = core.resolve_sd(SD_OVERRIDE)
                path_entry = core.read_activity_file(sd, activity_id)
                payload = path_entry.read_text(encoding="utf-8", errors="replace")
                self._json(200, {"ok": True,
                                "id": activity_id,
                                "name": path_entry.name,
                                "size": path_entry.stat().st_size,
                                "content": payload})
            elif path == "/favicon.ico":
                self.send_response(204)
                self.end_headers()
            else:
                self._error(404, "not found")
        except Exception as exc:
            self._error(400, str(exc))

    def do_POST(self):
        if not self._host_ok() or not secrets.compare_digest(self.headers.get("X-ABVX-Token", ""), STATE.token):
            self._error(403, "forbidden")
            return
        route = urllib.parse.urlsplit(self.path)
        try:
            if route.path == "/api/intent/resolve":
                status = device_status()
                payload = validate_payload_json(self._read_json())
                resolved = INTENT_ADAPTER.resolve(payload, status)
                execution = None
                if resolved["status"] == "ok" and not resolved["requires_confirmation"]:
                    execution = self._execute_confirmed_intent(resolved["intent"], resolved["arguments"])
                    STATE.clear_pending_intent()
                elif resolved["status"] == "ok":
                    STATE.set_pending_intent(resolved)
                else:
                    STATE.clear_pending_intent()
                self._json(200, {
                    "ok": True,
                    "adapter": INTENT_ADAPTER.name,
                    "resolved": resolved,
                    "execution": execution,
                    "pending_intent": STATE.snapshot()["pending_intent"],
                })
            elif route.path == "/api/intent/cancel":
                STATE.clear_pending_intent()
                self._json(200, {"ok": True, "message": "Pending intent cleared"})
            elif route.path == "/api/intent/confirm":
                payload = validate_payload_json(self._read_json())
                pending = STATE.snapshot().get("pending_intent")
                if not pending:
                    raise RuntimeError("no pending intent")
                if payload.get("confirm") is not True:
                    raise RuntimeError("intent confirmation required")
                result = self._execute_confirmed_intent(pending["intent"], pending["arguments"])
                STATE.clear_pending_intent()
                self._json(200, {"ok": True, "result": result})
            elif route.path == "/api/import":
                self._import_file(urllib.parse.parse_qs(route.query))
            elif route.path == "/api/sync-notes":
                payload = validate_payload_json(self._read_json())
                self._sync_notes(delete_after=bool(payload.get("delete_after", False)))
            elif route.path == "/api/sync-voice":
                payload = validate_payload_json(self._read_json())
                self._sync_voice(delete_after=bool(payload.get("delete_after", False)))
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

    def _execute_confirmed_intent(self, intent, arguments):
        arguments = core.validate_intent_arguments(intent, arguments)
        if intent == "sd_status":
            return {"message": "status loaded", "status": device_status()}
        if intent == "sync_time":
            STATE.start("TIME SYNC", time_sync_command())
            return {"message": "Time sync started"}
        if intent == "sync_music":
            STATE.start("SYNC MUSIC", pipeline_command("music"))
            return {"message": "Music sync started"}
        if intent == "sync_books":
            STATE.start("SYNC BOOKS", pipeline_command("books"))
            return {"message": "Books sync started"}
        if intent == "sync_voice":
            self._sync_voice(delete_after=arguments["delete_after"])
            return {"message": "Voice sync complete"}
        if intent == "prepare_browser_package":
            raise RuntimeError("browser package preparation is not implemented")
        raise RuntimeError(f"unsupported confirmed intent: {intent}")

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
                    if kind == "music":
                        STATE.set_sync_pending(TRACKS, last_file=filename)
                    try:
                        core.add_books(sd, [str(source)]) if kind == "book" else core.add_music(sd, [str(source)])
                    except Exception:
                        if kind == "music":
                            STATE.set_sync_fail(TRACKS, last_file=filename)
                        raise
                    if kind == "music":
                        STATE.set_sync_done(TRACKS, last_file=filename)
                self._json(200, {"ok": True, "message": log.getvalue().strip()})
        finally:
            IMPORT_LOCK.release()

    def _sync_notes(self, delete_after=False):
        STATE.set_sync_pending(NOTES, last_file="pull")
        try:
            sd = core.resolve_sd(SD_OVERRIDE)
            if not sd.is_dir():
                raise RuntimeError(f"SD is not a directory: {sd}")
            core.pull_notes(sd, BACKUP_NOTES, delete_after=delete_after)
            STATE.set_sync_done(NOTES, last_file="pull")
            self._json(200, {"ok": True, "message": "notes sync complete"})
        except Exception as exc:
            STATE.set_sync_fail(NOTES, last_file="pull", last_error=str(exc))
            raise

    def _sync_voice(self, delete_after=False):
        STATE.set_sync_pending(VOICE, last_file="pull")
        try:
            sd = core.resolve_sd(SD_OVERRIDE)
            if not sd.is_dir():
                raise RuntimeError(f"SD is not a directory: {sd}")
            core.pull_recordings(sd, BACKUP_VOICE, delete_after=delete_after)
            STATE.set_sync_done(VOICE, last_file="pull")
            self._json(200, {"ok": True, "message": "voice sync complete"})
        except Exception as exc:
            STATE.set_sync_fail(VOICE, last_file="pull", last_error=str(exc))
            raise

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
    ensure_backup_catalog()
    save_sync_state(STATE.sync)
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
