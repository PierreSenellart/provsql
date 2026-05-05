"""psycopg helpers: pool, statement-timeout-bounded execution, relation discovery."""
from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import psycopg
from psycopg import sql
from psycopg_pool import ConnectionPool


def _config_dir() -> Path:
    """Where Studio persists user-level state across restarts. Honours
    the testing override `PROVSQL_STUDIO_CONFIG_DIR` so tests don't write
    into the user's real config tree."""
    override = os.environ.get("PROVSQL_STUDIO_CONFIG_DIR")
    if override:
        return Path(override)
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "provsql-studio"


def _read_config_file() -> dict:
    p = _config_dir() / "config.json"
    if not p.exists():
        return {}
    try:
        return json.loads(p.read_text()) or {}
    except Exception:
        return {}


def _write_config_file(data: dict) -> None:
    """Atomically write `data` to the on-disk config file. Best-effort:
    failures (e.g. read-only home dir) are swallowed because losing
    persistence is less harmful than refusing a POST /api/config that
    already updated the in-process state."""
    p = _config_dir() / "config.json"
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(data, indent=2))
        os.replace(tmp, p)
    except Exception:
        pass


def load_persisted_gucs() -> dict[str, str]:
    """Return any GUC overrides written by a previous Studio run, or an
    empty dict if the file is missing/unreadable. Whitelist-filtered so a
    stale config can't escalate into a non-panel GUC."""
    raw = _read_config_file().get("runtime_gucs", {}) or {}
    return {k: str(v) for k, v in raw.items() if k in _PANEL_GUCS}


def save_persisted_gucs(values: dict[str, str]) -> None:
    data = _read_config_file()
    data["runtime_gucs"] = dict(values)
    _write_config_file(data)


def load_persisted_options() -> dict:
    """Return Studio-level option overrides (max_circuit_depth, statement
    timeout, ...) from the on-disk config file. Untrusted values are
    silently dropped — callers should treat missing keys as 'use default'."""
    raw = _read_config_file().get("options", {}) or {}
    out: dict = {}
    if "max_circuit_depth" in raw:
        try:
            n = int(raw["max_circuit_depth"])
            if 1 <= n <= 50:
                out["max_circuit_depth"] = n
        except (TypeError, ValueError):
            pass
    if "statement_timeout_seconds" in raw:
        try:
            n = int(raw["statement_timeout_seconds"])
            if 1 <= n <= 3600:
                out["statement_timeout_seconds"] = n
        except (TypeError, ValueError):
            pass
    return out


def save_persisted_options(values: dict) -> None:
    data = _read_config_file()
    data["options"] = dict(values)
    _write_config_file(data)


def validate_panel_option(name: str, value) -> tuple[str, object]:
    """Validate a Studio-option pair and return the canonical (name, value).
    Raises ValueError on rejection."""
    if name == "max_circuit_depth":
        try:
            n = int(value)
        except (TypeError, ValueError):
            raise ValueError("max_circuit_depth must be an integer")
        if not (1 <= n <= 50):
            raise ValueError("max_circuit_depth must be between 1 and 50")
        return (name, n)
    if name == "statement_timeout_seconds":
        try:
            n = int(value)
        except (TypeError, ValueError):
            raise ValueError("statement_timeout_seconds must be an integer")
        if not (1 <= n <= 3600):
            raise ValueError("statement_timeout_seconds must be between 1 and 3600")
        return (name, n)
    raise ValueError(f"option not user-configurable: {name}")


# Per pg_attribute discovery query, cribbed from where_panel/index.php and
# matched against provsql.identify_token's filter (typname='uuid' AND
# nspname<>'provsql'). Returns one row per provenance-tagged relation.
_RELATIONS_QUERY = """
SELECT
    n.nspname             AS schema_name,
    c.relname             AS table_name,
    c.oid::regclass::text AS regclass
FROM pg_attribute a
JOIN pg_class      c ON a.attrelid = c.oid
JOIN pg_namespace  n ON c.relnamespace = n.oid
JOIN pg_type      ty ON a.atttypid = ty.oid
WHERE a.attname = 'provsql'
  AND ty.typname = 'uuid'
  AND c.relkind = 'r'
  AND n.nspname <> 'provsql'
  AND a.attnum > 0
ORDER BY n.nspname, c.relname
"""


