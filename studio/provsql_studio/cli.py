"""Command-line entry point: parse args, build the Flask app, run the server."""
from __future__ import annotations

import argparse
import logging
import re
import sys

from . import __version__


# Endpoints the front-end polls on a timer and that would otherwise drown
# out genuine activity in the access log. The connection-status dot polls
# /api/conn every 5s; matched against the request line werkzeug logs so
# both 200s and 503s (server down) are dropped.
_QUIET_PATHS = (
    re.compile(r'"GET /api/conn HTTP/[0-9.]+"'),
)


class _QuietAccessLogFilter(logging.Filter):
    """Drop werkzeug access-log records for the polled endpoints listed in
    _QUIET_PATHS. Any other log record (including app errors) flows through
    untouched."""

    def filter(self, record: logging.LogRecord) -> bool:
        msg = record.getMessage()
        return not any(p.search(msg) for p in _QUIET_PATHS)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="provsql-studio",
        description="Web UI for the ProvSQL PostgreSQL extension.",
    )
    p.add_argument(
        "--version",
        action="version",
        version=f"provsql-studio {__version__}",
    )
    p.add_argument(
        "--dsn",
        default=None,
        help="PostgreSQL DSN (e.g. 'postgresql://user@host/db'). "
             "If omitted, psycopg falls back to PG* env vars.",
    )
    p.add_argument("--host", default="127.0.0.1", help="Bind host (default 127.0.0.1).")
    p.add_argument("--port", type=int, default=8000, help="Bind port (default 8000).")
    p.add_argument(
        "--statement-timeout",
        default="30s",
        help="PostgreSQL statement_timeout applied per request (default 30s).",
    )
    p.add_argument(
        "--max-circuit-depth",
        type=int,
        default=8,
        help="BFS depth cap for /api/circuit (default 8).",
    )
    p.add_argument(
        "--max-circuit-nodes",
        type=int,
        default=200,
        help="Node-count cap for /api/circuit responses (default 200). "
             "When a render exceeds the cap the front-end surfaces a "
             "structured banner with a 'Render at depth N-1' button and "
             "a hint to drill into a specific node, so the cap should be "
             "low enough that wide circuits hit the actionable path "
             "rather than silently rendering an unreadable hairball.",
    )
    p.add_argument(
        "--max-sidebar-rows",
        type=int,
        default=100,
        help="Per-relation row cap in the where-mode sidebar (default 100). "
             "Without a cap, the sidebar tries to render every row of every "
             "tagged relation on page load and freezes the browser on real "
             "datasets.",
    )
    p.add_argument(
        "--max-result-rows",
        type=int,
        default=1000,
        help="Row cap for /api/exec result rendering (default 1000). The "
             "server fetches up to N+1 rows and trims to N; the front-end "
             "surfaces a 'showing first N rows' footer when truncated.",
    )
    p.add_argument(
        "--search-path",
        default="",
        help="Comma-separated search_path applied per request (provsql is "
             "appended automatically). Empty = inherit the database default.",
    )
    p.add_argument(
        "--tool-search-path",
        default="",
        help="Colon-separated list of directories prepended to PATH when "
             "ProvSQL spawns external tools (d4, c2d, minic2d, dsharp, "
             "weightmc, graph-easy). Sets provsql.tool_search_path.",
    )
    p.add_argument("--debug", action="store_true", help="Enable Flask debug mode.")
    p.add_argument(
        "--ignore-version",
        action="store_true",
        help="Skip the ProvSQL extension version check on startup. "
             "Use when running against a development branch that pre-dates "
             "the minimum required version.",
    )
    return p


# Minimum ProvSQL extension version Studio is built against. A `-dev`
# suffix on the installed version is accepted as the matching release
# (e.g. `1.4.0-dev` is treated as `1.4.0`), so Studio can ride alongside
# an unreleased extension build.
REQUIRED_PROVSQL_VERSION = (1, 4, 0)

_VERSION_RE = re.compile(r'^\s*(\d+)\.(\d+)(?:\.(\d+))?')


def _parse_extversion(s: str) -> tuple[int, int, int] | None:
    m = _VERSION_RE.match(s)
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0))


