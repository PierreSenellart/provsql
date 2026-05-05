"""Flask app factory + routes for ProvSQL Studio.

Routes:
  GET /, /where, /circuit  – serve the shared shell with a body class.
  GET  /api/relations      – list provenance-tagged relations + content.
  POST /api/exec           – run a SQL batch; only the last statement's result is shown.
  GET  /api/config         – read the four whitelisted GUCs.
  POST /api/config         – write one whitelisted GUC.
"""
from __future__ import annotations

import re
from pathlib import Path

import sqlparse
from flask import Flask, jsonify, redirect, request, send_from_directory

from . import db


_STATIC_DIR = Path(__file__).resolve().parent / "static"

# Statements that are wrappable for where-mode (i.e. their last statement is
# a SELECT we can plug into `SELECT *, provenance(), where_provenance(...) FROM (<last>) t`).
_WRAPPABLE_RE = re.compile(r"^\s*(WITH|SELECT)\b", re.IGNORECASE)


def create_app(
    *,
    dsn: str | None = None,
    statement_timeout: str = "30s",
    max_circuit_depth: int = 8,
    max_circuit_nodes: int = 500,
) -> Flask:
    app = Flask(__name__, static_folder=None)  # we serve /static/ ourselves
    app.config.update(
        STATEMENT_TIMEOUT=statement_timeout,
        MAX_CIRCUIT_DEPTH=max_circuit_depth,
        MAX_CIRCUIT_NODES=max_circuit_nodes,
    )

    pool = db.make_pool(dsn)
    app.extensions["provsql_pool"] = pool

    # ──────── static + shell routes ────────

    @app.get("/")
    def root():
        return redirect("/where", code=302)

    @app.get("/where")
    def where_shell():
        return _serve_shell("where")

    @app.get("/circuit")
    def circuit_shell():
        return _serve_shell("circuit")

    @app.get("/static/<path:filename>")
    def static_file(filename: str):
        return send_from_directory(_STATIC_DIR, filename)

    def _serve_shell(mode: str):
        html = (_STATIC_DIR / "index.html").read_text()
        # Replace the body class so app.js sees the correct mode and adjust
        # asset URLs so /static/ resolves under Flask's mount.
        html = html.replace('<body class="mode-where">', f'<body class="mode-{mode}">')
        for asset in (
            "fonts-face.css",
            "colors_and_type.css",
            "app.css",
            "app.js",
            "img/favicon.ico",
            "img/logo.png",
        ):
            html = html.replace(f'href="{asset}"', f'href="/static/{asset}"')
            html = html.replace(f'src="{asset}"',  f'src="/static/{asset}"')
        return html, 200, {"Content-Type": "text/html; charset=utf-8"}

    # ──────── API routes ────────

    @app.get("/api/relations")
    def api_relations():
        return jsonify(db.list_relations(pool))

    @app.post("/api/exec")
    def api_exec():
        payload = request.get_json(silent=True) or {}
        sql_text = payload.get("sql", "")
        mode = payload.get("mode", "where")

        statements = _split_statements(sql_text)
        if not statements:
            return jsonify({"blocks": []})

        last = statements[-1]
        wrap_last = mode == "where" and bool(_WRAPPABLE_RE.match(last))

        intermediate, final = db.exec_batch(
            pool,
            statements,
            statement_timeout=app.config["STATEMENT_TIMEOUT"],
            where_provenance=(mode == "where"),
            wrap_last=wrap_last,
        )

        blocks = [r.to_dict() for r in intermediate]
        if final is not None:
            blocks.append(final.to_dict())
        return jsonify({"blocks": blocks, "wrapped": wrap_last})

    @app.get("/api/config")
    def api_config_get():
        return jsonify(db.get_gucs(pool, sorted(db._GUC_WHITELIST)))

    @app.post("/api/config")
    def api_config_set():
        payload = request.get_json(silent=True) or {}
        name = payload.get("key", "")
        value = payload.get("value", "")
        try:
            db.set_guc(pool, name, value)
        except ValueError as e:
            return jsonify({"error": str(e)}), 400
        return jsonify({"ok": True})

    return app


def _split_statements(sql_text: str) -> list[str]:
    """Split a SQL batch into individual statements. sqlparse handles
    dollar-quoting, comments, and string literals correctly."""
    out: list[str] = []
    for raw in sqlparse.split(sql_text):
        stripped = raw.strip().rstrip(";").strip()
        if stripped:
            out.append(stripped)
    return out
