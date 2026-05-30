"""Playwright fixtures for the browser (PGlite) build of ProvSQL Studio.

Unlike studio/tests/e2e/ (which drives the server Studio against a live
PostgreSQL), these tests serve the assembled static doc-root (studio/web/) and
let the whole backend run in the browser via Pyodide + PGlite. No PostgreSQL.

The doc-root must be built first (studio/web/build.sh); the fixtures skip with
a clear message otherwise. Booting the page downloads Pyodide + wheels from a
CDN and seeds a database, so it is slow -- hence the generous boot timeout and
the module-scoped shared page for read-only tests.
"""
from __future__ import annotations

import functools
import http.server
import os
import socket
import socketserver
import threading
from pathlib import Path

import pytest

# Ubuntu 26.04 isn't an official Playwright platform; match the install
# override (see CLAUDE.local.md) so launch finds the 24.04 build.
os.environ.setdefault("PLAYWRIGHT_HOST_PLATFORM_OVERRIDE", "ubuntu24.04-x64")

WEB_ROOT = Path(__file__).resolve().parents[2] / "web"  # studio/web
BOOT_TIMEOUT = 240_000  # ms; first load fetches Pyodide + flask + seeds a DB

# The only server behaviour the static host needs (mirrors studio/web/serve.py
# and the two Apache Redirect lines): serve files + redirect the clean mode
# paths onto ?mode=.
_REDIRECTS = {"/circuit": "/?mode=circuit", "/where": "/?mode=where"}


class _Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm", ".js": "text/javascript",
        ".mjs": "text/javascript", ".tar.gz": "application/gzip",
        ".data": "application/octet-stream",
    }

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        target = _REDIRECTS.get(self.path.split("?", 1)[0])
        if target is not None:
            self.send_response(302)
            self.send_header("Location", target)
            self.end_headers()
            return
        return super().do_GET()

    def log_message(self, *args):  # quiet
        pass


class _Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="session")
def web_server() -> str:
    if not (WEB_ROOT / "index.html").exists() or not (WEB_ROOT / "pglite").is_dir() \
            or not (WEB_ROOT / "provsql.tar.gz").exists():
        pytest.skip(
            "browser build not assembled; run studio/web/build.sh "
            "--pglite <dist> --provsql <provsql.tar.gz> first")
    port = _free_port()
    httpd = _Server(("127.0.0.1", port), functools.partial(_Handler, directory=str(WEB_ROOT)))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{port}"
    httpd.shutdown()


def _boot(page, url: str) -> None:
    """Wait until studio-boot.js finishes (it hides the status bar) or fails
    (it turns the bar red and leaves it visible)."""
    page.goto(url + "/", wait_until="domcontentloaded")
    try:
        page.wait_for_selector("#studio-boot-status", state="hidden", timeout=BOOT_TIMEOUT)
    except Exception:
        bar = page.locator("#studio-boot-status")
        if bar.count() and bar.is_visible():
            raise AssertionError("boot did not finish: " + (bar.inner_text() or "")[:300])
        raise


_CLIPBOARD = ["clipboard-read", "clipboard-write"]


@pytest.fixture()
def open_studio(browser, web_server):
    """Factory: open a freshly-booted Studio page (isolated storage). Pass a
    database name to land on it directly (sets ps.activeDb before load), or a
    `path` (e.g. a "?mode=&db=&q=" deep link) to open verbatim."""
    contexts = []

    def _open(db: str | None = None, path: str = "/"):
        ctx = browser.new_context(permissions=_CLIPBOARD)
        if db:
            ctx.add_init_script(
                "try{localStorage.setItem('ps.activeDb', %r)}catch(e){}" % db)
        contexts.append(ctx)
        page = ctx.new_page()
        page.goto(web_server + path, wait_until="domcontentloaded")
        page.wait_for_selector("#studio-boot-status", state="hidden", timeout=BOOT_TIMEOUT)
        return page

    yield _open
    for ctx in contexts:
        ctx.close()


@pytest.fixture(scope="module")
def cs7_page(browser, web_server):
    """A page booted once on cs7 (provenance-tracked, with probabilities) for
    the read-only assertions, so they don't each pay a fresh boot."""
    ctx = browser.new_context(permissions=_CLIPBOARD)
    ctx.add_init_script("try{localStorage.setItem('ps.activeDb','cs7')}catch(e){}")
    page = ctx.new_page()
    _boot(page, web_server)
    yield page
    ctx.close()