@dataclass
class StatementResult:
    """One block of /api/exec output. Mirrors the JSON shape the front-end consumes."""
    kind: str  # "rows" | "status" | "error"
    columns: list[dict] | None = None      # for rows: [{name, type_oid, type_name}]
    rows: list[list] | None = None         # for rows: list of cell lists
    rowcount: int | None = None            # for rows + status
    message: str | None = None             # for status + error
    sqlstate: str | None = None            # for error

    def to_dict(self) -> dict:
        d = {"kind": self.kind}
        if self.kind == "rows":
            d["columns"] = self.columns
            d["rows"] = self.rows
            d["rowcount"] = self.rowcount
        elif self.kind == "status":
            d["message"] = self.message
            d["rowcount"] = self.rowcount
        elif self.kind == "error":
            d["message"] = self.message
            d["sqlstate"] = self.sqlstate
        return d


def make_pool(dsn: str | None) -> ConnectionPool:
    """Open a connection pool. dsn=None means psycopg picks up PG* env vars.

    Every newly-opened connection is configured with two session-level
    defaults so Studio-internal queries (relation listing, conn info, db
    switch, config show, circuit fetch, leaf resolve, …) stay quiet:

    * `lc_messages = 'C'` so PG diagnostics arrive in English regardless
      of the cluster locale.
    * `provsql.verbose_level = 0` so the gated `provsql_notice` calls
      don't fire on housekeeping queries the user never typed. The user
      can still raise the level for their own queries: `exec_batch`
      applies the panel-managed override via `SET LOCAL`, which takes
      effect only for the duration of that batch and reverts to the
      connection default when the transaction ends.

    Both settings are PGC_SUSET so they only stick for superusers; a
    non-superuser session still sees the cluster defaults (no harm done,
    we just lose the noise filter)."""
    def _configure(conn):
        try:
            with conn.cursor() as cur:
                cur.execute("SET lc_messages = 'C'")
                cur.execute("SET provsql.verbose_level = 0")
            conn.commit()
        except Exception:
            # Don't refuse the connection just because we can't pin
            # the defaults; the user-visible behaviour is still OK.
            try:
                conn.rollback()
            except Exception:
                pass
    return ConnectionPool(
        conninfo=dsn or "",
        min_size=1,
        max_size=8,
        open=True,
        configure=_configure,
    )


def list_databases(pool: ConnectionPool) -> list[str]:
    """Names of every database the current_user is allowed to CONNECT to,
    excluding template databases. Used to populate the database-switcher."""
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT datname FROM pg_database "
            "WHERE NOT datistemplate "
            "  AND has_database_privilege(current_user, datname, 'CONNECT') "
            "ORDER BY datname"
        )
        return [r[0] for r in cur.fetchall()]


def conn_info(pool: ConnectionPool) -> dict:
    """Return the current PostgreSQL session's identity for the chrome chip:
    role, database, and (when not a Unix socket) host. Hits one short SELECT
    per call; the result is small enough that callers can re-fetch freely."""
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT current_user, current_database(), "
            "       coalesce(inet_server_addr()::text, '')"
        )
        user, database, host = cur.fetchone()
    return {"user": user, "database": database, "host": host or None}


def list_relations(pool: ConnectionPool) -> list[dict]:
    """One entry per provenance-tagged relation:
    {schema, table, regclass, columns: [{name, type_name}, ...],
     rows: [{uuid, values}, ...]}.

    The relation content is shown exactly as `SELECT *` would render it under
    the active ProvSQL planner, which means the trailing `provsql` UUID column
    is included alongside the user-defined columns. We pick that column out
    by name to provide a stable per-row identifier for the hover-highlight."""
    out: list[dict] = []
    with pool.connection() as conn:
        with conn.cursor() as cur:
            cur.execute(_RELATIONS_QUERY)
            rels = cur.fetchall()
        for schema, table, regclass in rels:
            with conn.cursor() as cur:
                cur.execute(
                    sql.SQL("SELECT * FROM {}").format(sql.Identifier(schema, table))
                )
                cols = [
                    {"name": d.name, "type_name": _type_name(cur, d.type_code)}
                    for d in cur.description
                ]
                data = cur.fetchall()

            try:
                prov_idx = next(i for i, c in enumerate(cols) if c["name"] == "provsql")
            except StopIteration:
                # Shouldn't happen: the relation made it into _RELATIONS_QUERY.
                continue

            out.append({
                "schema": schema,
                "table": table,
                "regclass": regclass,
                "columns": cols,
                "prov_col": prov_idx,
                "rows": [
                    {"uuid": str(r[prov_idx]), "values": [_to_jsonable(v) for v in r]}
                    for r in data
                ],
            })
    return out


def _to_jsonable(v):
    """Coerce a Python value returned by psycopg into something json.dumps can handle."""
    if v is None:
        return None
    if isinstance(v, (str, int, float, bool)):
        return v
    return str(v)


_NO_PROV_MARKER = "called on a table without provenance"


