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
  GET  /api/kc/cnf         – Tseytin CNF (DIMACS) for a provenance circuit.
  GET  /api/kc/ddnnf       – compiled d-DNNF (DOT + SVG) via an external compiler.
  GET  /api/kc/td          – tree decomposition (DOT + SVG + treewidth).
  GET  /api/kc/benchmark   – time every probability_evaluate method.
  GET  /api/config         – read the four whitelisted GUCs.
  POST /api/config         – write one whitelisted GUC.
"""
from __future__ import annotations

import re
import subprocess
import threading
import uuid as uuid_mod
from pathlib import Path

import psycopg
import psycopg.conninfo
import sqlparse
from flask import Flask, jsonify, redirect, request, send_from_directory

from . import __version__ as STUDIO_VERSION
from . import circuit as circuit_mod
from . import db
from . import kc as kc_mod


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
    max_sidebar_rows: int = 100,
    max_result_rows: int = 1000,
    search_path: str = "",
    tool_search_path: str = "",
    db_is_auto: bool = False,
) -> Flask:
    app = Flask(__name__, static_folder=None)  # we serve /static/ ourselves
    app.config.update(
        STATEMENT_TIMEOUT=statement_timeout,
        MAX_CIRCUIT_DEPTH=max_circuit_depth,
        MAX_CIRCUIT_NODES=max_circuit_nodes,
        MAX_SIDEBAR_ROWS=max_sidebar_rows,
        MAX_RESULT_ROWS=max_result_rows,
        SEARCH_PATH=search_path,
        TOOL_SEARCH_PATH=tool_search_path,
    )

    app.config["DSN"] = dsn or ""
    # True when the CLI couldn't infer a DB from --dsn or PG* env vars
    # and fell back to the postgres maintenance DB. The /api/conn route
    # surfaces this so the UI can prompt the user to pick a real DB
    # via the top-nav switcher. Cleared once the user switches.
    app.config["DB_IS_AUTO"] = bool(db_is_auto)
    # Runtime overrides for the panel-managed GUCs (provsql.active and
    # provsql.verbose_level). Applied as SET LOCAL on every batch so changes
    # survive across pool checkouts. Toggle-managed GUCs go in their own
    # request fields, not here.
    #
    # Loaded from ~/.config/provsql-studio/config.json so a Studio restart
    # doesn't drop the user's chosen kill-switch / verbosity settings.
    app.config["RUNTIME_GUCS"] = dict(db.load_persisted_gucs())
    # Session-sticky mode flags. Currently just the Boolean-provenance
    # selector: when the user picks the Boolean flavour in /api/exec,
    # this dict gets {"provsql.boolean_provenance": "on"} ; subsequent
    # /api/circuit and /api/evaluate calls merge it into their
    # extra_gucs so the load-time foldBooleanIdentities fires there
    # too.  Picking Semiring / Where in /api/exec drops the entry.
    # Distinct from RUNTIME_GUCS (which is the Config-panel overlay
    # the user manages explicitly) so the mode selector stays
    # invisible to the panel API.
    app.config["SESSION_MODES"] = {}
    # Studio-level option overrides (Config-panel managed): persisted-on-disk
    # values for max_circuit_depth and statement_timeout. They override the
    # CLI defaults set above so the panel-set values survive restarts.
    persisted_opts = db.load_persisted_options()
    if "max_circuit_depth" in persisted_opts:
        app.config["MAX_CIRCUIT_DEPTH"] = persisted_opts["max_circuit_depth"]
    if "max_circuit_nodes" in persisted_opts:
        app.config["MAX_CIRCUIT_NODES"] = persisted_opts["max_circuit_nodes"]
    if "max_sidebar_rows" in persisted_opts:
        app.config["MAX_SIDEBAR_ROWS"] = persisted_opts["max_sidebar_rows"]
    if "max_result_rows" in persisted_opts:
        app.config["MAX_RESULT_ROWS"] = persisted_opts["max_result_rows"]
    if "statement_timeout_seconds" in persisted_opts:
        app.config["STATEMENT_TIMEOUT"] = f"{persisted_opts['statement_timeout_seconds']}s"
    if "search_path" in persisted_opts:
        app.config["SEARCH_PATH"] = persisted_opts["search_path"]
    if "tool_search_path" in persisted_opts:
        app.config["TOOL_SEARCH_PATH"] = persisted_opts["tool_search_path"]
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

    def _backend_gucs(extra: dict[str, str] | None = None) -> dict[str, str]:
        """Merge the panel overlay + session-sticky mode flags + any
        per-call extras into a single dict ready to feed db.exec_batch
        / circuit_mod.get_circuit / circuit_mod.evaluate_circuit.

        Order : RUNTIME_GUCS (panel) -> SESSION_MODES (sticky) ->
        per-call extras (highest priority).  The whitelist applied
        downstream still gates which keys actually fire a SET LOCAL ;
        this helper just assembles the dict.
        """
        out = dict(app.config["RUNTIME_GUCS"])
        out.update(app.config["SESSION_MODES"])
        if extra:
            out.update(extra)
        return out

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
        info["db_is_auto"] = app.config.get("DB_IS_AUTO", False)
        info["studio_version"] = STUDIO_VERSION
        # Send back a password-stripped DSN so the connection editor
        # can prefill its input without leaking secrets to the page.
        # The user re-types the password if they need to switch host
        # or role.
        try:
            params = psycopg.conninfo.conninfo_to_dict(app.config.get("DSN", ""))
            params.pop("password", None)
            info["dsn"] = psycopg.conninfo.make_conninfo(**params)
        except Exception:
            info["dsn"] = ""
        return jsonify(info)

    @app.post("/api/conn")
    def api_conn_switch():
        payload = request.get_json(silent=True) or {}
        new_dsn = payload.get("dsn")
        target  = payload.get("database")
        dsn_no_db = False
        if new_dsn and isinstance(new_dsn, str) and new_dsn.strip():
            # Free-form DSN path: the user pasted a full conninfo string
            # (host, port, user, password, options, ...). We open a fresh
            # pool and probe it with SELECT 1 before swapping; if anything
            # is wrong (auth, host unreachable, bad syntax) the old pool
            # stays in service and the error reaches the front-end.
            new_dsn = new_dsn.strip()
            # If the user didn't specify dbname, default to the postgres
            # maintenance DB and re-raise the auto-fallback banner so
            # they can pick a real database from the switcher. Mirrors
            # the CLI launch behaviour.
            try:
                params = psycopg.conninfo.conninfo_to_dict(new_dsn)
            except Exception:
                params = None
            if params is not None and "dbname" not in params:
                params["dbname"] = "postgres"
                new_dsn = psycopg.conninfo.make_conninfo(**params)
                dsn_no_db = True
            try:
                probe_pool = db.make_pool(new_dsn)
                with probe_pool.connection() as c, c.cursor() as cur:
                    cur.execute("SELECT 1")
                    cur.fetchone()
            except Exception as e:
                try:
                    probe_pool.close()
                except Exception:
                    pass
                return jsonify({
                    "error": "cannot connect with the supplied DSN",
                    "reason": str(e).strip() or repr(e),
                }), 400
            new_pool = probe_pool
        elif target and isinstance(target, str):
            # Convenience path: swap dbname only, preserving the rest of
            # the connection parameters. Used by the top-nav switcher.
            accessible = db.list_databases(get_pool())
            if target not in accessible:
                return jsonify({"error": f"database {target!r} not accessible"}), 403
            params = psycopg.conninfo.conninfo_to_dict(app.config["DSN"])
            params["dbname"] = target
            new_dsn = psycopg.conninfo.make_conninfo(**params)
            new_pool = db.make_pool(new_dsn)
        else:
            return jsonify({"error": "missing 'dsn' or 'database'"}), 400

        old_pool = app.extensions["provsql_pool"]
        app.extensions["provsql_pool"] = new_pool
        app.config["DSN"] = new_dsn
        # The "no DB picked" hint reappears whenever we land on the
        # postgres maintenance DB by default (here when the user
        # supplied a DSN without a dbname). For an explicit dbname or a
        # plain database-switch, drop it.
        app.config["DB_IS_AUTO"] = dsn_no_db
        layout_cache._store.clear()
        try:
            old_pool.close()
        except Exception:
            pass
        info = db.conn_info(new_pool)
        info["studio_version"] = STUDIO_VERSION
        return jsonify(info)

    @app.get("/api/databases")
    def api_databases():
        return jsonify(db.list_databases(get_pool()))

    @app.get("/api/relations")
    def api_relations():
        return jsonify(db.list_relations(
            get_pool(),
            max_rows=int(app.config["MAX_SIDEBAR_ROWS"]),
        ))

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

        # The provenance-flavour selector is a three-way choice :
        # `boolean` (provsql.boolean_provenance on, safe-query rewriter
        # enabled), `semiring` (both flavour GUCs off), `where`
        # (provsql.where_provenance on, eligible queries get wrapped
        # with where_provenance(provenance()) for cell-level highlights).
        # Boolean and where are mutually exclusive at the C level
        # (where-provenance gates do not survive the safe-query rewrite),
        # so the front-end's segmented control enforces a single pick.
        # In Where UI mode the cell-highlight wrap requires where, so
        # the selector is locked there client-side; if a stale payload
        # arrives anyway we override.
        prov_scheme = (payload.get("prov_scheme") or "semiring").lower()
        if mode == "where":
            prov_scheme = "where"
        if prov_scheme not in ("boolean", "semiring", "where"):
            prov_scheme = "semiring"
        where_prov = prov_scheme == "where"
        boolean_prov = prov_scheme == "boolean"
        # Session-sticky : update the app-level mode flag so subsequent
        # /api/circuit and /api/evaluate calls (which carry their own
        # backend connection from the pool) also run under
        # boolean_provenance=on when the user is in Boolean mode.
        # Without this the load-time foldBooleanIdentities pass fires
        # only on the original /api/exec batch ; circuit-mode cell
        # clicks would then render the unfolded form.
        prev_bool = app.config["SESSION_MODES"].get("provsql.boolean_provenance")
        if boolean_prov:
            app.config["SESSION_MODES"]["provsql.boolean_provenance"] = "on"
        else:
            app.config["SESSION_MODES"].pop("provsql.boolean_provenance", None)
        new_bool = app.config["SESSION_MODES"].get("provsql.boolean_provenance")
        # The LayoutCache is keyed on (root, depth) only, so a cached
        # scene from a previous mode would shadow the next /api/circuit
        # fetch.  Drop the cache on every mode flip so the next fetch
        # genuinely re-runs the load-time simplifier.
        if prev_bool != new_bool:
            layout_cache.clear()
        # The where-mode result wrap is only applied when the user
        # picked the where flavour AND the last statement is a wrappable
        # SELECT (the wrap calls where_provenance(provenance()) which
        # would otherwise return zero matches).
        wrap_last = where_prov and bool(_WRAPPABLE_RE.match(last))
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
                boolean_provenance=boolean_prov,
                wrap_last=wrap_last,
                extra_gucs=_backend_gucs(),
                on_pid=register_pid,
                search_path=app.config.get("SEARCH_PATH", ""),
                tool_search_path=app.config.get("TOOL_SEARCH_PATH", ""),
                max_result_rows=int(app.config["MAX_RESULT_ROWS"]),
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
                get_pool(), root=root, depth=depth,
                max_nodes=app.config["MAX_CIRCUIT_NODES"],
                extra_gucs=_backend_gucs(),
            )
        except circuit_mod.CircuitTooLarge as e:
            return jsonify({
                "error": "circuit too large",
                "node_count": e.node_count,
                "cap": e.cap,
                "depth": e.depth,
                "depth_1_size": e.depth_1_size,
                "hint": "reduce depth or click into a specific node",
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
        # Best-effort probability: when set_prob has been called on the
        # gate, surface the value alongside the resolved row(s) so the
        # inspector can show the per-row probability without a second
        # round-trip.  None when unset / inapplicable.  Fetch BEFORE the
        # empty-rows short-circuit so anonymous Bernoullis (e.g. those
        # minted by `provsql.mixture(p_value, x, y)`, or by the user
        # via `create_gate(uuid, 'input') + set_prob(uuid, p)` without
        # a tracked source table) still surface their probability even
        # though no row in any tracked relation references the UUID.
        probability = circuit_mod.get_prob(get_pool(), uuid_str)
        if not rows:
            if probability is not None:
                return jsonify({"matches": [], "probability": probability})
            return jsonify({"error": "no row maps to this input gate"}), 404
        body = {"matches": rows}
        if probability is not None:
            body["probability"] = probability
        # Single-relation case is the norm; if multiple tables share the UUID,
        # return the list and let the front-end pick.
        return jsonify(body)

    @app.post("/api/set_prob")
    def api_set_prob():
        """Write a probability for an input/update gate via
        provsql.set_prob. Backs the inspector's click-to-edit affordance:
        the user opens an input gate, clicks the displayed probability,
        types a new value, hits Enter, and we fire this endpoint."""
        import psycopg
        payload = request.get_json(silent=True) or {}
        try:
            uuid_str = _coerce_to_uuid(payload.get("uuid", ""))
        except ValueError:
            return jsonify({"error": "not a valid UUID"}), 400
        raw = payload.get("probability", None)
        try:
            p = float(raw)
        except (TypeError, ValueError):
            return jsonify({"error": "probability must be a number"}), 400
        if not (0.0 <= p <= 1.0):
            return jsonify({"error": "probability must be between 0 and 1"}), 400
        try:
            with get_pool().connection() as conn, conn.cursor() as cur:
                cur.execute(
                    "SELECT provsql.set_prob(%s::uuid, %s::double precision)",
                    (uuid_str, p),
                )
        except psycopg.Error as e:
            diag = getattr(e, "diag", None)
            return jsonify({
                "error": "set_prob failed",
                "detail": (diag.message_primary if diag else str(e)) or str(e),
                "sqlstate": diag.sqlstate if diag else None,
            }), 400
        return jsonify({"ok": True, "probability": p})

    @app.get("/api/provenance_mappings")
    def api_provenance_mappings():
        # Used by the circuit-mode semiring evaluation strip to populate
        # the mapping select; refreshed each time the panel opens, so
        # newly-created mappings show up without a page reload.
        return jsonify(db.list_provenance_mappings(get_pool()))

    @app.get("/api/custom_semirings")
    def api_custom_semirings():
        # Discovered SQL/PL wrappers around `provenance_evaluate`.
        # Populates the eval strip's "Custom Semirings" optgroup; refreshed
        # whenever the panel opens.
        return jsonify(db.list_custom_semirings(get_pool()))

    @app.post("/api/evaluate")
    def api_evaluate():
        import psycopg
        payload = request.get_json(silent=True) or {}
        try:
            token = _coerce_to_uuid(payload.get("token", ""))
        except ValueError:
            return jsonify({"error": "token is not a valid UUID"}), 400
        semiring  = (payload.get("semiring") or "").strip().lower()
        mapping   = payload.get("mapping") or None
        method    = payload.get("method") or None
        arguments = payload.get("arguments") or None
        function  = payload.get("function") or None
        # Conditioning gate UUID for scalar evaluators (distribution-
        # profile, moment, sample).  When the user pins a "Condition on"
        # gate via the strip's UUID picker we forward it as the `prov`
        # argument to rv_moment / rv_support / rv_histogram / rv_sample.
        # Validated as a UUID below; ignored by every other semiring.
        condition_uuid = payload.get("condition_uuid") or None
        if condition_uuid:
            try:
                condition_uuid = _coerce_to_uuid(condition_uuid)
            except ValueError:
                return jsonify({"error": "condition_uuid is not a valid UUID"}), 400
        # Merge per-request GUC overrides over the panel-managed +
        # session-sticky ones.  The payload's extra_gucs lets tests
        # and Studio's evaluate-strip pin per-request behaviour (e.g.
        # seed / sample budget for a distribution-profile invocation)
        # without mutating the session-wide panel state ;
        # evaluate_circuit validates each key via the same whitelist
        # either path uses.
        merged_gucs = _backend_gucs(
            {str(k): str(v) for k, v in (payload.get("extra_gucs") or {}).items()}
            if isinstance(payload.get("extra_gucs"), dict)
            else None
        )
        try:
            data = db.evaluate_circuit(
                get_pool(),
                token=token,
                semiring=semiring,
                mapping=mapping,
                method=method,
                arguments=arguments,
                function=function,
                statement_timeout=app.config["STATEMENT_TIMEOUT"],
                search_path=app.config.get("SEARCH_PATH", ""),
                tool_search_path=app.config.get("TOOL_SEARCH_PATH", ""),
                extra_gucs=merged_gucs,
                condition_uuid=condition_uuid,
            )
        except ValueError as e:
            return jsonify({"error": str(e)}), 400
        except psycopg.errors.UndefinedFunction as e:
            # Older provsql or missing helper : surface the underlying
            # diagnostic so the user can see which function is missing.
            return jsonify({
                "error": "evaluation function unavailable on this database",
                "detail": str(e).splitlines()[0],
            }), 501
        except psycopg.Error as e:
            diag = getattr(e, "diag", None)
            return jsonify({
                "error": "evaluation failed",
                "sqlstate": diag.sqlstate if diag else None,
                "detail": str(e).strip(),
            }), 500
        return jsonify(data)

    # ── Knowledge-compilation demo helpers ────────────────────────────
    # Four read-only inspectors that surface the new SQL helpers added
    # in extension 1.7.0: the Tseytin CNF, the compiled d-DNNF (as DOT
    # + SVG), the tree decomposition (DOT + SVG + treewidth), and a
    # side-by-side timing of every probability_evaluate method.

    _KC_COMPILERS_WHITELIST = {"d4", "c2d", "minic2d", "dsharp"}

    def _kc_token():
        try:
            return _coerce_to_uuid(request.args.get("token", ""))
        except ValueError:
            return None

    def _kc_unavailable(e: Exception):
        return jsonify({
            "error": "knowledge-compilation helpers unavailable on this database",
            "detail": str(e).splitlines()[0],
            "hint": (
                "Upgrade the extension to 1.7.0+ "
                "(ALTER EXTENSION provsql UPDATE) or switch databases."
            ),
        }), 501

    @app.get("/api/kc/cnf")
    def api_kc_cnf():
        import psycopg
        token = _kc_token()
        if token is None:
            return jsonify({"error": "token is not a valid UUID"}), 400
        weighted = (request.args.get("weighted", "true").lower() != "false")
        try:
            cnf = kc_mod.tseytin_cnf(get_pool(), token, weighted)
        except psycopg.errors.UndefinedFunction as e:
            return _kc_unavailable(e)
        except psycopg.Error as e:
            return jsonify({"error": "tseytin_cnf failed", "detail": str(e).strip()}), 500
        return jsonify({"cnf": cnf, "weighted": weighted})

    @app.get("/api/kc/ddnnf")
    def api_kc_ddnnf():
        import psycopg
        token = _kc_token()
        if token is None:
            return jsonify({"error": "token is not a valid UUID"}), 400
        compiler = request.args.get("compiler", "d4")
        if compiler not in _KC_COMPILERS_WHITELIST:
            return jsonify({
                "error": f"unknown compiler '{compiler}'",
                "hint": f"choose one of: {sorted(_KC_COMPILERS_WHITELIST)}",
            }), 400
        try:
            data = kc_mod.compile_to_ddnnf(get_pool(), token, compiler)
        except psycopg.errors.UndefinedFunction as e:
            return _kc_unavailable(e)
        except psycopg.Error as e:
            return jsonify({
                "error": f"compile_to_ddnnf_dot({compiler}) failed",
                "detail": str(e).strip(),
            }), 500
        except subprocess.CalledProcessError as e:
            return jsonify({
                "error": "dot -Tsvg failed", "detail": str(e),
            }), 500
        data["compiler"] = compiler
        return jsonify(data)

    @app.get("/api/kc/td")
    def api_kc_td():
        import psycopg
        token = _kc_token()
        if token is None:
            return jsonify({"error": "token is not a valid UUID"}), 400
        try:
            data = kc_mod.tree_decomposition(get_pool(), token)
        except psycopg.errors.UndefinedFunction as e:
            return _kc_unavailable(e)
        except psycopg.Error as e:
            return jsonify({
                "error": "tree_decomposition_dot failed",
                "detail": str(e).strip(),
            }), 500
        except subprocess.CalledProcessError as e:
            return jsonify({
                "error": "dot -Tsvg failed", "detail": str(e),
            }), 500
        return jsonify(data)

    @app.get("/api/kc/benchmark")
    def api_kc_benchmark():
        import psycopg
        token = _kc_token()
        if token is None:
            return jsonify({"error": "token is not a valid UUID"}), 400
        try:
            samples = int(request.args.get("samples", "10000"))
        except ValueError:
            return jsonify({"error": "samples must be an integer"}), 400
        if samples <= 0:
            return jsonify({"error": "samples must be a positive integer"}), 400
        raw_compilers = request.args.get("compilers", "d4")
        compilers = [c.strip() for c in raw_compilers.split(",") if c.strip()]
        bad = [c for c in compilers if c not in _KC_COMPILERS_WHITELIST]
        if bad:
            return jsonify({
                "error": f"unknown compiler(s): {bad}",
                "hint": f"choose from: {sorted(_KC_COMPILERS_WHITELIST)}",
            }), 400
        try:
            rows = kc_mod.probability_benchmark(
                get_pool(), token, samples, compilers,
            )
        except psycopg.errors.UndefinedFunction as e:
            return _kc_unavailable(e)
        except psycopg.Error as e:
            return jsonify({
                "error": "probability_benchmark failed",
                "detail": str(e).strip(),
            }), 500
        return jsonify({"rows": rows, "samples": samples, "compilers": compilers})

    _OPTION_KEYS = {
        "max_circuit_depth",
        "max_circuit_nodes",
        "max_sidebar_rows",
        "max_result_rows",
        "statement_timeout_seconds",
        "search_path",
        "tool_search_path",
    }

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
            "max_circuit_nodes": int(app.config["MAX_CIRCUIT_NODES"]),
            "max_sidebar_rows": int(app.config["MAX_SIDEBAR_ROWS"]),
            "max_result_rows": int(app.config["MAX_RESULT_ROWS"]),
            "statement_timeout_seconds": seconds if seconds is not None else 30,
            "search_path": app.config.get("SEARCH_PATH", "") or "",
            "tool_search_path": app.config.get("TOOL_SEARCH_PATH", "") or "",
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
            elif key == "max_circuit_nodes":
                app.config["MAX_CIRCUIT_NODES"] = canonical
                # Drop any cached layouts: the cap change may unblock
                # circuits that were 413'd at the old cap, and stale
                # cache entries would still reflect the old result.
                layout_cache.clear()
            elif key == "max_sidebar_rows":
                app.config["MAX_SIDEBAR_ROWS"] = canonical
            elif key == "max_result_rows":
                app.config["MAX_RESULT_ROWS"] = canonical
            elif key == "statement_timeout_seconds":
                app.config["STATEMENT_TIMEOUT"] = f"{canonical}s"
            elif key == "search_path":
                app.config["SEARCH_PATH"] = canonical
            elif key == "tool_search_path":
                app.config["TOOL_SEARCH_PATH"] = canonical
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
        # The layout cache is keyed on (root, depth) only; any panel
        # GUC that changes what the C function returns (notably
        # provsql.simplify_on_load, provsql.hybrid_evaluation) must
        # invalidate cached scenes so the next /api/circuit fetches
        # the fresh shape.
        if name in ("provsql.simplify_on_load", "provsql.hybrid_evaluation"):
            layout_cache.clear()
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
