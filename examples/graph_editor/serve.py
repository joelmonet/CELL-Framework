#!/usr/bin/env python3
"""Run the Cell Graph Editor and render local WAV auditions."""
import argparse
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from threading import BoundedSemaphore


ASSETS = Path(__file__).resolve().parent
MAX_GRAPH = 512 * 1024
MAX_WAVE = 32 * 1024 * 1024
MAX_RENDER = 128 * 1024 * 1024
MAX_PROJECT = MAX_GRAPH + MAX_WAVE + 64
STATIC = {"/": ("index.html", "text/html; charset=utf-8"),
          "/editor.js": ("editor.js", "text/javascript; charset=utf-8")}


class EditorServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port, demo):
        super().__init__(("127.0.0.1", port), EditorHandler)
        self.demo = demo
        self.host = f"127.0.0.1:{self.server_port}"
        self.origin = f"http://{self.host}"
        self.renderer = BoundedSemaphore(1)


class EditorHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def send_body(self, status, mime, size, headers=()):
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(size))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy",
                         "default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; "
                         "connect-src 'self'; media-src blob:; frame-ancestors 'none'")
        for key, value in headers:
            self.send_header(key, str(value))
        self.end_headers()

    def json_reply(self, status, value):
        data = json.dumps(value).encode("utf-8")
        self.send_body(status, "application/json", len(data))
        self.wfile.write(data)

    def do_GET(self):
        if self.headers.get("Host") != self.server.host:
            self.send_error(HTTPStatus.FORBIDDEN)
        elif self.path == "/capabilities":
            self.json_reply(HTTPStatus.OK, {"render": True, "maximumWaveBytes": MAX_WAVE,
                                           "maximumProjectBytes": MAX_PROJECT})
        elif self.path in STATIC:
            name, mime = STATIC[self.path]
            data = (ASSETS / name).read_bytes()
            self.send_body(HTTPStatus.OK, mime, len(data))
            self.wfile.write(data)
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        if (self.headers.get("Host") != self.server.host or
                self.headers.get("Origin") != self.server.origin):
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        if self.path not in ("/render", "/open-project", "/save-project"):
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        if self.headers.get_content_type() != "application/octet-stream":
            self.send_error(HTTPStatus.UNSUPPORTED_MEDIA_TYPE)
            return
        try:
            size = int(self.headers.get("Content-Length", ""))
            graph_size = 0 if self.path == "/open-project" else int(self.headers.get("X-Cell-Graph-Bytes", ""))
        except ValueError:
            self.json_reply(HTTPStatus.BAD_REQUEST, {"error": "Missing graph or audio length."})
            return
        if self.path == "/open-project":
            if not 0 < size <= MAX_PROJECT:
                self.json_reply(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": "Choose a smaller Cell project."})
                return
        elif not 0 < graph_size <= MAX_GRAPH or not 12 <= size - graph_size <= MAX_WAVE:
            self.json_reply(HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                            {"error": "Use a graph up to 512 KiB and a WAV up to 32 MiB."})
            return
        if not self.server.renderer.acquire(blocking=False):
            self.json_reply(HTTPStatus.SERVICE_UNAVAILABLE, {"error": "Another graph operation is in progress."})
            return
        try:
            with tempfile.TemporaryDirectory(prefix="cell-graph-editor-") as directory:
                if self.path == "/open-project":
                    self.open_project(Path(directory), size)
                else:
                    self.graph_and_wave(Path(directory), graph_size, size - graph_size)
        except subprocess.TimeoutExpired:
            self.json_reply(HTTPStatus.GATEWAY_TIMEOUT, {"error": "The graph operation timed out."})
        except ValueError as error:
            self.json_reply(HTTPStatus.UNPROCESSABLE_ENTITY, {"error": str(error)})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            return
        finally:
            self.server.renderer.release()

    def read_upload(self, destination, remaining, header=b""):
        with destination.open("wb") as output:
            output.write(header)
            while remaining:
                chunk = self.rfile.read(min(remaining, 65536))
                if not chunk:
                    raise ValueError("The upload was incomplete.")
                output.write(chunk)
                remaining -= len(chunk)

    def run_cell(self, command, *paths):
        result = subprocess.run([str(self.server.demo), command, *map(str, paths)],
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=30, check=False)
        if result.returncode != 0:
            raise ValueError(result.stderr.strip()[-1000:] or "Cell could not complete the graph operation.")

    def send_files(self, mime, *files, headers=()):
        self.send_body(HTTPStatus.OK, mime, sum(file.stat().st_size for file in files), headers)
        for file in files:
            with file.open("rb") as stream:
                shutil.copyfileobj(stream, self.wfile, length=65536)

    def open_project(self, directory, size):
        project, graph, audio = (directory / name for name in ("input.cellgraph", "graph.json", "input.wav"))
        self.read_upload(project, size)
        self.run_cell("extract-graph", project, graph)
        self.run_cell("extract-wave", project, audio)
        graph_size = graph.stat().st_size
        if graph_size > MAX_GRAPH or audio.stat().st_size > MAX_WAVE:
            raise ValueError("The project needs a graph up to 512 KiB and embedded WAV up to 32 MiB.")
        self.send_files("application/octet-stream", graph, audio, headers=(("X-Cell-Graph-Bytes", graph_size),))

    def graph_and_wave(self, directory, graph_size, wave_size):
        graph, source = directory / "graph.json", directory / "input.wav"
        self.read_upload(graph, graph_size)
        header = self.rfile.read(12)
        if header[:4] not in (b"RIFF", b"RF64") or header[8:] != b"WAVE":
            raise ValueError("Choose a RIFF or RF64 WAV file.")
        self.read_upload(source, wave_size - len(header), header)
        if self.path == "/save-project":
            output = directory / "edited.cellgraph"
            self.run_cell("pack", graph, source, output)
            if output.stat().st_size > MAX_PROJECT:
                raise ValueError("The saved project is too large.")
            self.send_files("application/octet-stream", output)
        else:
            output = directory / "render.wav"
            self.run_cell("headless", graph, source, output)
            if output.stat().st_size > MAX_RENDER:
                raise ValueError("The rendered WAV is too large for audition. Try a shorter input.")
            self.send_files("audio/wav", output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", required=True, type=Path, help="Path to the built cell_graph_demo executable")
    parser.add_argument("--port", type=int, default=8765, help="Local HTTP port")
    args = parser.parse_args()
    demo = args.demo.resolve()
    if not demo.is_file():
        parser.error("The cell_graph_demo executable was not found")
    try:
        with EditorServer(args.port, demo) as server:
            print(f"Cell Graph Editor: {server.origin}/", flush=True)
            server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
