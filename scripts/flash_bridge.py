#!/usr/bin/env python3
"""
Localhost bridge for the SL6801 iPod Web Flasher (https://fp101fs.github.io/sl6801-ipod/).
Wraps smartlink_flash/smtlink_dump over http://127.0.0.1:8765 with CORS + Private Network Access.

Usage:
    sudo python3 scripts/flash_bridge.py
"""
import json
import os
import subprocess
import tempfile
from http.server import BaseHTTPRequestHandler, HTTPServer

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUMPER = os.path.join(REPO_DIR, "smartlink_flash", "smtlink_dump")
IPOD_BIN = os.path.join(REPO_DIR, "firmware", "sl6801_ipod.bin")
BACKUP_BIN = os.path.join(REPO_DIR, "firmware", "sl6801_backup.bin")
HOST = "127.0.0.1"
PORT = 8765


def run_dumper(args, timeout=120):
    if not os.path.exists(DUMPER):
        return 1, f"smtlink_dump binary not found at {DUMPER}"
    try:
        p = subprocess.run([DUMPER, *args], capture_output=True, text=True, timeout=timeout)
        out = (p.stdout or "") + (p.stderr or "")
        return p.returncode, out.strip()
    except subprocess.TimeoutExpired:
        return 1, "Timed out waiting for SL6801 USB device."
    except Exception as e:
        return 1, str(e)


class BridgeHandler(BaseHTTPRequestHandler):
    def _cors_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Private-Network", "true")

    def _json(self, code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self._cors_headers()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors_headers()
        self.end_headers()

    def do_GET(self):
        if self.path == "/status":
            rc, out = run_dumper(["serial"], timeout=5)
            connected = (rc == 0 and "20201111000001" in out) or (rc == 0)
            self._json(200, {
                "bridge": True,
                "connected": connected,
                "root": (os.geteuid() == 0) if hasattr(os, "geteuid") else True,
                "log": out or ("SL6801 bootloader detected" if connected else "No SL6801 in bootloader mode (301a:2800)"),
            })
            return
        self._json(404, {"error": "Not found"})

    def do_POST(self):
        if self.path == "/backup":
            out_path = os.path.join(REPO_DIR, "firmware", "sl6801_backup_web.bin")
            rc, out = run_dumper(["init", "flash_id", "read_flash", "0", "2M", out_path], timeout=120)
            self._json(200 if rc == 0 else 500, {"ok": rc == 0, "path": out_path, "log": out})
            return

        if self.path == "/flash":
            length = int(self.headers.get("Content-Length", "0"))
            tmp_path = None
            target = IPOD_BIN
            if length > 0:
                data = self.rfile.read(length)
                if len(data) != 2 * 1024 * 1024:
                    self._json(400, {"ok": False, "log": f"Expected 2 MB (2097152 bytes) image, got {len(data)} bytes."})
                    return
                fd, tmp_path = tempfile.mkstemp(prefix="sl6801_", suffix=".bin")
                with os.fdopen(fd, "wb") as f:
                    f.write(data)
                target = tmp_path
            try:
                rc, out = run_dumper(["init", "write_flash", "0", "0", "0", target, "check_flash", "0xe000", "0x1ea8f0"], timeout=180)
                self._json(200 if rc == 0 else 500, {"ok": rc == 0, "log": out})
            finally:
                if tmp_path and os.path.exists(tmp_path):
                    os.unlink(tmp_path)
            return

        if self.path == "/restore":
            if not os.path.exists(BACKUP_BIN):
                self._json(400, {"ok": False, "log": f"Backup not found at {BACKUP_BIN}"})
                return
            rc, out = run_dumper(["init", "write_flash", "0", "0", "0", BACKUP_BIN, "check_flash", "0xe000", "0x1ea8f0"], timeout=180)
            self._json(200 if rc == 0 else 500, {"ok": rc == 0, "log": out})
            return

        self._json(404, {"error": "Not found"})

    def log_message(self, fmt, *args):
        print(f"[flash_bridge] {self.address_string()} - {fmt % args}")


if __name__ == "__main__":
    print(f"SL6801 Web Flasher Bridge listening on http://{HOST}:{PORT}")
    print("Open https://fp101fs.github.io/sl6801-ipod/ and select 'Method 2: Web Flasher Bridge'.")
    HTTPServer((HOST, PORT), BridgeHandler).serve_forever()
