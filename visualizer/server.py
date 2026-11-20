#!/usr/bin/env python3
"""HTTP server for the Compiler Visualizer: static files plus a JSON API.

Role: the only process the browser talks to. It serves visualizer/static/ and
forwards API requests to pipelines.py (compilers and toolchain) and gemini.py
(AI explanations). Python standard library only.

Usage: python3 visualizer/server.py [--port N] [--host ADDR]

Port selection order: --port, then PORT from the environment or .env (each
used only if it is actually free), then the first free port in 8100-8999.
"""

import argparse
import json
import mimetypes
import os
import socket
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gemini      # noqa: E402
import pipelines   # noqa: E402
import adapters    # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
STATIC = Path(__file__).resolve().parent / "static"
PORT_RANGE = (8100, 8999)
AVOID_PORTS = {3000, 5000, 8000, 8080, 8888}
MAX_BODY_BYTES = 1024 * 1024


def load_dotenv(path):
    """Read KEY=VALUE lines from .env into os.environ (existing variables win)."""
    if not path.is_file():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip().strip('"').strip("'")
        if key.strip() and value and key.strip() not in os.environ:
            os.environ[key.strip()] = value


def listening_ports():
    """Ports with a listening TCP socket, according to `ss -ltn`."""
    try:
        out = subprocess.run(["ss", "-ltnH"], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return set()
    ports = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and ":" in parts[3]:
            tail = parts[3].rsplit(":", 1)[1]
            if tail.isdigit():
                ports.add(int(tail))
    return ports


def can_bind(host, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def choose_port(host, preferred):
    """Return (port, note). `preferred` is used only if it is free."""
    busy = listening_ports()
    note = None
    if preferred is not None:
        if preferred not in busy and can_bind(host, preferred):
            return preferred, None
        note = f"Port {preferred} is in use; searching {PORT_RANGE[0]}-{PORT_RANGE[1]} instead."
    for port in range(PORT_RANGE[0], PORT_RANGE[1] + 1):
        if port in AVOID_PORTS or port in busy:
            continue
        if can_bind(host, port):
            return port, note
    raise SystemExit(f"No free port found in {PORT_RANGE[0]}-{PORT_RANGE[1]}.")


class Handler(BaseHTTPRequestHandler):
    server_version = "CompilerVisualizer/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[http] " + (fmt % args) + "\n")

    # ---- helpers ---------------------------------------------------------
    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_static(self, url_path):
        rel = "index.html" if url_path in ("", "/") else url_path.lstrip("/")
        target = (STATIC / rel).resolve()
        if STATIC.resolve() not in target.parents or not target.is_file():
            self.send_json({"error": "not found"}, 404)
            return
        body = target.read_bytes()
        ctype = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype.endswith("javascript"):
            ctype += "; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_json_body(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            raise pipelines.PipelineError("bad Content-Length")
        if length > MAX_BODY_BYTES:
            raise pipelines.PipelineError("request body too large")
        try:
            body = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise pipelines.PipelineError("request body is not valid JSON")
        if not isinstance(body, dict):
            raise pipelines.PipelineError("request body must be a JSON object")
        return body

    # ---- routing ---------------------------------------------------------
    def do_GET(self):
        url = urlparse(self.path)
        try:
            if url.path == "/api/health":
                self.send_json({"status": "ok", "tools": pipelines.tool_versions(), "gemini": gemini.status(),
                                "limits": {"max_source_bytes": pipelines.MAX_SOURCE_BYTES,
                                           "compile_timeout_s": pipelines.COMPILE_TIMEOUT,
                                           "run_timeout_s": pipelines.RUN_TIMEOUT}})
            elif url.path == "/api/examples":
                part = parse_qs(url.query).get("part", [""])[0]
                self.send_json({"part": part, "examples": pipelines.list_examples(part)})
            elif url.path == "/api/compilers":
                self.send_json({"compilers": adapters.catalog()})
            elif url.path.startswith("/api/"):
                self.send_json({"error": "unknown endpoint"}, 404)
            else:
                self.send_static(url.path)
        except pipelines.PipelineError as e:
            self.send_json({"error": str(e)}, 400)
        except Exception as e:  # keep the server alive on unexpected errors
            self.send_json({"error": f"internal error: {e}"}, 500)

    def do_POST(self):
        url = urlparse(self.path)
        try:
            body = self.read_json_body()
            if url.path == "/api/c/compile":
                self.send_json(pipelines.compile_c(body.get("source"), body.get("opt", 0)))
            elif url.path == "/api/c/gcc":
                self.send_json(pipelines.gcc_pipeline(body.get("source"), body.get("opt", 0)))
            elif url.path == "/api/ai/compile":
                self.send_json(pipelines.compile_ai(body.get("source")))
            elif url.path == "/api/compiler/compile":
                self.send_json(adapters.compile_session(body.get("compiler"), body.get("source"), body.get("opt", 0)))
            elif url.path == "/api/explain":
                self.send_json(gemini.explain(body))
            else:
                self.send_json({"error": "unknown endpoint"}, 404)
        except pipelines.PipelineError as e:
            self.send_json({"error": str(e)}, 400)
        except gemini.GeminiError as e:
            self.send_json({"error": str(e)}, e.status)
        except Exception as e:
            self.send_json({"error": f"internal error: {e}"}, 500)


def main():
    load_dotenv(ROOT / ".env")
    parser = argparse.ArgumentParser(description="Compiler Visualizer server")
    parser.add_argument("--port", type=int, default=None, help="port to use if it is free")
    parser.add_argument("--host", default="127.0.0.1", help="bind address (default: 127.0.0.1)")
    args = parser.parse_args()

    preferred = args.port
    if preferred is None and os.environ.get("PORT", "").strip().isdigit():
        preferred = int(os.environ["PORT"])
    port, note = choose_port(args.host, preferred)
    if note:
        print(note, flush=True)

    httpd = ThreadingHTTPServer((args.host, port), Handler)
    httpd.daemon_threads = True
    g = gemini.status()
    print(f"Gemini: {'configured, model ' + str(g['model']) if g['configured'] else 'not configured (no GEMINI_API_KEY)'}",
          flush=True)
    print(f"Visualizer running at http://{args.host}:{port}", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