def exec_batch(
    pool: ConnectionPool,
    statements: list[str],
    *,
    statement_timeout: str,
    where_provenance: bool,
    update_provenance: bool = False,
    wrap_last: bool,
    extra_gucs: dict[str, str] | None = None,
    on_pid=None,
) -> tuple[list[StatementResult], StatementResult | None, dict]:
    """Run `statements` in a single transaction with SET LOCAL settings.

    Returns (intermediate_errors, final_result, meta):
      * intermediate_errors is empty if every non-final statement succeeded;
        otherwise it contains the first failing block (no statements after it
        run). The final_result is None in that case.
      * final_result is the displayed block (rows, status, or error) for the
        last statement, with where-provenance wrapping applied if wrap_last.
      * meta is `{"wrapped": bool, "notice": str | None}`. `wrapped` reports
        whether the wrap was actually applied (the wrap is silently dropped
        with a notice when the user's query references no provenance-tracked
        relation, so e.g. `SELECT * FROM species` for an untagged table
        renders as plain rows instead of erroring out).

    The wrap is:
        SELECT *,
               provsql.provenance() AS __prov,
               provsql.where_provenance(provsql.provenance()) AS __wprov
        FROM (<last>) t
    """
    if not statements:
        return [], None, {"wrapped": False, "notices": []}

    *prelude, last = statements
    intermediate: list[StatementResult] = []

    panel_verbose_str = (extra_gucs or {}).get("provsql.verbose_level", "0")
    try:
        panel_verbose = int(panel_verbose_str)
    except (TypeError, ValueError):
        panel_verbose = 0

    meta: dict = {"wrapped": wrap_last, "notices": []}

    # Notices reach the front-end only for SQL the user explicitly ran via
    # "Send Query" — i.e. the `last` statement (and any prelude statements
    # they typed alongside it). When we run Studio-injected SQL ourselves
    # (the where-mode wrap, with its internal `where_provenance() ->
    # identify_token() -> SELECT * FROM <relation> WHERE provsql=…` chain),
    # we flip the capture flag off so the planner-hook NOTICE messages
    # those inject at provsql.verbose_level >= 20 stay invisible.
    #
    # The `__prov` / `__wprov` filter is a belt: the wrap-and-rewritten
    # SQL contains those markers and is the loudest single source of
    # noise; even if a future code path forgets to flip the flag, this
    # keeps the obvious wrap chatter out.
    capture = [True]

    def _on_notice(diag):
        if not capture[0]:
            return
        msg = diag.message_primary or ""
        if "__prov" in msg or "__wprov" in msg:
            return
        meta["notices"].append({
            "severity": (diag.severity or "NOTICE").upper(),
            "message": msg,
        })

    with pool.connection() as conn:
        # Capture this connection's backend pid before we run anything the
        # user can wait on, so /api/cancel/<id> can resolve the request id
        # to a pid and fire pg_cancel_backend on a separate connection.
        # Failure here is non-fatal: the batch still runs, but cancel
        # won't have a target.
        if on_pid is not None:
            try:
                with conn.cursor() as cur0:
                    cur0.execute("SELECT pg_backend_pid()")
                    on_pid(int(cur0.fetchone()[0]))
            except psycopg.Error:
                pass

        conn.add_notice_handler(_on_notice)
        # Use one transaction so SET LOCAL persists across all statements.
        try:
            with conn.cursor() as cur:
                cur.execute(
                    sql.SQL("SET LOCAL statement_timeout = {}").format(
                        sql.Literal(statement_timeout)
                    )
                )
                cur.execute(
                    "SET LOCAL provsql.where_provenance = "
                    + ("on" if where_provenance else "off")
                )
                cur.execute(
                    "SET LOCAL provsql.update_provenance = "
                    + ("on" if update_provenance else "off")
                )
                # Config-panel GUCs (provsql.active, provsql.verbose_level)
                # apply on top of the per-query toggles so the user can keep
                # the rewriter off via the panel without having to remember
                # it on every query.
                for guc_name, guc_val in (extra_gucs or {}).items():
                    if guc_name not in _PANEL_GUCS:
                        continue
                    cur.execute(
                        sql.SQL("SET LOCAL {} = {}").format(
                            sql.Identifier(*guc_name.split(".")),
                            sql.Literal(guc_val),
                        )
                    )

                # Run prelude statements; halt on first error.
                for stmt in prelude:
                    try:
                        cur.execute(stmt)
                    except psycopg.Error as e:
                        intermediate.append(
                            _user_error_result(e, meta, statement_timeout)
                        )
                        return intermediate, None, meta

                wrapped_sql = (
                    f"SELECT *, "
                    f"provsql.provenance() AS __prov, "
                    f"provsql.where_provenance(provsql.provenance()) AS __wprov "
                    f"FROM ({last}) t"
                )

                if wrap_last:
                    # If the user has bumped verbose_level high enough for
                    # the planner hook to fire NOTICEs, do a probe pass:
                    # run the user's literal SQL inside a read-only
                    # savepoint with capture enabled. Any rewriting notices
                    # collected here describe the user's query. Then
                    # rollback (discard results) and run the actual wrap
                    # with capture off so its where_provenance() ->
                    # identify_token() chain stays silent.
                    #
                    # transaction_read_only on the subtransaction protects
                    # us from queries that have side effects (writable
                    # CTEs, etc.); they error during the probe and we just
                    # discard the captured notices for that case.
                    if panel_verbose >= 20:
                        cur.execute("SAVEPOINT verbose_probe")
                        cur.execute("SET LOCAL transaction_read_only = on")
                        probe_notices_at_start = len(meta["notices"])
                        try:
                            cur.execute(last)
                        except psycopg.Error:
                            # Side effect hit the read-only barrier (or
                            # query had a runtime error). Notices captured
                            # before the failure are still valid (the
                            # rewriting NOTICE fires at plan time).
                            del meta["notices"][probe_notices_at_start:]
                        cur.execute("ROLLBACK TO SAVEPOINT verbose_probe")
                        cur.execute("RELEASE SAVEPOINT verbose_probe")

                    # Wrap phase: silence the notice handler so the wrap's
                    # internal helpers don't pollute the captured set.
                    capture[0] = False
                    cur.execute("SAVEPOINT before_wrap")
                    try:
                        cur.execute(wrapped_sql)
                    except psycopg.Error as e:
                        if _NO_PROV_MARKER in str(e):
                            cur.execute("ROLLBACK TO SAVEPOINT before_wrap")
                            meta["wrapped"] = False
                            meta["notices"].append({
                                "severity": "INFO",
                                "message":
                                    "Source relation is not provenance-tracked; "
                                    "where-provenance highlights are unavailable. "
                                    "Run “SELECT add_provenance('…')” to enable.",
                            })
                            # Re-enable capture for the unwrapped retry —
                            # this run IS the user's query (no probe ran
                            # because the wrap failed before we got there).
                            capture[0] = True
                            try:
                                cur.execute(last)
                            except psycopg.Error as e2:
                                return [], _user_error_result(e2, meta, statement_timeout), meta
                        else:
                            return [], _user_error_result(e, meta, statement_timeout), meta
                    capture[0] = True
                else:
                    # No wrap (circuit mode or unwrappable last). The user's
                    # query runs once with capture enabled — its planner-hook
                    # notices describe the user's literal SQL.
                    try:
                        cur.execute(last)
                    except psycopg.Error as e:
                        return [], _user_error_result(e, meta, statement_timeout), meta

                final = _result_from_cursor(cur)
        except psycopg.Error as e:
            return [], _user_error_result(e, meta, statement_timeout), meta
        finally:
            # Connections come from a pool, so leaving the per-batch handler
            # attached would accumulate one handler per request.
            try:
                conn.remove_notice_handler(_on_notice)
            except Exception:
                pass

    return intermediate, final, meta


