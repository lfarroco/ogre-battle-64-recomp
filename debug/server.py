#!/usr/bin/env python3
"""Static server for the WebAssembly build (COOP/COEP + no-store).

The Emscripten build uses pthreads, which needs ``SharedArrayBuffer``, which
browsers only expose on a cross-origin-isolated page. This serves the repository
root (or ``--root``) with the two isolation headers, ``application/wasm`` for
``.wasm``, and ``Cache-Control: no-store`` so a rebuild is picked up on reload.

Usage:
    python3 debug/server.py [--port 8931] [--bind 127.0.0.1] [--root .] [--verbose]

The default root is the repository root (the parent of this file's directory),
and the default port (8931) is what the probes in ``debug/probes`` expect.
"""

from __future__ import annotations

import argparse
import functools
import mimetypes
import os
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("text/javascript", ".js")
mimetypes.add_type("text/html", ".html")

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class COOPCOEPHandler(SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler with the cross-origin-isolation headers."""

    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        if self.server.verbose:  # type: ignore[attr-defined]
            super().log_message(fmt, *args)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8931, help="port to listen on (default 8931)")
    parser.add_argument("--bind", default="127.0.0.1", help="address to bind (default 127.0.0.1)")
    parser.add_argument("--root", default=REPO_ROOT, help="directory to serve (default: repo root)")
    parser.add_argument("--verbose", action="store_true", help="log every request")
    args = parser.parse_args(argv)

    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        parser.error(f"--root is not a directory: {root}")

    handler = functools.partial(COOPCOEPHandler, directory=root)
    # Threaded: the wasm pthread workers fetch many files at once and a
    # single-threaded server can stall the page.
    httpd = ThreadingHTTPServer((args.bind, args.port), handler)
    httpd.verbose = args.verbose  # type: ignore[attr-defined]

    print(f"serving {root}")
    print(f"  http://{args.bind}:{args.port}/app/web/index.html")
    print("  COOP/COEP + Cache-Control: no-store; Ctrl-C to stop")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
