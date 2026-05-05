"""Command-line entry point: parse args, build the Flask app, run the server."""
from __future__ import annotations

import argparse
import logging
import re
import sys


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
        default=500,
        help="Node-count cap for /api/circuit responses (default 500).",
    )
    p.add_argument(
        "--search-path",
        default="",
        help="Comma-separated search_path applied per request (provsql is "
             "appended automatically). Empty = inherit the database default.",
    )
    p.add_argument("--debug", action="store_true", help="Enable Flask debug mode.")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    from .app import create_app  # local import keeps --help fast

    app = create_app(
        dsn=args.dsn,
        statement_timeout=args.statement_timeout,
        max_circuit_depth=args.max_circuit_depth,
        max_circuit_nodes=args.max_circuit_nodes,
        search_path=args.search_path,
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