def _result_from_cursor(cur: psycopg.Cursor) -> StatementResult:
    """Translate the cursor's most recent statement result into a StatementResult."""
    if cur.description is None:
        # DDL / DML without RETURNING.
        return StatementResult(
            kind="status",
            message=cur.statusmessage or "OK",
            rowcount=cur.rowcount if cur.rowcount >= 0 else None,
        )
    columns = [
        {"name": c.name, "type_oid": c.type_code, "type_name": _type_name(cur, c.type_code)}
        for c in cur.description
    ]
    raw_rows = cur.fetchall()
    rows = [[_to_jsonable(v) for v in r] for r in raw_rows]
    return StatementResult(kind="rows", columns=columns, rows=rows, rowcount=len(rows))


def _type_name(cur: psycopg.Cursor, oid: int) -> str:
    """Look up a type name for the OID. We cache once per connection on the cursor."""
    cache = getattr(cur.connection, "_type_name_cache", None)
    if cache is None:
        cache = {}
        cur.connection._type_name_cache = cache  # type: ignore[attr-defined]
    if oid in cache:
        return cache[oid]
    try:
        with cur.connection.cursor() as c2:
            c2.execute("SELECT typname FROM pg_type WHERE oid = %s", (oid,))
            row = c2.fetchone()
            cache[oid] = row[0] if row else "unknown"
    except psycopg.Error:
        cache[oid] = "unknown"
    return cache[oid]


