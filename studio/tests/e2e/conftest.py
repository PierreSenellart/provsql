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
        proc = subprocess.Popen(
            cmd, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
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
