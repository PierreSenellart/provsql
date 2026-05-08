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
    into the user's real config tree.

    Platform-specific defaults via `platformdirs`:
      * Linux:   $XDG_CONFIG_HOME/provsql-studio (else ~/.config/provsql-studio)
      * macOS:   ~/Library/Application Support/provsql-studio
      * Windows: %APPDATA%\\provsql-studio
    """
    override = os.environ.get("PROVSQL_STUDIO_CONFIG_DIR")
    if override:
        return Path(override)
    import platformdirs
    # appauthor=False suppresses the extra `$appauthor` segment platformdirs
    # otherwise inserts on Windows (which defaults to the appname and would
    # produce `…\provsql-studio\provsql-studio`). roaming=True picks
    # %APPDATA% over %LOCALAPPDATA% on Windows so the panel preferences
    # roam between machines on a domain account.
    return Path(platformdirs.user_config_dir(
        "provsql-studio", appauthor=False, roaming=True
    ))


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
    silently dropped : callers should treat missing keys as 'use default'."""
    raw = _read_config_file().get("options", {}) or {}
    out: dict = {}
    if "max_circuit_depth" in raw:
        try:
            n = int(raw["max_circuit_depth"])
            if 1 <= n <= 50:
                out["max_circuit_depth"] = n
        except (TypeError, ValueError):
            pass
    if "max_circuit_nodes" in raw:
        try:
            n = int(raw["max_circuit_nodes"])
            if 10 <= n <= 10000:
                out["max_circuit_nodes"] = n
        except (TypeError, ValueError):
            pass
    if "max_sidebar_rows" in raw:
        try:
            n = int(raw["max_sidebar_rows"])
            if 1 <= n <= 5000:
                out["max_sidebar_rows"] = n
        except (TypeError, ValueError):
            pass
    if "max_result_rows" in raw:
        try:
            n = int(raw["max_result_rows"])
            if 1 <= n <= 100000:
                out["max_result_rows"] = n
        except (TypeError, ValueError):
            pass
    if "statement_timeout_seconds" in raw:
        try:
            n = int(raw["statement_timeout_seconds"])
            if 1 <= n <= 3600:
                out["statement_timeout_seconds"] = n
        except (TypeError, ValueError):
            pass
    if "search_path" in raw:
        try:
            _, canonical = validate_panel_option("search_path", raw["search_path"])
            out["search_path"] = canonical
        except ValueError:
            pass
    if "tool_search_path" in raw:
        try:
            _, canonical = validate_panel_option(
                "tool_search_path", raw["tool_search_path"]
            )
            out["tool_search_path"] = canonical
        except ValueError:
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
    if name == "max_circuit_nodes":
        try:
            n = int(value)
        except (TypeError, ValueError):
            raise ValueError("max_circuit_nodes must be an integer")
        if not (10 <= n <= 10000):
            raise ValueError("max_circuit_nodes must be between 10 and 10000")
        return (name, n)
    if name == "max_sidebar_rows":
        try:
            n = int(value)
        except (TypeError, ValueError):
            raise ValueError("max_sidebar_rows must be an integer")
        if not (1 <= n <= 5000):
            raise ValueError("max_sidebar_rows must be between 1 and 5000")
        return (name, n)
    if name == "max_result_rows":
        try:
            n = int(value)
        except (TypeError, ValueError):
            raise ValueError("max_result_rows must be an integer")
        if not (1 <= n <= 100000):
            raise ValueError("max_result_rows must be between 1 and 100000")
        return (name, n)
    if name == "statement_timeout_seconds":
        try:
            n = int(value)
        except (TypeError, ValueError):
            raise ValueError("statement_timeout_seconds must be an integer")
        if not (1 <= n <= 3600):
            raise ValueError("statement_timeout_seconds must be between 1 and 3600")
        return (name, n)
    if name == "search_path":
        if value is None:
            return (name, "")
        if not isinstance(value, str):
            raise ValueError("search_path must be a string")
        v = value.strip()
        if len(v) > 1024:
            raise ValueError("search_path is too long (max 1024 chars)")
        # Reject characters that could only appear in a SQL injection
        # attempt: a real schema list never contains semicolons, comment
        # markers, or unmatched quotes. The value is passed to
        # set_config(...) as a parameter so it's already safe at runtime,
        # but rejecting up front gives the user a clearer error than a
        # cryptic "invalid value for parameter \"search_path\"" later.
        if any(c in v for c in (";", "--", "/*", "*/")):
            raise ValueError("search_path contains forbidden characters")
        return (name, v)
    if name == "tool_search_path":
        # provsql.tool_search_path is a colon-separated list of directories
        # prepended to PATH when ProvSQL spawns external tools (d4, c2d,
        # weightmc, graph-easy…). Same up-front rejection as search_path
        # for clearer errors; set_config parameterises the value at
        # runtime so it's already injection-safe.
        if value is None:
            return (name, "")
        if not isinstance(value, str):
            raise ValueError("tool_search_path must be a string")
        v = value.strip()
        if len(v) > 1024:
            raise ValueError("tool_search_path is too long (max 1024 chars)")
        if any(c in v for c in (";", "--", "/*", "*/")):
            raise ValueError("tool_search_path contains forbidden characters")
        return (name, v)
    raise ValueError(f"option not user-configurable: {name}")


