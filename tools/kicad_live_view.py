#!/usr/bin/env python3
"""Live, read-only PCB viewer — watch a .kicad_pcb change while an agent edits it.

Serves a local page that renders the board in 2D with KiCanvas (a browser-side
KiCad renderer) and reloads it whenever the file on disk changes, plus a 3D tab
driven by kicad-cli. Nothing here opens the board in KiCad: no `.lck` lock file,
no editor cache, no risk of the GUI holding a stale copy and clobbering an
agent's write. It only ever reads.

    python tools/kicad_live_view.py                     # newest board under kicad/
    python tools/kicad_live_view.py path/to/x.kicad_pcb
    python tools/kicad_live_view.py --port 8010 --no-browser

Mid-write safety: a save is not atomic from the reader's side, so every re-read
is validated (starts with `(kicad_pcb`, parens balance) before it is published.
A half-written file is never served -- the viewer keeps showing the last good
snapshot and flags the file as being written.

Why 3D is built from a temp copy, not from the board in place
------------------------------------------------------------
kicad-cli is handed the exact bytes the 2D view is showing, so the two tabs can
never disagree and a build can never catch a half-written save. That relocates
the board, which would silently drop every 3D model -- the footprints reference
`${KIPRJMOD}/EASYEDA_MODELS/...`, and KIPRJMOD follows the file. So the copy is
rendered with `-D KIPRJMOD=<original board dir>`. Measured on board3: without it
you get a bare green board and no error at all.

3D cost, measured on board3 (150 footprints, ~53 MB of STEP), which is why
snapshots are the default and GLB is on request:

    render (PNG, fixed camera)      3.1 s     0.10 MB
    export glb, board only          1.2 s     0.24 MB   (useless: no parts)
    export glb + components        26.3 s    13.5   MB
    export glb + copper            21.7 s    17.8   MB
    export glb + both              46.7 s    31.1   MB
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import webbrowser
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from kicad_env import KiCadNotFound, find_kicad_cli  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
HERE = Path(__file__).resolve().parent
VIEWER_HTML = HERE / "kicad_live_view.html"
KICANVAS_CACHE = HERE / "vendor" / "kicanvas.js"
KICANVAS_URL = "https://kicanvas.org/kicanvas/kicanvas.js"
MODELVIEWER_CACHE = HERE / "vendor" / "model-viewer.min.js"
MODELVIEWER_URL = "https://ajax.googleapis.com/ajax/libs/model-viewer/4.1.0/model-viewer.min.js"

POLL_SECONDS = 0.3

# A build is ~3 s at best and ~47 s at worst, so never start one on a board that
# is still being written to -- wait for the file to hold still first.
BUILD_DEBOUNCE = 1.0
BUILD_TIMEOUT = 240

# Camera presets for `pcb render`. --side gives the six orthogonal views; the
# isometrics are a side plus a rotation, which is the only way to get an angled
# camera out of the CLI.
VIEWS = {
    "iso": ["--side", "top", "--perspective", "--rotate", "-25,0,25"],
    "top": ["--side", "top"],
    "front": ["--side", "front"],
    "right": ["--side", "right"],
    "iso-bot": ["--side", "bottom", "--perspective", "--rotate", "-25,0,25"],
    "bottom": ["--side", "bottom"],
}

# Copper, silk and mask as real geometry. Roughly doubles GLB build time and
# size, so it is opt-in rather than the default.
GLB_DETAIL = [
    "--include-tracks",
    "--include-pads",
    "--include-zones",
    "--include-silkscreen",
    "--include-soldermask",
]


def tracked_boards(paths: list[Path]) -> list[Path]:
    """Those of `paths` that git tracks. Empty on any git trouble -- advisory only."""
    try:
        done = subprocess.run(
            ["git", "-C", str(REPO), "ls-files", "--", "*.kicad_pcb"],
            capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return []
    if done.returncode != 0:
        return []
    known = {(REPO / line.strip()).resolve() for line in done.stdout.splitlines() if line.strip()}
    return [p for p in paths if p.resolve() in known]


def newest_board() -> Path:
    """The board to watch: newest, but preferring one git actually tracks.

    Plain newest-wins is a trap this repo has now hit twice. Routing and cascade
    experiments leave variant boards (`...-c0.kicad_pcb`, `...-it.kicad_pcb`)
    beside the real one and they are always newer, so a glob lands on scratch --
    tools/kicad_render.py picked one up exactly this way on 2026-07-28, and this
    viewer then spent a session watching a file another agent deleted underneath
    it. Tracked files are the design; untracked neighbours are working scratch.
    """
    boards = [
        p
        for p in REPO.glob("kicad/**/*.kicad_pcb")
        if "-backups" not in str(p) and not p.name.startswith("_autosave")
    ]
    if not boards:
        sys.exit("no .kicad_pcb found under kicad/ - pass one explicitly")
    return max(tracked_boards(boards) or boards, key=lambda p: p.stat().st_mtime)


def is_complete(data: bytes) -> bool:
    """True if `data` looks like a whole board file, not a half-finished write.

    Balances parens outside of quoted strings. KiCad field values legitimately
    contain parens (`"Cap (X7R)"`), so a naive count is not enough.
    """
    if not data.lstrip().startswith(b"(kicad_pcb"):
        return False
    depth = 0
    in_str = False
    escaped = False
    for ch in data:
        if in_str:
            if escaped:
                escaped = False
            elif ch == 0x5C:  # backslash
                escaped = True
            elif ch == 0x22:  # "
                in_str = False
            continue
        if ch == 0x22:
            in_str = True
        elif ch == 0x28:  # (
            depth += 1
        elif ch == 0x29:  # )
            depth -= 1
            if depth < 0:
                return False
    return depth == 0 and not in_str


class BoardWatcher:
    """Polls one file, publishes only complete snapshots of it."""

    def __init__(self, path: Path):
        self.path = path
        self.lock = threading.Lock()
        self.data = b""
        self.rev = 0
        self.stamp = 0.0
        self.writing = False  # last read caught a partial file
        self.error = ""
        self._sig = None
        self._stop = threading.Event()
        self.refresh()

    def signature(self):
        st = self.path.stat()
        return (st.st_mtime_ns, st.st_size)

    def refresh(self) -> None:
        try:
            sig = self.signature()
        except OSError as exc:
            with self.lock:
                self.error = f"{type(exc).__name__}: {exc}"
            return
        if sig == self._sig:
            return
        try:
            data = self.path.read_bytes()
        except OSError:
            # Windows can deny the read outright mid-save; try again next poll.
            with self.lock:
                self.writing = True
            return
        if not is_complete(data):
            with self.lock:
                self.writing = True
            return
        self._sig = sig
        with self.lock:
            self.data = data
            self.rev += 1
            self.stamp = time.time()
            self.writing = False
            self.error = ""

    def run(self) -> None:
        while not self._stop.wait(POLL_SECONDS):
            try:
                self.refresh()
            except Exception as exc:  # a viewer must never die on the watcher
                with self.lock:
                    self.error = f"{type(exc).__name__}: {exc}"

    def start(self) -> None:
        threading.Thread(target=self.run, daemon=True).start()

    def status(self) -> dict:
        with self.lock:
            return {
                "rev": self.rev,
                "name": self.path.name,
                "path": str(self.path),
                "bytes": len(self.data),
                "stamp": self.stamp,
                "writing": self.writing,
                "error": self.error,
            }

    def snapshot(self) -> bytes:
        with self.lock:
            return self.data


class Builder:
    """Builds 3D artefacts from the watcher's snapshot, one at a time, in back.

    The page declares what it is looking at (`want`); this builds that and only
    that. Nothing is built while the 2D tab is open, because a 3D build costs
    seconds of CPU and nobody would see it.
    """

    def __init__(self, watcher: BoardWatcher):
        self.watcher = watcher
        self.lock = threading.Lock()
        self.want = {"kind": "none", "view": "iso", "detail": False}
        self.cache: OrderedDict[tuple, bytes] = OrderedDict()
        self.building: tuple | None = None
        self.error = ""
        # Per kind: a snapshot takes ~3 s and a model ~30 s, so one shared
        # number would quote the wrong build's duration after switching mode.
        self.seconds: dict[str, float] = {}
        self.tmp = Path(tempfile.mkdtemp(prefix="kicad-live-"))
        self.cli: Path | None = None
        self.cli_error = ""
        try:
            self.cli = find_kicad_cli()
        except KiCadNotFound as exc:
            self.cli_error = str(exc)

    # -- cache ---------------------------------------------------------------
    # GLBs run to 31 MB, so they are capped far tighter than the ~100 KB PNGs.
    LIMITS = {"snapshot": 12, "model": 2}

    def remember(self, key: tuple, blob: bytes) -> None:
        with self.lock:
            self.cache[key] = blob
            for kind, limit in self.LIMITS.items():
                keys = [k for k in self.cache if k[0] == kind]
                for stale in keys[: max(0, len(keys) - limit)]:
                    del self.cache[stale]

    def get(self, key: tuple) -> bytes | None:
        with self.lock:
            return self.cache.get(key)

    def key_for(self, rev: int, want: dict) -> tuple | None:
        if want["kind"] == "snapshot":
            return ("snapshot", rev, want["view"], False)
        if want["kind"] == "model":
            return ("model", rev, "-", bool(want["detail"]))
        return None

    # -- build ---------------------------------------------------------------
    def command(self, key: tuple, src: Path, dst: Path) -> list[str]:
        kind, _rev, view, detail = key
        # KIPRJMOD must point at the real board dir or every 3D model vanishes.
        common = ["-D", f"KIPRJMOD={self.watcher.path.parent}", "-o", str(dst)]
        if kind == "snapshot":
            return [
                "pcb", "render", *common,
                "--width", "1600", "--height", "1000",
                "--quality", "basic", "--background", "opaque",
                *VIEWS[view], str(src),
            ]
        return ["pcb", "export", "glb", "-f", *common, *(GLB_DETAIL if detail else []), str(src)]

    def build(self, key: tuple) -> None:
        src = self.tmp / "live.kicad_pcb"
        src.write_bytes(self.watcher.snapshot())
        dst = self.tmp / ("out.png" if key[0] == "snapshot" else "out.glb")
        dst.unlink(missing_ok=True)
        started = time.time()
        proc = subprocess.run(
            [str(self.cli), *self.command(key, src, dst)],
            capture_output=True, text=True, timeout=BUILD_TIMEOUT,
        )
        if not dst.exists():
            tail = (proc.stderr or proc.stdout or "no output").strip().splitlines()
            raise RuntimeError(tail[-1] if tail else f"kicad-cli exit {proc.returncode}")
        self.remember(key, dst.read_bytes())
        with self.lock:
            self.seconds[key[0]] = time.time() - started
            self.error = ""

    def run(self) -> None:
        while True:
            time.sleep(0.25)
            with self.lock:
                want = dict(self.want)
            if want["kind"] == "none" or self.cli is None:
                continue
            status = self.watcher.status()
            key = self.key_for(status["rev"], want)
            if key is None or self.get(key) is not None:
                continue
            # Let a burst of writes settle; also skip a board mid-save.
            if status["writing"] or time.time() - status["stamp"] < BUILD_DEBOUNCE:
                continue
            with self.lock:
                self.building = key
            try:
                self.build(key)
            except Exception as exc:
                with self.lock:
                    self.error = f"{type(exc).__name__}: {exc}"
            finally:
                with self.lock:
                    self.building = None

    def start(self) -> None:
        threading.Thread(target=self.run, daemon=True).start()

    def status(self) -> dict:
        with self.lock:
            want = dict(self.want)
            building = self.building
            # Match the *whole* key shape, not just the kind: a cached top view
            # is not a built isometric, and reporting it as ready would leave the
            # page waiting on an artefact nobody is building.
            shape = self.key_for(0, want)
            ready = (
                []
                if shape is None
                else [k[1] for k in self.cache if (k[0], k[2], k[3]) == (shape[0], shape[2], shape[3])]
            )
            return {
                "kind": want["kind"],
                "view": want["view"],
                "detail": want["detail"],
                "building": building[1] if building else None,
                "ready": max(ready) if ready else None,
                "seconds": round(self.seconds.get(want["kind"], 0.0), 1),
                "error": self.error or self.cli_error,
            }

    def cleanup(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)


def cached_bundle(path: Path, url: str, label: str, refresh: bool = False) -> bytes:
    """A vendored JS bundle, fetched once. Viewer-only, so it stays out of git."""
    if path.exists() and not refresh:
        return path.read_bytes()
    print(f"fetching {label} -> {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url, timeout=60) as resp:
        data = resp.read()
    path.write_bytes(data)
    return data


def make_handler(watcher: BoardWatcher, builder: Builder, bundles: dict[str, bytes]):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass  # the console is for the watcher, not an access log

        def send_payload(self, body: bytes, ctype: str, cache: bool = False):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            if not cache:
                self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            route, _, raw_query = self.path.partition("?")
            query = dict(
                (p.split("=", 1) + [""])[:2] for p in raw_query.split("&") if p
            )
            if route in ("/", "/index.html"):
                self.send_payload(VIEWER_HTML.read_bytes(), "text/html; charset=utf-8")
            elif route in ("/kicanvas.js", "/model-viewer.js"):
                self.send_payload(bundles[route], "text/javascript; charset=utf-8", cache=True)
            elif route == "/rev":
                # One poll answers both tabs: the page never has to ask twice.
                body = watcher.status() | {"three": builder.status()}
                self.send_payload(json.dumps(body).encode(), "application/json")
            elif route == "/board.kicad_pcb":
                self.send_payload(watcher.snapshot(), "text/plain; charset=utf-8")
            elif route == "/3d/want":
                # The page declares what it is looking at; the builder makes that.
                with builder.lock:
                    builder.want = {
                        "kind": query.get("kind", "none"),
                        "view": query.get("view", "iso") if query.get("view") in VIEWS else "iso",
                        "detail": query.get("detail") == "1",
                    }
                self.send_payload(json.dumps(builder.status()).encode(), "application/json")
            elif route == "/3d/art":
                kind = query.get("kind", "")
                key = (
                    kind,
                    int(query.get("rev", "-1")),
                    query.get("view", "-") if kind == "snapshot" else "-",
                    query.get("detail") == "1" if kind == "model" else False,
                )
                blob = builder.get(key)
                if blob is None:
                    self.send_error(404)
                else:
                    ctype = "image/png" if kind == "snapshot" else "model/gltf-binary"
                    self.send_payload(blob, ctype)
            else:
                self.send_error(404)

    return Handler


class QuietServer(ThreadingHTTPServer):
    """A browser dropping a keep-alive socket is routine, not an error.

    socketserver prints a full traceback per dropped connection and a single
    page reload drops several, so the console fills with stack traces while the
    server is perfectly healthy -- which reads exactly like a crash loop. Only
    the disconnect family is swallowed; a real fault still surfaces.
    """

    daemon_threads = True  # so Ctrl-C exits now, not after in-flight requests

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (ConnectionResetError, ConnectionAbortedError, BrokenPipeError)):
            return
        super().handle_error(request, client_address)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", nargs="?", help="path to a .kicad_pcb (default: newest under kicad/)")
    ap.add_argument("--port", type=int, default=8010)
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--refresh-viewer", action="store_true", help="re-download the KiCanvas bundle")
    args = ap.parse_args()

    board = Path(args.board).resolve() if args.board else newest_board()
    if not board.exists():
        sys.exit(f"no such board: {board}")

    bundles = {
        "/kicanvas.js": cached_bundle(
            KICANVAS_CACHE, KICANVAS_URL, "KiCanvas", args.refresh_viewer
        ),
        "/model-viewer.js": cached_bundle(
            MODELVIEWER_CACHE, MODELVIEWER_URL, "model-viewer", args.refresh_viewer
        ),
    }
    watcher = BoardWatcher(board)
    watcher.start()
    builder = Builder(watcher)
    builder.start()

    server = QuietServer(("127.0.0.1", args.port), make_handler(watcher, builder, bundles))
    url = f"http://127.0.0.1:{args.port}/"
    try:
        rel = board.relative_to(REPO)
    except ValueError:
        rel = board
    print(f"watching  {rel}")
    print(f"viewer    {url}   (Ctrl-C to stop; read-only, no lock file)")
    print(f"kicad-cli {builder.cli or 'NOT FOUND -- 2D only. ' + builder.cli_error}")
    if not args.no_browser:
        threading.Timer(0.4, webbrowser.open, args=(url,)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        builder.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
