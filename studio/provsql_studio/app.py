"""Flask app factory + routes for ProvSQL Studio.

Routes:
  GET /, /where, /circuit  – serve the shared shell with a body class.
  GET  /api/conn           – current_user / current_database / host.
  POST /api/conn           – swap the active database (rebuilds the pool).
  GET  /api/databases      – list databases the current user can connect to.
  GET  /api/relations      – list provenance-tagged relations + content.
  GET  /api/schema         – list all SELECT-able tables/views with their columns.
  POST /api/exec           – run a SQL batch; only the last statement's result is shown.
  POST /api/cancel/<id>    – pg_cancel_backend the batch in flight under that request id.
  GET  /api/circuit/<uuid> – BFS subgraph + dot-layout for a circuit root.
  POST /api/circuit/<uuid>/expand – fetch a sub-DAG rooted at a frontier node.
  GET  /api/leaf/<uuid>    – resolve an input gate back to its source row.
  GET  /api/config         – read the four whitelisted GUCs.
  POST /api/config         – write one whitelisted GUC.
"""
from __future__ import annotations

import re
import threading
import uuid as uuid_mod
from pathlib import Path

import psycopg
import psycopg.conninfo
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
    search_path: str = "",
) -> Flask:
    app = Flask(__name__, static_folder=None)  # we serve /static/ ourselves
    app.config.update(
        STATEMENT_TIMEOUT=statement_timeout,
        MAX_CIRCUIT_DEPTH=max_circuit_depth,
        MAX_CIRCUIT_NODES=max_circuit_nodes,
        SEARCH_PATH=search_path,
    )

    app.config["DSN"] = dsn or ""
    # Runtime overrides for the panel-managed GUCs (provsql.active and
    # provsql.verbose_level). Applied as SET LOCAL on every batch so changes
    # survive across pool checkouts. Toggle-managed GUCs go in their own
    # request fields, not here.
    #
    # Loaded from ~/.config/provsql-studio/config.json so a Studio restart
    # doesn't drop the user's chosen kill-switch / verbosity settings.
    app.config["RUNTIME_GUCS"] = dict(db.load_persisted_gucs())
    # Studio-level option overrides (Config-panel managed): persisted-on-disk
    # values for max_circuit_depth and statement_timeout. They override the
    # CLI defaults set above so the panel-set values survive restarts.
    persisted_opts = db.load_persisted_options()
    if "max_circuit_depth" in persisted_opts:
        app.config["MAX_CIRCUIT_DEPTH"] = persisted_opts["max_circuit_depth"]
    if "statement_timeout_seconds" in persisted_opts:
        app.config["STATEMENT_TIMEOUT"] = f"{persisted_opts['statement_timeout_seconds']}s"
    if "search_path" in persisted_opts:
        app.config["SEARCH_PATH"] = persisted_opts["search_path"]
    app.extensions["provsql_pool"] = db.make_pool(dsn)
    # Registry of in-flight POST /api/exec batches, keyed by the
    # client-generated request id. Lets POST /api/cancel/<id> resolve a
    # request id to a backend pid and fire pg_cancel_backend on a
    # separate connection while the original /api/exec is still
    # blocked. Threaded Flask (cli.py) is required for that to work.
    app.extensions["provsql_inflight"] = {
        "lock": threading.Lock(),
        "by_id": {},  # request_id -> pg_backend_pid
    }
    layout_cache = circuit_mod.LayoutCache()

    # Routes read the live pool through this getter so swapping the pool
    # (when the user switches database) is picked up without re-binding
    # closures.
    def get_pool():
        return app.extensions["provsql_pool"]

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

    @app.get("/api/conn")
    def api_conn():
        # Catch the OperationalError that the pool raises when PG is down
        # (server stopped, network blip, auth revoked) so /api/conn
        # responds with a structured 503 + human-readable reason instead
        # of a bare Flask 500. The connectivity-poll on the front-end
        # surfaces this string in the dot's tooltip.
        try:
            info = db.conn_info(get_pool())
        except psycopg.OperationalError as e:
            return jsonify({
                "error": "database unreachable",
                "reason": str(e).strip() or "cannot connect to PostgreSQL",
            }), 503
        # Display the path that user queries effectively see: the Studio
        # override (Config panel) when set, else the session value, with
        # provsql always pinned at the end. The front-end renders this
        # as `<path> [lock]` to indicate provsql is enforced.
        info["search_path"] = db.compose_search_path(
            app.config.get("SEARCH_PATH", ""),
            info["search_path"],
        )
        return jsonify(info)

    @app.post("/api/conn")
    def api_conn_switch():
        payload = request.get_json(silent=True) or {}
        target = payload.get("database")
        if not target or not isinstance(target, str):
            return jsonify({"error": "missing 'database'"}), 400
        # Verify the user can actually connect, before tearing down the pool.
        accessible = db.list_databases(get_pool())
        if target not in accessible:
            return jsonify({"error": f"database {target!r} not accessible"}), 403
        # Compose a new DSN by swapping dbname, preserving other connection
        # parameters (host, port, options like search_path, ...).
        params = psycopg.conninfo.conninfo_to_dict(app.config["DSN"])
        params["dbname"] = target
        new_dsn = psycopg.conninfo.make_conninfo(**params)
        new_pool = db.make_pool(new_dsn)
        old_pool = app.extensions["provsql_pool"]
        app.extensions["provsql_pool"] = new_pool
        app.config["DSN"] = new_dsn
        layout_cache._store.clear()
        try:
            old_pool.close()
        except Exception:
            pass
        return jsonify(db.conn_info(new_pool))

    @app.get("/api/databases")
    def api_databases():
        return jsonify(db.list_databases(get_pool()))

    @app.get("/api/relations")
    def api_relations():
        return jsonify(db.list_relations(get_pool()))

    @app.get("/api/schema")
    def api_schema():
        return jsonify(db.list_schema(get_pool()))

    @app.post("/api/exec")
    def api_exec():
        payload = request.get_json(silent=True) or {}
        sql_text = payload.get("sql", "")
        mode = payload.get("mode", "where")
        request_id = str(payload.get("request_id") or "").strip()

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

        inflight = app.extensions["provsql_inflight"]
        registered = False

        def register_pid(pid: int) -> None:
            nonlocal registered
            if not request_id:
                return
            with inflight["lock"]:
                inflight["by_id"][request_id] = pid
                registered = True

        try:
            intermediate, final, meta = db.exec_batch(
                get_pool(),
                statements,
                statement_timeout=app.config["STATEMENT_TIMEOUT"],
                where_provenance=where_prov,
                update_provenance=update_prov,
                wrap_last=wrap_last,
                extra_gucs=app.config["RUNTIME_GUCS"],
                on_pid=register_pid,
                search_path=app.config.get("SEARCH_PATH", ""),
            )
        finally:
            if registered:
                with inflight["lock"]:
                    inflight["by_id"].pop(request_id, None)

        blocks = [r.to_dict() for r in intermediate]
        if final is not None:
            blocks.append(final.to_dict())
        return jsonify({
            "blocks": blocks,
            "wrapped": meta["wrapped"],
            "notices": meta.get("notices", []),
        })

    @app.post("/api/cancel/<request_id>")
    def api_cancel(request_id: str):
        # Fires pg_cancel_backend(pid) on a *fresh* connection (not from
        # the pool) so we never wait for a slot that may itself be held
        # by the very query we're trying to cancel. The cancel arrives
        # at the running backend as a SIGINT, which the patched
        # provsql_sigint_handler turns into the standard
        # InterruptPending / QueryCancelPending pair, and PG ereports
        # 57014 that exec_batch then surfaces as a normal error block.
        inflight = app.extensions["provsql_inflight"]
        with inflight["lock"]:
            pid = inflight["by_id"].get(request_id)
        if pid is None:
            return jsonify({
                "ok": False,
                "reason": "no in-flight query for this id",
            }), 404
        try:
            with psycopg.connect(app.config["DSN"]) as conn:
                with conn.cursor() as cur:
                    cur.execute("SELECT pg_cancel_backend(%s)", (pid,))
                    ok = bool(cur.fetchone()[0])
            return jsonify({"ok": ok})
        except psycopg.Error as e:
            return jsonify({
                "ok": False,
                "reason": str(e).strip(),
            }), 500

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
        import psycopg
        cached = layout_cache.get(root, depth)
        if cached is not None:
            return jsonify(cached)
        try:
            data = circuit_mod.get_circuit(
                get_pool(), root=root, depth=depth, max_nodes=app.config["MAX_CIRCUIT_NODES"]
            )
        except circuit_mod.CircuitTooLarge as e:
            return jsonify({
                "error": "circuit too large",
                "node_count": e.node_count,
                "cap": e.cap,
                "hint": "reduce depth or expand interactively",
            }), 413
        except psycopg.errors.UndefinedFunction:
            # The current database carries an older provsql that predates
            # circuit_subgraph / resolve_input. Tell the user instead of
            # leaking the raw "function ... does not exist" stack trace.
            return jsonify({
                "error": "circuit introspection unavailable on this database",
                "hint": (
                    "The connected database has an older provsql installation "
                    "without provsql.circuit_subgraph. Upgrade the extension "
                    "(ALTER EXTENSION provsql UPDATE) or switch to a database "
                    "that has the current version."
                ),
            }), 501
        layout_cache.put(root, depth, data)
        return jsonify(data)

    @app.get("/api/leaf/<token>")
    def api_leaf(token: str):
        import psycopg
        try:
            uuid_str = _coerce_to_uuid(token)
        except ValueError:
            return jsonify({"error": "not a valid UUID"}), 400
        try:
            rows = circuit_mod.resolve_input(get_pool(), uuid_str)
        except psycopg.errors.UndefinedFunction:
            return jsonify({
                "error": "leaf resolution unavailable on this database",
                "hint": (
                    "The connected database has an older provsql installation "
                    "without provsql.resolve_input. Upgrade the extension or "
                    "switch to a database that has the current version."
                ),
            }), 501
        if not rows:
            return jsonify({"error": "no row maps to this input gate"}), 404
        # Single-relation case is the norm; if multiple tables share the UUID,
        # return the list and let the front-end pick.
        return jsonify({"matches": rows})

    _OPTION_KEYS = {"max_circuit_depth", "statement_timeout_seconds", "search_path"}

    def _current_options() -> dict:
        # Surface the live values of the Studio-level options so the
        # Config panel can display them after a restart, including those
        # picked up from CLI flags rather than the on-disk config file.
        timeout = str(app.config["STATEMENT_TIMEOUT"]).strip().lower()
        # Best-effort parse of the timeout string; the CLI accepts any
        # PG-parseable interval ("30s", "500ms", "1min"), but the panel
        # stores it as plain seconds.
        seconds: int | None = None
        if timeout.endswith("ms"):
            try:
                seconds = max(1, int(timeout[:-2]) // 1000)
            except ValueError:
                pass
        elif timeout.endswith("s") and not timeout.endswith("ms"):
            try:
                seconds = int(timeout[:-1])
            except ValueError:
                pass
        elif timeout.endswith("min"):
            try:
                seconds = int(timeout[:-3]) * 60
            except ValueError:
                pass
        return {
            "max_circuit_depth": int(app.config["MAX_CIRCUIT_DEPTH"]),
            "statement_timeout_seconds": seconds if seconds is not None else 30,
            "search_path": app.config.get("SEARCH_PATH", "") or "",
        }

    @app.get("/api/config")
    def api_config_get():
        # Returns the *effective* values of the panel GUCs after our runtime
        # overrides are applied, plus the bare overrides we hold in app
        # state (so the front-end can show "modified" markers if it wants).
        effective = db.show_panel_gucs(get_pool(), app.config["RUNTIME_GUCS"])
        return jsonify({
            "effective": effective,
            "overrides": dict(app.config["RUNTIME_GUCS"]),
            "options": _current_options(),
        })

    @app.post("/api/config")
    def api_config_set():
        payload = request.get_json(silent=True) or {}
        name = payload.get("key", "")
        value = payload.get("value", "")
        # Studio-level options (not GUCs) are validated and stored in app
        # config, then persisted alongside the GUC overrides.
        if name in _OPTION_KEYS:
            try:
                key, canonical = db.validate_panel_option(name, value)
            except ValueError as e:
                return jsonify({"error": str(e)}), 400
            if key == "max_circuit_depth":
                app.config["MAX_CIRCUIT_DEPTH"] = canonical
            elif key == "statement_timeout_seconds":
                app.config["STATEMENT_TIMEOUT"] = f"{canonical}s"
            elif key == "search_path":
                app.config["SEARCH_PATH"] = canonical
            db.save_persisted_options(_current_options())
            return jsonify({"ok": True, "key": key, "value": canonical})
        # Otherwise treat as a GUC override.
        try:
            canonical = db.validate_panel_guc(name, value)
        except ValueError as e:
            return jsonify({"error": str(e)}), 400
        app.config["RUNTIME_GUCS"][name] = canonical
        # Best-effort persist so a Studio restart keeps the user's choice.
        db.save_persisted_gucs(app.config["RUNTIME_GUCS"])
        return jsonify({"ok": True, "key": name, "value": canonical})

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