# Per pg_attribute discovery query, cribbed from where_panel/index.php and
# matched against provsql.identify_token's filter (typname='uuid' AND
# nspname<>'provsql'). Returns one row per provenance-tagged relation.
_RELATIONS_QUERY = """
SELECT
    n.nspname             AS schema_name,
    c.relname             AS table_name,
    c.oid::regclass::text AS regclass,
    c.reltuples::bigint   AS estimated_rows
FROM pg_attribute a
JOIN pg_class      c ON a.attrelid = c.oid
JOIN pg_namespace  n ON c.relnamespace = n.oid
JOIN pg_type      ty ON a.atttypid = ty.oid
WHERE a.attname = 'provsql'
  AND ty.typname = 'uuid'
  AND c.relkind = 'r'
  AND n.nspname <> 'provsql'
  AND a.attnum > 0
  -- Exclude provenance-mapping-shaped relations even if they happen to
  -- carry a provsql column (e.g. CTAS over a tracked table materializes
  -- the planner-injected provsql alongside the user's value/provenance
  -- pair). Mappings label input gates rather than acting as source data,
  -- so they have no place in the where-mode source-relations sidebar.
  -- Same is_mapping predicate as _SCHEMA_QUERY.
  AND NOT (
    EXISTS (
      SELECT 1 FROM pg_attribute aa
      WHERE aa.attrelid = c.oid
        AND aa.attname = 'value'
        AND aa.attnum > 0
        AND NOT aa.attisdropped
    ) AND EXISTS (
      SELECT 1 FROM pg_attribute aa
      WHERE aa.attrelid = c.oid
        AND aa.attname = 'provenance'
        AND aa.atttypid = 'uuid'::regtype
        AND aa.attnum > 0
        AND NOT aa.attisdropped
    )
  )
ORDER BY n.nspname, c.relname
"""


# Tables and views the current_user can SELECT from. Excludes catalog
# schemas (pg_catalog, information_schema) and ProvSQL's internal one,
# which are noise for "what can I query". Columns come back as a
# parallel array_agg ordered by attnum so the front-end can render the
# column list inline without a second round-trip.
_SCHEMA_QUERY = """
SELECT
    n.nspname AS schema_name,
    c.relname AS table_name,
    CASE c.relkind
        WHEN 'r' THEN 'table'
        WHEN 'p' THEN 'table'
        WHEN 'v' THEN 'view'
        WHEN 'm' THEN 'matview'
        WHEN 'f' THEN 'foreign'
        ELSE c.relkind::text
    END AS kind,
    coalesce((
        SELECT array_agg(a.attname ORDER BY a.attnum)
        FROM pg_attribute a
        WHERE a.attrelid = c.oid
          AND a.attnum > 0
          AND NOT a.attisdropped
    ), '{}'::text[]) AS columns,
    coalesce((
        SELECT array_agg(format_type(a.atttypid, a.atttypmod) ORDER BY a.attnum)
        FROM pg_attribute a
        WHERE a.attrelid = c.oid
          AND a.attnum > 0
          AND NOT a.attisdropped
    ), '{}'::text[]) AS column_types,
    -- Matches the predicate _RELATIONS_QUERY uses (provsql.identify_token's
    -- discovery rule): a relation is provenance-tracked iff it carries a
    -- non-dropped `provsql uuid` column.
    EXISTS (
        SELECT 1 FROM pg_attribute a
        WHERE a.attrelid = c.oid
          AND a.attname = 'provsql'
          AND NOT a.attisdropped
          AND a.atttypid = 'uuid'::regtype
    ) AS has_provenance,
    -- Mirrors list_provenance_mappings's discovery rule: a relation is a
    -- provenance mapping iff it has a `value` column and a `provenance uuid`
    -- column (the shape `create_provenance_mapping` produces).
    (EXISTS (
        SELECT 1 FROM pg_attribute a
        WHERE a.attrelid = c.oid
          AND a.attname = 'value'
          AND NOT a.attisdropped
    ) AND EXISTS (
        SELECT 1 FROM pg_attribute a
        WHERE a.attrelid = c.oid
          AND a.attname = 'provenance'
          AND NOT a.attisdropped
          AND a.atttypid = 'uuid'::regtype
    )) AS is_mapping
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE c.relkind IN ('r', 'p', 'v', 'm', 'f')
  AND n.nspname NOT IN ('pg_catalog', 'information_schema', 'provsql')
  AND n.nspname NOT LIKE 'pg_toast%'
  AND n.nspname NOT LIKE 'pg_temp%'
  AND has_table_privilege(current_user, c.oid, 'SELECT')
ORDER BY n.nspname, c.relname
"""


def list_schema(pool: ConnectionPool) -> list[dict]:
    """Return all SELECT-able tables/views/matviews/foreign tables grouped
    by their qualified name, with each row's columns + types. Used by the
    Schema nav button so the user can see what they can query without
    leaving the UI.

    Views and matviews never carry a literal `provsql` column in
    `pg_attribute` even when the ProvSQL planner hook adds one to their
    rewritten output (CS2's `f` and `f_replicated` are the canonical
    case). For these we fall back to a runtime probe: `SELECT * FROM
    <qname> LIMIT 0` exposes the post-rewrite column list, and a
    `provsql uuid` column there means the view propagates provenance."""
    out: list[dict] = []
    with pool.connection() as conn:
        with conn.cursor() as cur:
            cur.execute(_SCHEMA_QUERY)
            rows = cur.fetchall()
        for schema, table, kind, cols, types, has_prov, is_mapping in rows:
            propagates = bool(has_prov) or (
                kind in ("view", "matview")
                and _view_propagates_provenance(conn, schema, table)
            )
            out.append({
                "schema": schema,
                "table": table,
                "kind": kind,
                "columns": [
                    {"name": n, "type": t}
                    for n, t in zip(cols or [], types or [])
                ],
                "has_provenance": propagates,
                "is_mapping": bool(is_mapping),
            })
    return out


_UUID_OID = 2950  # `'uuid'::regtype::oid` is stable across PG versions.


def _view_propagates_provenance(conn, schema: str, table: str) -> bool:
    """Probe whether a `SELECT * FROM <schema>.<table> LIMIT 0` plan
    carries a provsql uuid column. The probe runs inside a savepoint so a
    broken view (or one whose plan errors for any reason) doesn't taint
    the schema-listing transaction."""
    qname = sql.Identifier(schema, table)
    try:
        with conn.transaction(force_rollback=False):
            with conn.cursor() as cur:
                cur.execute(sql.SQL("SELECT * FROM {} LIMIT 0").format(qname))
                desc = cur.description or []
                for col in desc:
                    if col.name == "provsql" and col.type_code == _UUID_OID:
                        return True
                return False
    except Exception:
        return False


