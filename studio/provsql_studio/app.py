"""Flask app factory + routes for ProvSQL Studio.

Routes:
  GET /, /where, /circuit  – serve the shared shell with a body class.
  GET  /api/relations      – list provenance-tagged relations + content.
  POST /api/exec           – run a SQL batch; only the last statement's result is shown.
  GET  /api/circuit/<uuid> – BFS subgraph + dot-layout for a circuit root.
  POST /api/circuit/<uuid>/expand – fetch a sub-DAG rooted at a frontier node.
  GET  /api/leaf/<uuid>    – resolve an input gate back to its source row.
  GET  /api/config         – read the four whitelisted GUCs.
  POST /api/config         – write one whitelisted GUC.
"""
from __future__ import annotations

import re
import uuid as uuid_mod
from pathlib import Path

import sqlparse
from flask import Flask, jsonify, redirect, request, send_from_directory

from . import circuit as circuit_mod
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
    layout_cache = circuit_mod.LayoutCache()

    # ──────── static + shell routes ────────

    @app.get("/")
    def root():
        return redirect("/circuit", code=302)

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

        # Toggles. In where mode `where_provenance` is forced on because the
        # wrap calls `provsql.where_provenance(provsql.provenance())` and
        # would otherwise return zero matches. In circuit mode both are
        # user-controlled; defaults match the previous fixed behaviour.
        where_prov = bool(payload.get("where_provenance", mode == "where"))
        if mode == "where":
            where_prov = True
        update_prov = bool(payload.get("update_provenance", False))

        intermediate, final = db.exec_batch(
            pool,
            statements,
            statement_timeout=app.config["STATEMENT_TIMEOUT"],
            where_provenance=where_prov,
            update_provenance=update_prov,
            wrap_last=wrap_last,
        )

        blocks = [r.to_dict() for r in intermediate]
        if final is not None:
            blocks.append(final.to_dict())
        return jsonify({"blocks": blocks, "wrapped": wrap_last})

    @app.get("/api/circuit/<token>")
    def api_circuit(token: str):
        try:
            root_uuid = _coerce_to_uuid(token)
        except ValueError:
            return jsonify({"error": "not a valid UUID or agg_token"}), 400
        depth = _clamp_depth(request.args.get("depth"), app.config["MAX_CIRCUIT_DEPTH"])
        return _layout_response(root_uuid, depth)

    @app.post("/api/circuit/<token>/expand")
    def api_circuit_expand(token: str):
        # Path token is the original root (kept for client-side correlation),
        # body carries the frontier we actually re-root the next BFS at.
        del token  # unused on the server; the frontier in the body is canonical
        payload = request.get_json(silent=True) or {}
        try:
            frontier = _coerce_to_uuid(payload.get("frontier_node_uuid", ""))
        except ValueError:
            return jsonify({"error": "frontier_node_uuid is not a valid UUID"}), 400
        depth = _clamp_depth(payload.get("additional_depth"), app.config["MAX_CIRCUIT_DEPTH"])
        return _layout_response(frontier, depth)

    def _layout_response(root: str, depth: int):
        cached = layout_cache.get(root, depth)
        if cached is not None:
            return jsonify(cached)
        try:
            data = circuit_mod.get_circuit(
                pool, root=root, depth=depth, max_nodes=app.config["MAX_CIRCUIT_NODES"]
            )
        except circuit_mod.CircuitTooLarge as e:
            return jsonify({
                "error": "circuit too large",
                "node_count": e.node_count,
                "cap": e.cap,
                "hint": "reduce depth or expand interactively",
            }), 413
        layout_cache.put(root, depth, data)
        return jsonify(data)

    @app.get("/api/leaf/<token>")
    def api_leaf(token: str):
        try:
            uuid_str = _coerce_to_uuid(token)
        except ValueError:
            return jsonify({"error": "not a valid UUID"}), 400
        rows = circuit_mod.resolve_input(pool, uuid_str)
        if not rows:
            return jsonify({"error": "no row maps to this input gate"}), 404
        # Single-relation case is the norm; if multiple tables share the UUID,
        # return the list and let the front-end pick.
        return jsonify({"matches": rows})

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


def _coerce_to_uuid(token: str) -> str:
    """Accept a UUID string (any case, with or without hyphens) and return its
    canonical 36-char form. Raises ValueError otherwise. Front-end agg_token
    cells should send the underlying UUID, not the formatted '<value> (*)'
    text; agg_token's text representation does not carry the UUID."""
    if not token:
        raise ValueError("empty token")
    return str(uuid_mod.UUID(token))


def _clamp_depth(raw, default_max: int) -> int:
    try:
        d = int(raw) if raw is not None and str(raw) != "" else default_max
    except (TypeError, ValueError):
        d = default_max
    return max(1, min(d, default_max))


def _split_statements(sql_text: str) -> list[str]:
    """Split a SQL batch into individual statements. sqlparse handles
    dollar-quoting, comments, and string literals correctly."""
    out: list[str] = []
    for raw in sqlparse.split(sql_text):
        stripped = raw.strip().rstrip(";").strip()
        if stripped:
            out.append(stripped)
    return out