def _check_extension_version(dsn: str) -> None:
    """Read provsql's extversion from the target DB; exit if too old.

    The check is best-effort: if the connection fails or the extension
    isn't listed, the function logs and returns rather than blocking
    startup, so an unreachable database surfaces through the normal
    pool-creation error path instead of through a confusing version
    message. Mismatch on a successful query exits the process."""
    import psycopg
    try:
        with psycopg.connect(dsn) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT extversion FROM pg_extension WHERE extname = 'provsql'"
                )
                row = cur.fetchone()
    except Exception as e:
        print(
            f"[provsql-studio] Skipping extension version check: "
            f"could not query target database ({e}).",
            file=sys.stderr,
        )
        return
    if row is None:
        req = '.'.join(str(x) for x in REQUIRED_PROVSQL_VERSION)
        print(
            "[provsql-studio] ProvSQL extension is not installed in the "
            "target database. Run `CREATE EXTENSION provsql CASCADE;` "
            f"first (Studio requires >= {req}).",
            file=sys.stderr,
        )
        sys.exit(1)
    actual = row[0]
    parsed = _parse_extversion(actual)
    if parsed is None:
        print(
            f"[provsql-studio] Could not parse extension version "
            f"'{actual}'; skipping version check.",
            file=sys.stderr,
        )
        return
    if parsed < REQUIRED_PROVSQL_VERSION:
        req = '.'.join(str(x) for x in REQUIRED_PROVSQL_VERSION)
        print(
            f"[provsql-studio] ProvSQL extension version {actual} is "
            f"too old; Studio requires >= {req}. Upgrade the extension "
            f"or pass --ignore-version to override.",
            file=sys.stderr,
        )
        sys.exit(1)


def main(argv: list[str] | None = None) -> int:
    import os
    args = build_parser().parse_args(argv)

    # Resolution order for the connection target:
    #   1. --dsn (CLI flag)
    #   2. DATABASE_URL, used as a DSN (libpq itself does not read
    #      this var, so we forward it explicitly)
    #   3. libpq's native PG* vars (PGDATABASE / PGSERVICE / ...): no
    #      DSN passed to psycopg, libpq picks them up directly.
    #   4. Fallback to the `postgres` maintenance DB so the in-page
    #      switcher can list candidates. The banner makes that next
    #      step visible before they get the page.
    dsn = args.dsn
    db_is_auto = False
    if not dsn:
        dsn = os.environ.get("DATABASE_URL") or None
    if not dsn and not any(os.environ.get(v) for v in (
        "PGDATABASE", "PGSERVICE"
    )):
        dsn = "dbname=postgres"
        db_is_auto = True
        print(
            "[provsql-studio] No --dsn and no PG* env hint: connecting to "
            "the 'postgres' maintenance DB. The page shows a 'Pick a "
            "database' button in the banner at the top.",
            file=sys.stderr,
        )

    # Verify the extension is recent enough before launching the
    # server. Skip on the no-DSN fallback (the maintenance `postgres`
    # DB rarely carries the extension; the user picks a real database
    # via the in-page switcher) and when --ignore-version is set.
    if not args.ignore_version and not db_is_auto:
        _check_extension_version(dsn)

    from .app import create_app  # local import keeps --help fast

    app = create_app(
        dsn=dsn,
        statement_timeout=args.statement_timeout,
        max_circuit_depth=args.max_circuit_depth,
        max_circuit_nodes=args.max_circuit_nodes,
        max_sidebar_rows=args.max_sidebar_rows,
        max_result_rows=args.max_result_rows,
        search_path=args.search_path,
        tool_search_path=args.tool_search_path,
        db_is_auto=db_is_auto,
    )
    # Quiet the high-frequency poll endpoints in the access log. Attached
    # to the werkzeug logger so it survives Flask's debug reloader.
    logging.getLogger("werkzeug").addFilter(_QuietAccessLogFilter())
    # threaded=True so POST /api/cancel/<id> can run while a long
    # POST /api/exec is still blocking on its pool connection.
    app.run(host=args.host, port=args.port, debug=args.debug, threaded=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