@dataclass
class StatementResult:
    """One block of /api/exec output. Mirrors the JSON shape the front-end consumes."""
    kind: str  # "rows" | "status" | "error"
    columns: list[dict] | None = None      # for rows: [{name, type_oid, type_name}]
    rows: list[list] | None = None         # for rows: list of cell lists
    rowcount: int | None = None            # for rows + status
    message: str | None = None             # for status + error
    sqlstate: str | None = None            # for error
    # When the studio session has provsql.aggtoken_text_as_uuid = on (the
    # default for studio connections), agg_token cells arrive as bare
    # UUIDs. agg_display maps each such UUID to its user-facing
    # "value (*)" string so the front-end can render the friendly form
    # while keeping the UUID in data-circuit-uuid for click-through.
    agg_display: dict[str, str] | None = None
    # True when fetchmany peeked one row past the cap : the row list has
    # been trimmed and at least one more row is available. The front-end
    # surfaces this as a "showing first N rows" footer.
    truncated: bool = False
    max_rows: int | None = None

    def to_dict(self) -> dict:
        d = {"kind": self.kind}
        if self.kind == "rows":
            d["columns"] = self.columns
            d["rows"] = self.rows
            d["rowcount"] = self.rowcount
            d["truncated"] = self.truncated
            if self.max_rows is not None:
                d["max_rows"] = self.max_rows
            if self.agg_display:
                d["agg_display"] = self.agg_display
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
        # Disable psycopg3's auto-prepare. The default threshold (5)
        # caches the query plan after the same SQL string has run that
        # many times : but the cached plan locks in whatever
        # provsql.where_provenance / update_provenance was active at
        # prepare time, so subsequent SET LOCAL toggles silently no-op
        # at the planner-hook level (the gates produced reflect the
        # original wp/up choice). Studio runs ad-hoc user queries with
        # varying toggles, so correctness wins over the marginal
        # planning saving.
        conn.prepare_threshold = None
        try:
            with conn.cursor() as cur:
                cur.execute("SET lc_messages = 'C'")
                cur.execute("SET provsql.verbose_level = 0")
                # Flip the agg_token output format to the underlying UUID
                # for every studio session: result-table cells of type
                # agg_token then expose the circuit root for click-
                # through. The user-facing "value (*)" string is recovered
                # per cell via provsql.agg_token_value_text(uuid).
                # Older provsql versions don't know this GUC; the
                # SAVEPOINT lets us swallow the resulting error without
                # poisoning the rest of the configure block (cells then
                # just stay as "value (*)" and aren't clickable).
                cur.execute("SAVEPOINT _aggtok_guc")
                try:
                    cur.execute("SET provsql.aggtoken_text_as_uuid = on")
                    cur.execute("RELEASE SAVEPOINT _aggtok_guc")
                except Exception:
                    cur.execute("ROLLBACK TO SAVEPOINT _aggtok_guc")
                    cur.execute("RELEASE SAVEPOINT _aggtok_guc")
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


def _has_provsql_in_search_path(s: str) -> bool:
    """Return True iff `provsql` appears as a schema in a SHOW search_path
    string. The format is comma-separated; each part may be wrapped in
    double-quotes (PG quotes "$user" and any name needing it). A literal
    substring match would false-positive on a user-named schema like
    'not_provsql', so we tokenise properly."""
    for part in s.split(","):
        part = part.strip()
        if part.startswith('"') and part.endswith('"'):
            part = part[1:-1]
        if part == "provsql":
            return True
    return False


def _with_provsql_last(s: str) -> str:
    """Append `provsql` to a SHOW search_path string if not already
    present. Putting it last means it acts as a fallback for unqualified
    references: user schemas and any same-named objects there resolve
    first; ProvSQL's helper functions are reachable unqualified only when
    nothing else shadows them. PG dedupes resolved schemas internally, so
    a redundant trailing entry would be harmless, but we keep the
    textual form clean for the UI display."""
    return s if _has_provsql_in_search_path(s) else f"{s}, provsql"


def compose_search_path(studio_override: str, session_value: str) -> str:
    """The search_path user queries should see: the Studio-config
    override (Config panel field) when non-empty, otherwise whatever PG's
    own session default reported, with `provsql` always pinned to the
    end so it never shadows user objects. This is what /api/conn
    displays and what exec_batch sets as SET LOCAL search_path before
    each batch."""
    base = (studio_override or "").strip() or session_value
    return _with_provsql_last(base)


def conn_info(pool: ConnectionPool) -> dict:
    """Return the current PostgreSQL session's identity for the chrome chip:
    role, database, host (when not a Unix socket), and the session's
    *resolved* search_path. We use current_schemas(false) instead of
    current_setting('search_path') so the UI shows the schemas user
    queries actually see: $user is substituted (or dropped when no such
    schema exists), missing schemas are silently skipped, and the result
    matches what name resolution will do at query time. Callers compose
    this with the Studio-config override via compose_search_path."""
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT current_user, current_database(), "
            "       coalesce(inet_server_addr()::text, ''), "
            "       coalesce(inet_server_port()::text, ''), "
            "       array_to_string(current_schemas(false), ', ')"
        )
        user, database, host, port, search_path = cur.fetchone()
        # Numeric server version, e.g. 130000 for PG 13, 140000 for PG 14.
        # Surfaced so the UI can gate features that depend on PG-version-
        # specific objects (currently `tstzmultirange` for sr_temporal).
        server_version = conn.info.server_version
    return {
        "user": user,
        "database": database,
        "host": host or None,
        "port": port or None,
        "search_path": search_path,
        "server_version": server_version,
    }