def _error_result(e: psycopg.Error) -> StatementResult:
    diag = getattr(e, "diag", None)
    return StatementResult(
        kind="error",
        message=str(e).strip(),
        sqlstate=diag.sqlstate if diag else None,
    )


def _is_timeout_error(e: psycopg.Error) -> bool:
    """A statement_timeout firing comes back as sqlstate 57014 with the
    canonical message "canceling statement due to statement timeout". Other
    57014 cancellations (lock_timeout, pg_cancel_backend) carry different
    text, so message-matching distinguishes the timeout cleanly."""
    diag = getattr(e, "diag", None)
    sqlstate = diag.sqlstate if diag else None
    if sqlstate != "57014":
        return False
    msg = (diag.message_primary if diag and diag.message_primary else str(e)).lower()
    return "statement timeout" in msg


def _timeout_error_result(timeout: str) -> StatementResult:
    return StatementResult(
        kind="error",
        message=(
            f"Query canceled: statement timeout ({timeout}) reached. "
            f"Increase the timeout in the Config panel if larger queries are expected."
        ),
        sqlstate="57014",
    )


def _user_error_result(
    e: psycopg.Error, meta: dict, statement_timeout: str
) -> StatementResult:
    """Translate a user-facing psycopg error into a StatementResult. On
    statement_timeout, swap in a Studio-styled message and drop any captured
    PostgreSQL notices: the timeout aborts execution mid-stream and any
    NOTICE / WARNING text already buffered may be partial, so showing them
    alongside the timeout would mislead more than help."""
    if _is_timeout_error(e):
        meta["notices"] = []
        return _timeout_error_result(statement_timeout)
    return _error_result(e)


# GUCs the front-end is allowed to manage. Split into two: those managed
# per-query through the form toggles (where_provenance, update_provenance)
# and those managed by the Config panel (active, verbose_level). The split
# matters because the two groups are applied at different points in
# exec_batch, and the panel must not expose the toggle GUCs (they would
# silently override what the user picks per query).
_TOGGLE_GUCS = {
    "provsql.where_provenance",
    "provsql.update_provenance",
}
_PANEL_GUCS = {
    "provsql.active",
    "provsql.verbose_level",
}
_GUC_WHITELIST = _TOGGLE_GUCS | _PANEL_GUCS


def show_panel_gucs(pool: ConnectionPool, runtime: dict[str, str]) -> dict[str, str]:
    """Return the *effective* values of the panel GUCs after the runtime
    overrides have been applied — i.e. what a query would see right now.
    Runs SHOW inside a one-shot transaction so the SET LOCALs stay scoped.
    """
    out: dict[str, str] = {}
    with pool.connection() as conn, conn.cursor() as cur:
        for name, value in runtime.items():
            if name in _PANEL_GUCS:
                cur.execute(
                    sql.SQL("SET LOCAL {} = {}").format(
                        sql.Identifier(*name.split(".")), sql.Literal(value)
                    )
                )
        for name in sorted(_PANEL_GUCS):
            try:
                cur.execute(f"SHOW {name}")
                out[name] = cur.fetchone()[0]
            except psycopg.Error:
                out[name] = ""
        # Roll the SET LOCALs back so the connection returns to the pool clean.
        conn.rollback()
    return out


def validate_panel_guc(name: str, value: str) -> str:
    """Validate (name, value) for the Config panel and return the canonical
    value as it should be stored. Raises ValueError on rejection."""
    if name not in _PANEL_GUCS:
        raise ValueError(f"GUC not user-configurable: {name}")
    v = (value or "").strip().lower()
    if name == "provsql.active":
        if v in ("on", "true", "1", "yes"):  return "on"
        if v in ("off", "false", "0", "no"): return "off"
        raise ValueError("provsql.active must be on or off")
    if name == "provsql.verbose_level":
        try:
            n = int(v)
        except (TypeError, ValueError):
            raise ValueError("provsql.verbose_level must be an integer 0..100")
        if not (0 <= n <= 100):
            raise ValueError("provsql.verbose_level must be between 0 and 100")
        return str(n)
    raise ValueError(f"GUC not user-configurable: {name}")


def set_guc(pool: ConnectionPool, name: str, value: str) -> None:
    """Legacy, no longer wired by the API: kept for callers that may still
    set a session GUC on a transient connection. Raises ValueError if
    `name` is not whitelisted."""
    if name not in _GUC_WHITELIST:
        raise ValueError(f"GUC not whitelisted: {name}")
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(f"SET {name} = %s", (value,))
