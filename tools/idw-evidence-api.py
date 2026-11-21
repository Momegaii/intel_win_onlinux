#!/usr/bin/env python3
"""Read-only HTTP wrapper for tests/physical/haswell-evidence.sh.

The server only runs the fixed collector script. It does not expose a shell,
accept command arguments, or read arbitrary files.
"""
import argparse
import hmac
import json
import os
import re
import secrets
import subprocess
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

EVIDENCE_FILES = (
    "environment.log",
    "lspci.log",
    "pci.log",
    "target.log",
    "drm.log",
    "modules.log",
    "summary.txt",
    "dmesg-i915.log",
)
RUN_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


def parse_args():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--script", type=Path,
                        default=root / "tests/physical/haswell-evidence.sh")
    parser.add_argument("--out-root", type=Path,
                        default=Path.home() / ".local/share/idw-evidence-api")
    parser.add_argument("--token", default=os.environ.get("IDW_EVIDENCE_TOKEN"))
    return parser.parse_args()


class State:
    def __init__(self, args):
        self.args = args
        self.token = args.token or secrets.token_urlsafe(32)
        self.lock = threading.Lock()
        args.out_root.mkdir(parents=True, exist_ok=True)


def collect(state):
    if not state.args.script.is_file():
        raise FileNotFoundError(f"collector not found: {state.args.script}")
    run_id = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + secrets.token_hex(4)
    out = state.args.out_root / run_id
    proc = subprocess.run(
        ["bash", str(state.args.script), str(out)],
        capture_output=True,
        text=True,
        timeout=180,
        check=False,
    )
    files = {}
    for name in EVIDENCE_FILES:
        path = out / name
        if path.is_file():
            files[name] = path.read_text(errors="replace")
    summary = files.get("summary.txt", "")
    result = ""
    for line in summary.splitlines():
        if line.startswith("result="):
            result = line.split("=", 1)[1]
            break
    return {
        "run_id": run_id,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "exit_code": proc.returncode,
        "result": result,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "files": files,
    }


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        server_version = "IDWEvidenceAPI/1"

        def log_message(self, fmt, *args):
            print("%s %s" % (self.address_string(), fmt % args), flush=True)

        def send_json(self, code, payload):
            body = json.dumps(payload, indent=2).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def authorized(self):
            header = self.headers.get("Authorization", "")
            if not header.startswith("Bearer "):
                return False
            return hmac.compare_digest(header[7:], state.token)

        def do_GET(self):
            path = urllib.parse.urlparse(self.path).path
            if path == "/health":
                self.send_json(200, {"ok": True})
                return
            if not self.authorized():
                self.send_json(401, {"error": "unauthorized"})
                return
            if path == "/":
                self.send_json(200, {
                    "service": "idw-evidence-api",
                    "collector": str(state.args.script),
                    "endpoints": ["GET /health", "POST /collect"],
                })
                return
            self.send_json(404, {"error": "not found"})

        def do_POST(self):
            path = urllib.parse.urlparse(self.path).path
            if path != "/collect":
                self.send_json(404, {"error": "not found"})
                return
            if not self.authorized():
                self.send_json(401, {"error": "unauthorized"})
                return
            if not state.lock.acquire(blocking=False):
                self.send_json(409, {"error": "collection already running"})
                return
            try:
                self.send_json(200, collect(state))
            except subprocess.TimeoutExpired:
                self.send_json(504, {"error": "collector timed out"})
            except Exception as exc:
                self.send_json(500, {"error": str(exc)})
            finally:
                state.lock.release()

    return Handler


def main():
    args = parse_args()
    state = State(args)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(state))
    print(f"listening on http://{args.host}:{args.port}", flush=True)
    print(f"token={state.token}", flush=True)
    print("POST /collect with Authorization: Bearer <token>", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
