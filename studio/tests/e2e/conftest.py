"""Playwright smoke-test harness.

Launches the Studio CLI in a subprocess against the session-scoped `test_dsn`
provided by `studio/tests/conftest.py`, polls until the HTTP server is up, and
yields the base URL. Smoke tests drive the live UI through Playwright."""
from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

import pytest


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_until_up(url: str, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    last_err: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=1) as r:
                if r.status == 200:
                    return
        except (urllib.error.URLError, ConnectionError, OSError) as e:
            last_err = e
        time.sleep(0.2)
    raise RuntimeError(
        f"Studio did not come up at {url} within {timeout}s "
        f"(last error: {last_err})"
    )


@pytest.fixture(scope="session")
def studio_url(test_dsn: str) -> str:
    port = _free_port()
    base_url = f"http://127.0.0.1:{port}"
    env = os.environ.copy()
    # Don't let the parent shell override --dsn.
    env.pop("DATABASE_URL", None)
    # Isolate the persisted Studio config so a developer's previously-saved
    # search_path doesn't override our --search-path here (app.py loads
    # persisted config eagerly and lets it win over CLI args).
    with tempfile.TemporaryDirectory(prefix="provsql-studio-e2e-") as cfg_dir:
        env["PROVSQL_STUDIO_CONFIG_DIR"] = cfg_dir
        # pytest-playwright tears the whole browser context down between
        # tests, which never fires pagehide, so the notebook's
        # kernel-closing beacon is lost and one kernel leaks per
        # notebook test (a Playwright artifact: real tab closes deliver
        # the beacon). Raise the cap so the leak cannot starve later
        # tests; the server's idle GC remains the real-world backstop.
        env["PROVSQL_STUDIO_MAX_KERNELS"] = "64"
        cmd = [
            sys.executable, "-m", "provsql_studio",
            "--host", "127.0.0.1",
            "--port", str(port),
            "--dsn", test_dsn,
            # The fixture creates `personnel` in the `provsql_test` schema
            # (see `test/sql/add_provenance.sql`). Studio appends `provsql`.
            "--search-path", "provsql_test",
            "--ignore-version",
        ]
        # Server output goes to a file, NOT a PIPE: nothing ever read
        # those pipes, so once werkzeug's per-request log lines filled
        # the 64 KB buffer, every server thread blocked on its next
        # write and the whole app stalled mid-suite (page.goto timeout
        # in whichever test crossed the threshold).
        log_path = os.path.join(cfg_dir, "studio-server.log")
        with open(log_path, "wb") as log_f:
            proc = subprocess.Popen(
                cmd, env=env,
                stdout=log_f, stderr=subprocess.STDOUT,
            )
            try:
                _wait_until_up(base_url + "/")
                yield base_url
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


@pytest.fixture(scope="session")
def browser_context_args(browser_context_args):
    # Pin a deterministic viewport so layout-sensitive selectors stay stable
    # across hosts; matches the desktop breakpoint the UI is designed for.
    return {**browser_context_args, "viewport": {"width": 1400, "height": 900}}


@pytest.fixture
def context(context):
    # CI runners share two cores between PostgreSQL, the Flask dev server,
    # and Chromium, so a navigation that pays Studio's schema introspection
    # can legitimately exceed Playwright's 30 s default -- observed as
    # sporadic one-off Page.goto timeouts in otherwise green matrix cells.
    # 90 s turns that flake into slack without masking real hangs (the
    # job-level timeout still bounds the damage). Set on the context so
    # every page derived from it inherits the value.
    context.set_default_navigation_timeout(90_000)
    return context
