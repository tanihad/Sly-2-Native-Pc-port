"""A local HTTP server for the installer's download tests (tests/unit/installer/run.ps1). Never on a
network: it binds 127.0.0.1 only and serves one folder read-only.

    python range_server.py <folder> <port file> <log file>

Paths:  /files/<name>       the file, with Range support (206 Partial Content)
        /norange/<name>     ignores Range (always 200 with the whole file: the client must start over)
        /drop/<name>        the first request for each name stops half-way (the connection drops)
        anything else       404
Each request is logged as "<method> <path> range=<Range header or ->" to the log file.
"""
import http.server
import os
import sys
import threading
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve()
DROPPED: set[str] = set()
LOCK = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        rng = self.headers.get("Range")
        with LOCK, open(sys.argv[3], "a", encoding="utf-8") as log:
            log.write(f"GET {self.path} range={rng or '-'}\n")
        parts = self.path.lstrip("/").split("/", 1)
        if len(parts) != 2 or parts[0] not in ("files", "norange", "drop"):
            return self.send_error(404)
        mode, name = parts
        path = (ROOT / name).resolve()
        if path.parent != ROOT or not path.is_file():
            return self.send_error(404)
        size = path.stat().st_size
        start = 0
        if rng and mode != "norange" and rng.startswith("bytes=") and rng.endswith("-"):
            start = int(rng[6:-1])
            if start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        drop = False
        if mode == "drop":
            with LOCK:
                drop = name not in DROPPED
                DROPPED.add(name)
        if start:
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{size - 1}/{size}")
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(size - start))
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        limit = (size - start) // 2 if drop else size - start
        with open(path, "rb") as f:
            f.seek(start)
            sent = 0
            while sent < limit:
                chunk = f.read(min(1 << 16, limit - sent))
                if not chunk:
                    break
                self.wfile.write(chunk)
                sent += len(chunk)
        if drop:
            self.wfile.flush()
            self.close_connection = True
            self.connection.shutdown(2)


def main():
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    Path(sys.argv[2]).write_text(str(srv.server_address[1]), encoding="ascii")
    srv.serve_forever()


if __name__ == "__main__":
    os.chdir(ROOT)
    main()