def list_relations(pool: ConnectionPool, *, max_rows: int = 100) -> list[dict]:
    """One entry per provenance-tagged relation:
    {schema, table, regclass, columns: [{name, type_name}, ...],
     rows: [{uuid, values}, ...], first_gate_type,
     truncated: bool, max_rows: int, estimated_rows: int|null}.

    The relation content is shown exactly as `SELECT *` would render it under
    the active ProvSQL planner, which means the trailing `provsql` UUID column
    is included alongside the user-defined columns. We pick that column out
    by name to provide a stable per-row identifier for the hover-highlight.

    Each per-relation SELECT is capped at `max_rows + 1` rows: when the +1
    row comes back we drop it, set `truncated=True`, and let the front-end
    surface a "showing first N of ~T" footer using `estimated_rows`
    (pg_class.reltuples). Without this cap the sidebar would try to render
    every row of every tagged relation on page load, which freezes the
    browser on real datasets.

    `first_gate_type` is `provsql.get_gate_type(...)` of the first row's
    provsql token, used by the front-end's "Input gates only" toggle to hide
    derived relations (where the provsql column carries plus/times/agg
    gates rather than input leaves). One extra round-trip per relation;
    None when the relation is empty or the probe errors."""
    fetch_limit = max(1, int(max_rows)) + 1
    out: list[dict] = []
    with pool.connection() as conn:
        with conn.cursor() as cur:
            cur.execute(_RELATIONS_QUERY)
            rels = cur.fetchall()
        for schema, table, regclass, estimated_rows in rels:
            with conn.cursor() as cur:
                cur.execute(
                    sql.SQL("SELECT * FROM {} LIMIT {}").format(
                        sql.Identifier(schema, table),
                        sql.Literal(fetch_limit),
                    )
                )
                cols = [
                    {"name": d.name, "type_name": _type_name(cur, d.type_code)}
                    for d in cur.description
                ]
                data = cur.fetchall()
            truncated = len(data) > max_rows
            if truncated:
                data = data[:max_rows]

            try:
                prov_idx = next(i for i, c in enumerate(cols) if c["name"] == "provsql")
            except StopIteration:
                # Shouldn't happen: the relation made it into _RELATIONS_QUERY.
                continue

            first_gate_type: str | None = None
            if data:
                first_token = data[0][prov_idx]
                try:
                    with conn.transaction(force_rollback=False):
                        with conn.cursor() as cur:
                            cur.execute(
                                "SELECT provsql.get_gate_type(%s::uuid)",
                                (str(first_token),),
                            )
                            row = cur.fetchone()
                            if row and row[0] is not None:
                                first_gate_type = str(row[0])
                except Exception:
                    # Probe failure (token not in circuit, race, etc.) is
                    # non-fatal : leave first_gate_type=None and let the UI
                    # treat the relation as unfiltered.
                    first_gate_type = None

            # pg_class.reltuples is -1 on never-analyzed tables; expose
            # null in that case so the front-end can suppress the "of ~T"
            # suffix rather than show "of ~-1".
            est = int(estimated_rows) if estimated_rows is not None else None
            if est is not None and est < 0:
                est = None
            out.append({
                "schema": schema,
                "table": table,
                "regclass": regclass,
                "columns": cols,
                "prov_col": prov_idx,
                "first_gate_type": first_gate_type,
                "truncated": truncated,
                "max_rows": int(max_rows),
                "estimated_rows": est,
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


# Compiled-semiring registry. Single source of truth for what the Studio
# exposes in the eval strip: the SQL function to call, the value-types its
# kernel accepts on the mapping (None means "polymorphic, any type"), and
# the minimum PG version required (None means "no gate").
#
# `interval-union` is a *family*: one UI option backed by three kernels
# (sr_temporal / sr_interval_num / sr_interval_int) that all implement the
# same `IntervalUnion(Oid)` algebra over different multirange carriers.
# The kernel dispatches on the mapping's value type at evaluation time;
# see `_resolve_compiled_semiring`.
#
# `boolexpr` accepts an *optional* mapping: with one, leaves are labelled
# by the mapping's `value` column; without one, leaves render as bare
# `x<id>` placeholders. Probability and prov-xml are handled separately,
# outside this registry.
#
# Numeric base types accepted by the scoring semirings (counting, tropical,
# viterbi, lukasiewicz). `format_type(_, NULL)` produces these exact
# spellings, so the set is matched against `value_base_type`, not the
# typmod-parameterised `value_type`.
_NUMERIC_TYPES = (
    "smallint", "integer", "bigint",
    "numeric", "real", "double precision",
)
_INTERVAL_KERNELS = {
    "tstzmultirange": "sr_temporal",
    "nummultirange":  "sr_interval_num",
    "int4multirange": "sr_interval_int",
}
_COMPILED_SEMIRINGS: dict[str, dict] = {
    # Boolean & symbolic.
    "boolexpr": {"func": "sr_boolexpr", "needs_mapping": False, "types": None},
    "boolean":  {"func": "sr_boolean",  "types": ("boolean",)},
    "formula":  {"func": "sr_formula",  "types": None},
    # Lineage.
    "why":      {"func": "sr_why",      "types": None},
    "which":    {"func": "sr_which",    "types": None},
    "how":      {"func": "sr_how",      "types": None},
    # Numeric / scoring.
    "counting":    {"func": "sr_counting",    "types": _NUMERIC_TYPES},
    "tropical":    {"func": "sr_tropical",    "types": _NUMERIC_TYPES},
    "viterbi":     {"func": "sr_viterbi",     "types": _NUMERIC_TYPES},
    "lukasiewicz": {"func": "sr_lukasiewicz", "types": _NUMERIC_TYPES},
    # Interval-valued (PG14+ multirange family). `func` is None: the
    # kernel is picked at evaluation time from the mapping's value type.
    "interval-union": {
        "func": None,
        "types": tuple(_INTERVAL_KERNELS.keys()),
        "min_pg": 140000,
    },
    # User-enum carrier. `accepts_enum: True` filters to mappings whose
    # `value` column is any user-defined enum (typtype = 'e'); the
    # carrier's bottom and top come from `pg_enum.enumsortorder`. The
    # third arg of `sr_minmax`/`sr_maxmin` is a sample value of that
    # enum, used only for type inference; the dispatch synthesises one
    # via `enum_first(NULL::<base_type>)`.
    "minmax": {"func": "sr_minmax", "accepts_enum": True},
    "maxmin": {"func": "sr_maxmin", "accepts_enum": True},
}
# probability_evaluate accepts these methods (see src/probability_evaluate.cpp).
# Of these, `monte-carlo` requires a sample count as `arguments`; `compilation`
# requires a compiler name (d4 / c2d / minic2d / dsharp); `weightmc` takes a
# weightmc-specific args string. The others ignore `arguments` (and warn on
# `possible-worlds` if one is given).
_PROBABILITY_METHODS = {
    "",                   # let provsql pick (independent → tree-decomposition → d4)
    "independent",
    "tree-decomposition",
    "possible-worlds",
    "monte-carlo",
    "compilation",
    "weightmc",
}


def list_provenance_mappings(pool: ConnectionPool) -> list[dict]:
    """Discover tables / views shaped like a provenance mapping :
    `create_provenance_mapping` produces `(value <T>, provenance uuid)`,
    and `sr_*` semirings consume any regclass with that signature.
    Returns one entry per match, qualified by schema. `value_type` is the
    parameterised type of the `value` column for display
    (e.g., `numeric(10,2)`, `varchar(40)`, `classification_level`);
    `value_base_type` is the same type without the `atttypmod`
    parameterisation (e.g., `numeric`, `character varying`), used to
    filter the eval-strip dropdown by type compatibility against
    compiled-semiring registries and custom-wrapper return types."""
    out: list[dict] = []
    sql_text = """
        SELECT n.nspname, c.relname, pg_table_is_visible(c.oid) AS visible,
               format_type(va.atttypid, va.atttypmod) AS value_type,
               format_type(va.atttypid, NULL)         AS value_base_type,
               t.typtype = 'e'                        AS is_enum
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        JOIN pg_attribute va
          ON va.attrelid = c.oid
         AND va.attname = 'value'
         AND NOT va.attisdropped
        JOIN pg_type t ON t.oid = va.atttypid
        WHERE c.relkind IN ('r', 'v', 'm')
          AND n.nspname NOT IN ('pg_catalog', 'information_schema', 'provsql')
          AND n.nspname NOT LIKE 'pg_%'
          AND has_table_privilege(current_user, c.oid, 'SELECT')
          AND EXISTS (
            SELECT 1 FROM pg_attribute a
            WHERE a.attrelid = c.oid
              AND a.attname = 'provenance'
              AND NOT a.attisdropped
              AND a.atttypid = 'uuid'::regtype
          )
        ORDER BY n.nspname, c.relname
    """
    # `pg_table_is_visible` is true iff the table is reachable through the
    # connection's search_path AND no earlier search_path entry shadows it.
    # When that holds the relation can be referenced unqualified, so the
    # display name drops the schema. The qualified name still goes back
    # in `qname` for the API call so the regclass cast is unambiguous.
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(sql_text)
        for schema, name, visible, value_type, value_base_type, is_enum in cur.fetchall():
            out.append({
                "schema": schema,
                "name": name,
                "qname": f"{schema}.{name}",
                "display_name": name if visible else f"{schema}.{name}",
                "value_type": str(value_type),
                "value_base_type": str(value_base_type),
                "is_enum": bool(is_enum),
            })
    return out


_CUSTOM_SEMIRING_FILTER = """
    p.pronargs = 2
    AND p.proargtypes[1] = 'regclass'::regtype
    AND p.prosrc ~ '\\mprovenance_evaluate\\M'
    AND has_function_privilege(current_user, p.oid, 'EXECUTE')
    AND n.nspname NOT IN ('pg_catalog', 'information_schema')
"""
# Word-boundary regex on `prosrc` matches the bare `provenance_evaluate`
# call but not `provenance_evaluate_compiled` (since `_` is a word
# character, `\M` won't bind between `evaluate` and `_compiled`). This
# excludes the four `sr_*` wrappers from discovery while accepting the
# case-study wrappers and `provsql.union_tstzintervals`.


def list_custom_semirings(pool: ConnectionPool) -> list[dict]:
    """Discover SQL/PL functions shaped like a custom-semiring wrapper:
    `f(<token>, regclass) RETURNS T` whose body invokes the bare
    `provenance_evaluate(...)`. The first argument's type is left
    unconstrained so wrappers declared with `anyelement` for parsing
    reasons still surface.
    Returns one entry per wrapper, qualified by schema."""
    sql_text = f"""
        SELECT n.nspname AS schema, p.proname AS name,
               format_type(p.prorettype, NULL) AS rettype
        FROM pg_proc p
        JOIN pg_namespace n ON n.oid = p.pronamespace
        WHERE {_CUSTOM_SEMIRING_FILTER}
        ORDER BY (n.nspname = 'provsql') DESC, n.nspname, p.proname
    """
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(sql_text)
        rows = cur.fetchall()
    # Bare names are nicer in the dropdown; fall back to schema.name only
    # for entries that actually collide (rare in practice).
    name_count: dict[str, int] = {}
    for _, name, _rt in rows:
        name_count[name] = name_count.get(name, 0) + 1
    out: list[dict] = []
    for schema, name, rettype in rows:
        qname = f"{schema}.{name}"
        out.append({
            "schema": schema,
            "name": name,
            "qname": qname,
            "display_name": qname if name_count[name] > 1 else name,
            "return_type": rettype,
        })
    return out


def evaluate_circuit(
    pool: ConnectionPool,
    *,
    token: str,
    semiring: str,
    mapping: str | None,
    method: str | None,
    arguments: str | None = None,
    function: str | None = None,
    statement_timeout: str,
    search_path: str = "",
    tool_search_path: str = "",
) -> dict:
    """Run a compiled-semiring or probability evaluation against `token`.
    Returns `{result, kind}` ready to JSON-encode. Raises ValueError on
    bad input and propagates psycopg.Error from the SQL call.

    The caller is responsible for catching psycopg errors and translating
    them to HTTP : we don't shape them here so the route can also surface
    the underlying SQLSTATE / message verbatim."""
    if semiring == "boolexpr":
        # Like prov-xml: the mapping is optional. With one, leaves are
        # labelled by the mapping's `value` column; without one, leaves
        # render as bare `x<id>` placeholders.
        if mapping:
            sql_stmt = sql.SQL(
                "SELECT provsql.sr_boolexpr({}::uuid, {}::regclass)::text"
            ).format(sql.Literal(token), sql.Literal(mapping))
        else:
            sql_stmt = sql.SQL(
                "SELECT provsql.sr_boolexpr({}::uuid)::text"
            ).format(sql.Literal(token))
        params: tuple = ()
    elif semiring == "probability":
        m = (method or "").strip()
        if m not in _PROBABILITY_METHODS:
            raise ValueError(f"unknown probability method: {m!r}")
        a = (arguments or "").strip()
        # Arguments are method-specific (sample count for monte-carlo,
        # compiler name for compilation, etc.). probability_evaluate's
        # third arg is nullable; pass NULL when empty so provsql falls
        # back to the method's own default behaviour.
        sql_stmt = sql.SQL(
            "SELECT provsql.probability_evaluate({}::uuid, {}, {})"
        ).format(
            sql.Literal(token),
            sql.Literal(m or None),
            sql.Literal(a or None),
        )
        params = ()
    elif semiring == "custom":
        fn = (function or "").strip()
        if not fn:
            raise ValueError("custom semiring requires a function name")
        if not mapping:
            raise ValueError("custom semiring requires a provenance mapping")
        if "." not in fn:
            raise ValueError(
                f"custom semiring function must be schema-qualified: {fn!r}"
            )
        schema, name = fn.split(".", 1)
        # Re-run the discovery filter restricted to (schema, name) so a
        # crafted payload can't reach an arbitrary `(uuid, regclass)`
        # function. The filter is identical to list_custom_semirings'.
        with pool.connection() as conn, conn.cursor() as cur:
            cur.execute(
                f"""SELECT 1 FROM pg_proc p
                    JOIN pg_namespace n ON n.oid = p.pronamespace
                    WHERE {_CUSTOM_SEMIRING_FILTER}
                      AND n.nspname = %s AND p.proname = %s""",
                (schema, name),
            )
            if cur.fetchone() is None:
                raise ValueError(f"unknown custom semiring: {fn!r}")
            cur.execute(
                f"""SELECT format_type(p.prorettype, NULL)
                    FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
                    WHERE n.nspname = %s AND p.proname = %s
                      AND p.pronargs = 2 AND p.proargtypes[1] = 'regclass'::regtype""",
                (schema, name),
            )
            rt_row = cur.fetchone()
        rettype = rt_row[0] if rt_row else None
        sql_stmt = sql.SQL("SELECT {}({}::uuid, {}::regclass)").format(
            sql.Identifier(schema, name),
            sql.Literal(token),
            sql.Literal(mapping),
        )
        params = ()
    elif semiring == "prov-xml":
        # Export the circuit as ProvenanceXML. The mapping is optional :
        # without it, leaves carry bare UUIDs; with it, leaves are
        # labelled by the mapping's `value` column.
        if mapping:
            sql_stmt = sql.SQL(
                "SELECT provsql.to_provxml({}::uuid, {}::regclass)"
            ).format(sql.Literal(token), sql.Literal(mapping))
        else:
            sql_stmt = sql.SQL(
                "SELECT provsql.to_provxml({}::uuid)"
            ).format(sql.Literal(token))
        params = ()
    elif semiring in _COMPILED_SEMIRINGS:
        spec = _COMPILED_SEMIRINGS[semiring]
        if not mapping:
            raise ValueError(
                f"semiring {semiring!r} requires a provenance mapping"
            )
        # `mapping` is a qualified name like "public.species_mapping"; the
        # ::regclass cast both validates the identifier and resolves it
        # against the search_path. Each sr_* function has its own natural
        # return type (int for counting, bool for boolean, text for
        # formula / why) : we keep that so `_to_jsonable` can pass the
        # primitive through to JSON without wrapping it as a string.
        kernel, base_type = _resolve_compiled_semiring(pool, semiring, spec, mapping)
        fn = sql.Identifier("provsql", kernel)
        # Cast multirange results to text server-side so we get PG's
        # canonical literal (`{(,)}`, `{[lo, hi)}`) instead of psycopg's
        # `str(Multirange([Range(None, None, '()')]))` which prints as
        # `{(None, None)}` and surprises users when the universal
        # multirange (the `one()` of IntervalUnion) shows up : that
        # happens whenever the mapping doesn't cover an input gate, since
        # `GenericCircuit::evaluate` falls back to `semiring.one()` for
        # unmapped leaves. Other compiled kernels return scalar types
        # that flow through `_to_jsonable` cleanly, so leave their SQL
        # alone.
        if semiring == "interval-union":
            sql_stmt = sql.SQL("SELECT {}({}::uuid, {}::regclass)::text").format(
                fn, sql.Literal(token), sql.Literal(mapping)
            )
        elif spec.get("accepts_enum"):
            # `sr_minmax`/`sr_maxmin` need a sample value of the carrier
            # enum for type inference. `enum_first(NULL::<base_type>)`
            # synthesises one without needing the mapping to contain any
            # row. `base_type` came from `format_type(_, NULL)` on the
            # mapping's value column, so it's a properly schema-qualified,
            # quoted PG type expression: safe to splice as raw SQL.
            sql_stmt = sql.SQL(
                "SELECT {}({}::uuid, {}::regclass, enum_first(NULL::{}))"
            ).format(
                fn, sql.Literal(token), sql.Literal(mapping),
                sql.SQL(base_type),
            )
        else:
            sql_stmt = sql.SQL("SELECT {}({}::uuid, {}::regclass)").format(
                fn, sql.Literal(token), sql.Literal(mapping)
            )
        params = ()
    else:
        raise ValueError(f"unknown semiring: {semiring!r}")

    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            sql.SQL("SET LOCAL statement_timeout = {}").format(
                sql.Literal(statement_timeout)
            )
        )
        # Custom-semiring wrappers commonly call `provenance_evaluate`
        # unqualified (every case-study wrapper does), so pin `provsql` on
        # the search_path the same way exec_batch does. Mirrors the
        # composition rules: explicit Studio override otherwise the
        # session default, with `provsql` appended when missing.
        if (search_path or "").strip():
            target_path = compose_search_path(search_path, "")
        else:
            cur.execute("SHOW search_path")
            target_path = compose_search_path("", cur.fetchone()[0])
        cur.execute(
            "SELECT set_config('search_path', %s, true)",
            (target_path,),
        )
        # Probability methods (compilation, weightmc) and PROV-XML export
        # may shell out to d4 / c2d / minic2d / dsharp / weightmc /
        # graph-easy. provsql.tool_search_path lets the user point
        # ProvSQL at a non-default install location for those binaries.
        if (tool_search_path or "").strip():
            cur.execute(
                "SELECT set_config('provsql.tool_search_path', %s, true)",
                (tool_search_path,),
            )
        cur.execute(sql_stmt, params)
        row = cur.fetchone()
    value = row[0] if row else None
    out = {
        "result": _to_jsonable(value),
        "kind": _result_kind(semiring),
    }
    if semiring == "custom":
        out["function"] = function
        out["type_name"] = rettype
    return out


def _result_kind(semiring: str) -> str:
    if semiring in ("probability", "tropical", "viterbi", "lukasiewicz"):
        return "float"
    if semiring == "counting":
        return "int"
    if semiring == "boolean":
        return "bool"
    if semiring == "custom":
        return "custom"
    if semiring == "prov-xml":
        return "xml"
    # boolexpr / formula / why / which all return text; interval-union
    # returns a multirange (psycopg's Multirange str-formats as
    # `{[lo, hi), ...}`, which `_to_jsonable` carries through unchanged).
    return "text"


def _mapping_value_info(pool: ConnectionPool, mapping: str) -> tuple[str, bool]:
    """Return (unparameterised type of `mapping.value`, is_enum), used for
    type-compatibility checks against compiled-semiring registries.
    `is_enum` is true iff the value column's type has `pg_type.typtype = 'e'`,
    i.e., it is a user-defined enum carrier accepted by `sr_minmax`/`sr_maxmin`.
    Raises ValueError if the relation isn't shaped like a mapping (no
    `value` column reachable from the current search_path)."""
    if "." in mapping:
        schema, name = mapping.split(".", 1)
        where = "n.nspname = %s AND c.relname = %s"
        params: tuple = (schema, name)
    else:
        where = "c.relname = %s AND pg_table_is_visible(c.oid)"
        params = (mapping,)
    sql_text = f"""
        SELECT format_type(va.atttypid, NULL), t.typtype = 'e'
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        JOIN pg_attribute va
          ON va.attrelid = c.oid
         AND va.attname = 'value'
         AND NOT va.attisdropped
        JOIN pg_type t ON t.oid = va.atttypid
        WHERE {where}
    """
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(sql_text, params)
        row = cur.fetchone()
    if row is None:
        raise ValueError(
            f"mapping {mapping!r} not found or has no `value` column"
        )
    return str(row[0]), bool(row[1])


def _resolve_compiled_semiring(
    pool: ConnectionPool, semiring: str, spec: dict, mapping: str
) -> tuple[str, str | None]:
    """Pick the SQL function name to call for a compiled-semiring entry.
    Validates the mapping's value type against the registry's accepted
    set and gates families (whose `func` is None) on that type. PG-version
    gating happens here too so crafted payloads can't bypass the frontend
    hide.

    Returns (kernel, base_type): `base_type` is None for entries that did
    not need a type lookup (polymorphic kernels), the unparameterised
    `format_type` of the mapping's `value` column otherwise. Callers that
    need to splice a type expression into the dispatch SQL (currently
    only `sr_minmax`/`sr_maxmin`'s third argument) consume it.

    Raises ValueError on PG-version mismatch or value-type incompatibility;
    the caller turns that into a clean 400."""
    min_pg = spec.get("min_pg")
    if min_pg is not None:
        with pool.connection() as conn:
            if conn.info.server_version < min_pg:
                raise ValueError(
                    f"semiring {semiring!r} requires PostgreSQL "
                    f"{min_pg // 10000} or newer"
                )
    accepted = spec.get("types")
    accepts_enum = bool(spec.get("accepts_enum"))
    if accepted is None and not accepts_enum and spec["func"] is not None:
        # Polymorphic kernel, single function : nothing to validate.
        return spec["func"], None
    base_type, is_enum = _mapping_value_info(pool, mapping)
    if accepts_enum:
        if not is_enum:
            raise ValueError(
                f"semiring {semiring!r} expects an enum-typed mapping, "
                f"got {base_type!r}"
            )
        return spec["func"], base_type
    if accepted is not None and base_type not in accepted:
        accepted_list = ", ".join(accepted)
        raise ValueError(
            f"semiring {semiring!r} expects mapping value type in "
            f"{{{accepted_list}}}, got {base_type!r}"
        )
    if spec["func"] is not None:
        return spec["func"], base_type
    # Family entry (`interval-union`): dispatch by carrier type.
    if semiring == "interval-union":
        return _INTERVAL_KERNELS[base_type], base_type
    # Defensive: every family entry should have an explicit dispatch
    # branch. Hitting this means the registry got an entry the resolver
    # doesn't know about.
    raise ValueError(f"no kernel dispatch for semiring {semiring!r}")


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
    search_path: str = "",
    tool_search_path: str = "",
    max_result_rows: int | None = None,
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
    # "Send Query" : i.e. the `last` statement (and any prelude statements
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
        # `elog_node_display` (fired by provsql at verbose_level >= 50 to
        # dump the parse tree before/after rewriting) puts the title in
        # message_primary and the actual node dump in message_detail; a
        # bare message_primary capture would silently drop the dump.
        detail = diag.message_detail or ""
        hint = diag.message_hint or ""
        parts = [msg]
        if detail:
            parts.append("DETAIL:  " + detail)
        if hint:
            parts.append("HINT:  " + hint)
        meta["notices"].append({
            "severity": (diag.severity or "NOTICE").upper(),
            "message": "\n".join(parts),
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
                # Pin provsql to the end of search_path for the batch
                # (so its helpers are reachable unqualified as a fallback
                # without shadowing user objects). When the Studio
                # config provides a search_path override we also use it
                # as the base; otherwise we read whatever the session
                # already has and append provsql if missing. set_config
                # parameterises the value so the user-supplied portion
                # never reaches PG as raw SQL.
                if (search_path or "").strip():
                    target_path = compose_search_path(search_path, "")
                else:
                    cur.execute("SHOW search_path")
                    target_path = compose_search_path("", cur.fetchone()[0])
                cur.execute(
                    "SELECT set_config('search_path', %s, true)",
                    (target_path,),
                )
                cur.execute(
                    "SET LOCAL provsql.where_provenance = "
                    + ("on" if where_provenance else "off")
                )
                cur.execute(
                    "SET LOCAL provsql.update_provenance = "
                    + ("on" if update_provenance else "off")
                )
                # provsql.tool_search_path: prepended to $PATH when
                # provsql spawns external tools (d4 / c2d / weightmc /
                # graph-easy). set_config parameterises the value so
                # the user-supplied portion never reaches PG as raw SQL.
                if (tool_search_path or "").strip():
                    cur.execute(
                        "SELECT set_config('provsql.tool_search_path', %s, true)",
                        (tool_search_path,),
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

                # The wrap projection calls where_provenance() once per
                # row. PG's executor is pull-based, so pushing the LIMIT
                # into the outermost SELECT stops the projection after
                # N+1 rows : without it, a `SELECT *` over 50k rows runs
                # 50k where_provenance() calls server-side before a
                # single byte reaches the client. The trailing cap_clause
                # mirrors the fetchmany peek used in circuit mode (one
                # extra row distinguishes "exactly N" from "at least N").
                cap_clause = (
                    f" LIMIT {int(max_result_rows) + 1}"
                    if max_result_rows is not None and max_result_rows >= 0
                    else ""
                )
                wrapped_sql = (
                    f"SELECT *, "
                    f"provsql.provenance() AS __prov, "
                    f"provsql.where_provenance(provsql.provenance()) AS __wprov "
                    f"FROM ({last}) t"
                    f"{cap_clause}"
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
                            # Re-enable capture for the unwrapped retry :
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
                    # query runs once with capture enabled : its planner-hook
                    # notices describe the user's literal SQL.
                    try:
                        cur.execute(last)
                    except psycopg.Error as e:
                        return [], _user_error_result(e, meta, statement_timeout), meta

                final = _result_from_cursor(cur, max_rows=max_result_rows)
                # If the result has any agg_token columns, resolve their
                # underlying UUIDs back to "value (*)" display strings in
                # one shot via provsql.agg_token_value_text. The pool
                # session has aggtoken_text_as_uuid = on, so the cells
                # arrived as bare UUIDs; the front-end uses agg_display
                # to render the friendly form without losing the UUID
                # click target.
                if final.kind == "rows":
                    final.agg_display = _resolve_agg_display(cur, final)
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


def _result_from_cursor(
    cur: psycopg.Cursor, *, max_rows: int | None = None
) -> StatementResult:
    """Translate the cursor's most recent statement result into a StatementResult.

    When `max_rows` is given, fetch at most `max_rows + 1` rows so we can
    detect whether the query had more rows than the cap. The +1 row is
    dropped before serialization; `truncated=True` is set so the
    front-end can render a "showing first N rows" footer. fetchmany also
    spares the json-serialization cost on the surplus rows."""
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
    truncated = False
    if max_rows is not None and max_rows >= 0:
        raw_rows = cur.fetchmany(max_rows + 1)
        if len(raw_rows) > max_rows:
            truncated = True
            raw_rows = raw_rows[:max_rows]
    else:
        raw_rows = cur.fetchall()
    rows = [[_to_jsonable(v) for v in r] for r in raw_rows]
    return StatementResult(
        kind="rows",
        columns=columns,
        rows=rows,
        rowcount=len(rows),
        truncated=truncated,
        max_rows=max_rows,
    )


def _resolve_agg_display(
    cur: psycopg.Cursor, result: StatementResult
) -> dict[str, str] | None:
    """Bulk-resolve agg_token UUIDs in `result` back to "value (*)" via
    provsql.agg_token_value_text. Returns None when there's nothing to
    resolve so the JSON payload stays minimal; returns an empty dict if
    older provsql doesn't expose the helper (the front-end then falls
    back to displaying the raw UUID, same shape as before this fix).
    """
    if not result.columns or not result.rows:
        return None
    agg_idx = [i for i, c in enumerate(result.columns)
               if (c.get("type_name") or "").lower() == "agg_token"]
    if not agg_idx:
        return None
    uuids: set[str] = set()
    for r in result.rows:
        for i in agg_idx:
            v = r[i]
            if isinstance(v, str) and v:
                uuids.add(v)
    if not uuids:
        return {}
    try:
        with cur.connection.cursor() as c2:
            c2.execute(
                "SELECT u, provsql.agg_token_value_text(u::uuid) "
                "FROM unnest(%s::text[]) AS u",
                (list(uuids),),
            )
            return {row[0]: row[1] for row in c2.fetchall() if row[1] is not None}
    except psycopg.Error:
        # Older provsql without agg_token_value_text. Don't poison the
        # batch's transaction (we used a separate cursor); return empty
        # so the front-end keeps showing the UUID as-is.
        return {}


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
    overrides have been applied : i.e. what a query would see right now.
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
