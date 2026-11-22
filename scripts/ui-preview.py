"""Serve the built SvelteKit front end locally and proxy /rest to the trainer, so the layout
can be worked on without rebuilding and flashing the firmware for every change. The device
must be running the open (FT_SECURITY=0) image, or the proxied calls come back 401.

    pio run -e t-rgb-preview        # flash the open image first, or /rest answers 401
    cd interface && npm run build
    python3 scripts/ui-preview.py   # then open http://127.0.0.1:8095/
"""

import http.server
import pathlib
import json
import os
import socketserver
import urllib.error
import urllib.request

ROOT = str(pathlib.Path(__file__).resolve().parent.parent / "interface" / "build")
DEVICE = "http://192.168.2.248"
PORT = 8095


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def proxy(self, method):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        request = urllib.request.Request(DEVICE + self.path, data=body, method=method)
        for header in ("Content-Type", "Authorization"):
            if self.headers.get(header):
                request.add_header(header, self.headers[header])
        try:
            with urllib.request.urlopen(request, timeout=25) as response:
                payload = response.read()
                status = response.status
                content_type = response.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as error:
            payload = error.read()
            status = error.code
            content_type = error.headers.get("Content-Type", "application/json")
        except Exception as error:  # the device is a weak-link device: report, do not crash
            payload = json.dumps({"error": str(error)}).encode()
            status = 502
            content_type = "application/json"
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path.startswith("/rest/") or self.path.startswith("/_app/env.js"):
            return self.proxy("GET")
        path = self.translate_path(self.path)
        if not os.path.exists(path) or os.path.isdir(path):
            self.path = "/index.html"
        return super().do_GET()

    def do_POST(self):
        if self.path.startswith("/rest/"):
            return self.proxy("POST")
        self.send_error(404)

    def log_message(self, *args):
        pass


with socketserver.ThreadingTCPServer(("127.0.0.1", PORT), Handler) as httpd:
    httpd.allow_reuse_address = True
    print(f"serving {ROOT} on http://127.0.0.1:{PORT}, /rest proxied to {DEVICE}")
    httpd.serve_forever()
