"""psycopg helpers: pool, statement-timeout-bounded execution, relation discovery."""
from __future__ import annotations

import json
import math
import os
import re
from dataclasses import dataclass
from pathlib import Path

import psycopg
import psycopg.conninfo
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


# Per pg_attribute discovery query, matched against
# provsql.identify_token's filter (typname='uuid' AND nspname<>'provsql').
# Returns one row per provenance-tagged relation.
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
    )) AS is_mapping,
    -- Per-relation provenance-tracking metadata recorded by
    -- add_provenance / repair_key / provenance_guard. NULL when the
    -- table is not tracked or the database predates the per-table
    -- store (1.5.0 and earlier). 'tid' = independent leaves,
    -- 'bid' = block-correlated, 'opaque' = the user supplied custom
    -- provsql values so the safe-query rewriter must refuse the
    -- table.  Wrapped in a sub-SELECT so the missing-function path
    -- (1.5.0 schema before the upgrade has run) degrades to NULL
    -- instead of erroring the whole query.
    (SELECT ti.kind
     FROM provsql.get_table_info(c.oid) ti) AS prov_kind,
    -- True when an unqualified reference to this relation in the
    -- current session's search_path would resolve to this exact OID.
    -- Used by the schema panel : a click on the row prefills
    -- `SELECT * FROM <relname>;` only when it would actually find
    -- the relation we showed; otherwise it qualifies with the
    -- schema.  to_regclass(quote_ident(...)) does the same lookup
    -- the parser would, so case-sensitive and special-character
    -- names are handled correctly.
    (to_regclass(quote_ident(c.relname))::oid
     IS NOT DISTINCT FROM c.oid) AS bare_resolves,
    -- Primary-key column names, so the schema panel can underline the
    -- key attributes (keys decide query safety; see case study 7).
    coalesce((
        SELECT array_agg(a.attname)
        FROM pg_index i
        JOIN pg_attribute a
          ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey)
        WHERE i.indrelid = c.oid AND i.indisprimary
    ), '{}'::text[]) AS pk_columns,
    -- Repair_key ("BID") grouping-key column names, so the schema panel
    -- can mark them with a dotted underline, distinct from the primary
    -- key. get_table_info().block_key is an attnum array (empty for
    -- non-BID tables); resolve it to names, preserving key-column order.
    coalesce((
        SELECT array_agg(a.attname ORDER BY k.ord)
        FROM provsql.get_table_info(c.oid) ti,
             LATERAL unnest(ti.block_key) WITH ORDINALITY AS k(attnum, ord)
        JOIN pg_attribute a
          ON a.attrelid = c.oid AND a.attnum = k.attnum
    ), '{}'::text[]) AS block_key_columns
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
            try:
                cur.execute(_SCHEMA_QUERY)
            except psycopg.errors.UndefinedFunction:
                # `provsql.get_table_info` was added in 1.6.0; on pre-1.6.0
                # schemas drop the LEFT JOIN and just leave `prov_kind` NULL.
                conn.rollback()
                cur.execute(
                    _SCHEMA_QUERY.replace(
                        "(SELECT ti.kind\n"
                        "     FROM provsql.get_table_info(c.oid) ti) AS prov_kind",
                        "NULL::text AS prov_kind",
                    ).replace(
                        "coalesce((\n"
                        "        SELECT array_agg(a.attname ORDER BY k.ord)\n"
                        "        FROM provsql.get_table_info(c.oid) ti,\n"
                        "             LATERAL unnest(ti.block_key) WITH ORDINALITY AS k(attnum, ord)\n"
                        "        JOIN pg_attribute a\n"
                        "          ON a.attrelid = c.oid AND a.attnum = k.attnum\n"
                        "    ), '{}'::text[]) AS block_key_columns",
                        "'{}'::text[] AS block_key_columns",
                    )
                )
            rows = cur.fetchall()
        for (schema, table, kind, cols, types,
             has_prov, is_mapping, prov_kind, bare_resolves,
             pk_cols, block_key_cols) in rows:
            propagates = bool(has_prov)
            # Views and matviews don't have a `provsql_table_info` entry,
            # so the SQL-level `prov_kind` is always NULL for them. We
            # probe the rewritten plan instead : the probe simultaneously
            # detects propagation (provsql column in the plan output) and
            # captures the classifier NOTICE on the view body to set the
            # kind. Tables keep their authoritative `provsql.get_table_info`
            # value (set at add_provenance / repair_key time) verbatim.
            if kind in ("view", "matview"):
                view_propagates, view_kind = _probe_view_provenance(
                    conn, schema, table)
                if view_propagates:
                    propagates = True
                    if view_kind is not None:
                        prov_kind = view_kind
            out.append({
                "schema": schema,
                "table": table,
                "kind": kind,
                "columns": [
                    {"name": n, "type": t,
                     "pk": n in (pk_cols or []),
                     "bidkey": n in (block_key_cols or [])}
                    for n, t in zip(cols or [], types or [])
                ],
                "has_provenance": propagates,
                "is_mapping": bool(is_mapping),
                # 'tid' / 'bid' / 'opaque' / None ; surfaced as a
                # discreet PROV-TID / PROV-BID / PROV-OPAQUE badge in
                # the schema panel so the user can tell at a glance
                # which probability-evaluation semantics apply.
                "prov_kind": prov_kind,
                # True when an unqualified reference to the table name
                # would resolve to this exact relation under the current
                # search_path. Drives the schema-panel click handler's
                # choice between `SELECT * FROM staff` and
                # `SELECT * FROM view_demo.staff`.
                "bare_resolves": bool(bare_resolves),
            })
    return out


_UUID_OID = 2950  # `'uuid'::regtype::oid` is stable across PG versions.

# Matches the NOTICE the planner-hook classifier emits when
# `provsql.classify_top_level` is on (see src/classify_query.c).
_CLASSIFY_NOTICE_RE = re.compile(
    r"^ProvSQL: query result is (TID|BID|OPAQUE)\b"
)


def _probe_view_provenance(
    conn, schema: str, table: str
) -> tuple[bool, str | None]:
    """Probe a view (or matview) once and return
    ``(propagates_provenance, prov_kind)``.

    * ``propagates_provenance`` is True iff
      ``SELECT * FROM <schema>.<table> LIMIT 0`` exposes a ``provsql uuid``
      column. Views/matviews never carry a literal `provsql` column in
      `pg_attribute` even when the ProvSQL planner hook adds one to their
      rewritten output (CS2's ``f`` and ``f_replicated`` are the canonical
      case), so this column-list probe is the only source of truth.
    * ``prov_kind`` is the planner-hook classifier's verdict
      (``'tid'`` / ``'bid'`` / ``'opaque'``) on the view body, captured
      via the ``NOTICE`` it emits when ``provsql.classify_top_level`` is
      on.  ``None`` when the classifier didn't certify (e.g. older
      extension without the GUC, or no NOTICE fired).  The schema-panel
      pill uses this to upgrade the bare ``prov`` label on a view to
      ``prov-tid`` / ``prov-bid`` so the user sees what kind of
      uncertainty the view actually carries, computed live from its
      body rather than from a recorded tag (views have no
      ``provsql_table_info`` entry).

    The probe runs inside a transaction with ``force_rollback=True`` so
    the GUC change and any side effects (the probe is read-only, but
    PG still opens a real transaction) don't leak.  The GUC ``SET LOCAL``
    is itself wrapped in a savepoint so older databases without the
    1.6.0-dev GUC degrade to ``(propagates_only, None)`` instead of
    aborting.
    """
    qname = sql.Identifier(schema, table)
    notices: list[str] = []

    def collect(diag):
        msg = diag.message_primary or ""
        if msg.startswith("ProvSQL: query result is "):
            notices.append(msg)

    propagates = False
    try:
        with conn.transaction(force_rollback=True):
            with conn.cursor() as cur:
                cur.execute("SAVEPOINT classify_guc")
                try:
                    cur.execute("SET LOCAL provsql.classify_top_level = on")
                except psycopg.errors.UndefinedObject:
                    cur.execute("ROLLBACK TO SAVEPOINT classify_guc")
                else:
                    cur.execute("RELEASE SAVEPOINT classify_guc")
            conn.add_notice_handler(collect)
            try:
                with conn.cursor() as cur:
                    cur.execute(sql.SQL("SELECT * FROM {} LIMIT 0").format(qname))
                    desc = cur.description or []
                    for col in desc:
                        if col.name == "provsql" and col.type_code == _UUID_OID:
                            propagates = True
                            break
            finally:
                conn.remove_notice_handler(collect)
    except Exception:
        return False, None

    for msg in notices:
        m = _CLASSIFY_NOTICE_RE.match(msg)
        if m:
            return propagates, m.group(1).lower()
    return propagates, None


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
    switch, config show, circuit fetch, leaf resolve…) stay quiet:

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
    return ConnectionPool(
        conninfo=dsn or "",
        min_size=1,
        max_size=8,
        open=True,
        configure=configure_connection,
    )


def configure_connection(conn: psycopg.Connection) -> None:
    """Session-default setup shared by every Studio connection: the
    pool's `configure` hook and the pinned notebook-kernel connections
    (see `make_pool`'s docstring for the rationale of each setting)."""
    # Disable psycopg3's auto-prepare. The default threshold (5)
    # caches the query plan after the same SQL string has run that
    # many times : but the cached plan locks in whatever
    # provsql.provenance / update_provenance was active at
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


def empty_database(pool: ConnectionPool) -> list[str]:
    """Drop every user schema in the current database (CASCADE) and
    recreate an empty `public`, then reinstall the provsql extension.
    Backs the nav bar's "empty database" action (a clean slate for
    re-running a notebook from the top).

    The reinstall is not optional: `uuid-ossp` conventionally lives in
    `public`, so dropping it CASCADEs through provsql too. Recreating
    both also resets provsql's own state (update-provenance history,
    tool overrides), which is exactly what "fully empty" should mean.

    Returns the list of schemas dropped."""
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT nspname FROM pg_namespace "
            "WHERE nspname NOT IN ('pg_catalog', 'information_schema', "
            "                      'provsql') "
            "  AND nspname NOT LIKE 'pg\\_%' "
            "ORDER BY nspname"
        )
        schemas = [r[0] for r in cur.fetchall()]
        for s in schemas:
            cur.execute(sql.SQL("DROP SCHEMA {} CASCADE").format(
                sql.Identifier(s)))
        # The provsql schema survives the loop above but its extension
        # objects may have been cascade-dropped with uuid-ossp; clear
        # any residue so CREATE EXTENSION repopulates cleanly.
        cur.execute("DROP EXTENSION IF EXISTS provsql CASCADE")
        cur.execute("DROP SCHEMA IF EXISTS provsql CASCADE")
        cur.execute("CREATE SCHEMA public")
        # Pre-PG15 default grants on public, so unqualified CREATE TABLE
        # keeps working for every role that could before.
        cur.execute("GRANT USAGE, CREATE ON SCHEMA public TO PUBLIC")
        # CREATE EXTENSION installs into the first existing schema on the
        # session search_path. That search_path is inherited from the
        # database default, which may point at a user schema we just
        # dropped above (e.g. the regression fixture sets the DB-level
        # search_path to provsql_test) -- leaving no valid creation
        # target and a "no schema has been selected for creation" error.
        # Pin it to the public schema we just recreated.
        cur.execute("SET search_path TO public")
        cur.execute("CREATE EXTENSION IF NOT EXISTS provsql CASCADE")
    return schemas


def create_database(dsn: str | None, name: str) -> str | None:
    """Create database `name` (requires CREATEDB) and best-effort install
    the provsql extension in it, so a scratch database minted for a
    notebook is immediately ready for provenance work.

    Returns a warning string when the database was created but the
    extension install failed (no install package, no preload, not
    superuser, ...); the caller surfaces it and the user's own
    `CREATE EXTENSION` / setup cells can still fix things up. Raises
    `psycopg.Error` when the CREATE DATABASE itself fails."""
    # CREATE DATABASE cannot run inside a transaction: autocommit, off-pool.
    with psycopg.connect(dsn or "", autocommit=True) as conn:
        conn.execute(sql.SQL("CREATE DATABASE {}").format(sql.Identifier(name)))

    params = psycopg.conninfo.conninfo_to_dict(dsn or "")
    params["dbname"] = name
    try:
        with psycopg.connect(psycopg.conninfo.make_conninfo(**params),
                             autocommit=True) as conn:
            conn.execute("CREATE EXTENSION IF NOT EXISTS provsql CASCADE")
    except psycopg.Error as e:
        return f"database created, but installing provsql failed: {str(e).strip()}"
    return None


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
            "       array_to_string(current_schemas(false), ', '), "
            "       (SELECT extversion FROM pg_extension WHERE extname = 'provsql'), "
            "       current_setting('is_superuser')::bool"
        )
        (user, database, host, port, search_path, ext_version,
         is_superuser) = cur.fetchone()
        # Numeric server version, e.g. 130000 for PG 13, 140000 for PG 14.
        # Surfaced so the UI can gate features that depend on PG-version-
        # specific objects (currently `tstzmultirange` for sr_temporal).
        server_version = conn.info.server_version
        # provsql.tool_search_path is PGC_SUSET (superuser-only) since the
        # security hardening: only a superuser -- or, on PG 15+, a role
        # GRANTed SET on the parameter -- may set it. Surface whether this
        # session can, so the Config panel marks the field read-only /
        # admin-managed instead of letting set_config fail at query time.
        tool_search_path_settable = bool(is_superuser)
        if not tool_search_path_settable and server_version >= 150000:
            cur.execute("SAVEPOINT _tsp_priv")
            try:
                cur.execute(
                    "SELECT has_parameter_privilege("
                    "'provsql.tool_search_path', 'SET')"
                )
                tool_search_path_settable = bool(cur.fetchone()[0])
                cur.execute("RELEASE SAVEPOINT _tsp_priv")
            except psycopg.Error:
                cur.execute("ROLLBACK TO SAVEPOINT _tsp_priv")
                cur.execute("RELEASE SAVEPOINT _tsp_priv")
    return {
        "user": user,
        "database": database,
        "host": host or None,
        "port": port or None,
        "search_path": search_path,
        "server_version": server_version,
        "extension_version": ext_version or None,
        "tool_search_path_settable": tool_search_path_settable,
    }


def apply_tool_search_path(cur, tool_search_path: str) -> bool:
    """Apply provsql.tool_search_path for the current batch, if non-empty.

    provsql.tool_search_path is PGC_SUSET (superuser-only) since the
    security hardening, so a non-superuser session cannot set it and
    set_config raises insufficient_privilege. We run it inside a
    SAVEPOINT and swallow that error, so the user's query still executes:
    the path is then simply admin-managed (the session falls back to the
    server PATH, or to a value an admin pinned via ALTER ROLE ... SET).
    Returns True iff the value was applied. Mirrors the _aggtok_guc
    savepoint pattern used for the other PGC_SUSET GUCs."""
    if not (tool_search_path or "").strip():
        return False
    cur.execute("SAVEPOINT _tsp_guc")
    try:
        cur.execute(
            "SELECT set_config('provsql.tool_search_path', %s, true)",
            (tool_search_path,),
        )
        cur.execute("RELEASE SAVEPOINT _tsp_guc")
        return True
    except psycopg.errors.InsufficientPrivilege:
        cur.execute("ROLLBACK TO SAVEPOINT _tsp_guc")
        cur.execute("RELEASE SAVEPOINT _tsp_guc")
        return False


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
    if isinstance(v, bool):
        return v
    if isinstance(v, float):
        # Postgres float8 carries +/-Infinity / NaN (e.g. rv_support of a
        # normal RV is [-Infinity, +Infinity]).  Python's json.dumps emits
        # them as the bare literals Infinity / -Infinity / NaN, which
        # browser JSON.parse rejects.  Send the string form so the wire is
        # valid JSON; circuit.js already accepts the strings 'Infinity' /
        # '-Infinity' alongside the JS number infinities.
        if math.isnan(v):
            return "NaN"
        if math.isinf(v):
            return "Infinity" if v > 0 else "-Infinity"
        return v
    if isinstance(v, (str, int)):
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
# `boolean_rewrite_compatible`: mirrors the C++
# `semiring::*::compatibleWithBooleanRewrite()` predicate (see
# src/semiring/*.h). When the root of the circuit being evaluated is a
# `gate_assumed` marker (produced e.g. by the safe-query rewriter
# under the 'boolean' provenance class), `GenericCircuit::evaluate` refuses
# any semiring whose predicate is false. Studio uses this flag to
# narrow the eval-strip dropdown in that case so users do not pick a
# semiring that will throw. Drift detection is handled by
# `test/sql/safe_query_semiring.sql` (which exercises one tagged token
# against each semiring) ; if the C++ side ever flips a value, that
# test will fail and this dict must be updated.
#
# `absorptive`: mirrors the C++ `semiring::*::absorptive()` predicate.
# A persistent `gate_assumed` wrapper labelled 'absorptive' (a cyclic
# recursive query truncated at the absorptive value fixpoint) is only
# sound for absorptive semirings; the in-memory absorptive-fold marker
# additionally tolerates Boolean-rewrite-compatible semirings (the
# folds preserve the Boolean function).  Same drift rule:
# `test/sql/absorptive_recursion.sql` pins the refusals.
_COMPILED_SEMIRINGS: dict[str, dict] = {
    # Boolean & symbolic.
    "boolexpr": {"func": "sr_boolexpr", "needs_mapping": False, "types": None,
                 "boolean_rewrite_compatible": True, "absorptive": True},
    "boolean":  {"func": "sr_boolean",  "types": ("boolean",),
                 "boolean_rewrite_compatible": True, "absorptive": True},
    "formula":  {"func": "sr_formula",  "types": None,
                 "boolean_rewrite_compatible": True, "absorptive": False},
    # Lineage.
    "why":      {"func": "sr_why",      "types": None,
                 "boolean_rewrite_compatible": False, "absorptive": False},
    "which":    {"func": "sr_which",    "types": None,
                 "boolean_rewrite_compatible": False, "absorptive": False},
    "how":      {"func": "sr_how",      "types": None,
                 "boolean_rewrite_compatible": False, "absorptive": False},
    # Numeric / scoring.
    "counting":    {"func": "sr_counting",    "types": _NUMERIC_TYPES,
                    "boolean_rewrite_compatible": False,
                    "absorptive": False},
    "tropical":    {"func": "sr_tropical",    "types": _NUMERIC_TYPES,
                    "boolean_rewrite_compatible": False,
                    "absorptive": False},
    # Min-plus restricted to nonnegative costs: absorptive, so it
    # accepts truncated cyclic-recursion tokens (exact min-cost
    # reachability on cyclic data); negative costs are rejected by the
    # kernel.  Dispatches to sr_tropical(..., nonnegative => true).
    "tropical-nonneg": {"func": "sr_tropical", "types": _NUMERIC_TYPES,
                        "nonneg": True,
                        "boolean_rewrite_compatible": False,
                        "absorptive": True},
    "viterbi":     {"func": "sr_viterbi",     "types": _NUMERIC_TYPES,
                    "boolean_rewrite_compatible": False,
                    "absorptive": True},
    "lukasiewicz": {"func": "sr_lukasiewicz", "types": _NUMERIC_TYPES,
                    "boolean_rewrite_compatible": False,
                    "absorptive": True},
    # Interval-valued (PG14+ multirange family). `func` is None: the
    # kernel is picked at evaluation time from the mapping's value type.
    "interval-union": {
        "func": None,
        "types": tuple(_INTERVAL_KERNELS.keys()),
        "min_pg": 140000,
        "boolean_rewrite_compatible": True,
        "absorptive": True,
    },
    # User-enum carrier. `accepts_enum: True` filters to mappings whose
    # `value` column is any user-defined enum (typtype = 'e'); the
    # carrier's bottom and top come from `pg_enum.enumsortorder`. The
    # third arg of `sr_minmax`/`sr_maxmin` is a sample value of that
    # enum, used only for type inference; the dispatch synthesises one
    # via `enum_first(NULL::<base_type>)`.
    "minmax": {"func": "sr_minmax", "accepts_enum": True,
               "boolean_rewrite_compatible": False, "absorptive": True},
    "maxmin": {"func": "sr_maxmin", "accepts_enum": True,
               "boolean_rewrite_compatible": False, "absorptive": True},
}
# probability_evaluate accepts these methods (see src/probability_evaluate.cpp).
# Of these, `monte-carlo` requires a sample count as `arguments`; `compilation`
# requires a compiler name (d4 / d4v2 / c2d / minic2d / dsharp / panini-*);
# `wmc` takes a tool name ("ganak" / "sharpsat-td" / "dpmc" /
# "weightmc[;ε;δ]"); legacy `weightmc` takes "ε;δ". The rest ignore
# `arguments` (and warn on `possible-worlds` if one is given).
_PROBABILITY_METHODS = {
    "",                   # let provsql pick (the cost-ordered exact chooser)
    "exact",              # alias for the empty/default method (the exact chooser)
    "relative",           # granted tolerance: (1±eps) guarantee, conf 1-delta;
                          # returns exact when cheaply available, else an FPRAS
    "additive",           # granted tolerance: |est-p| <= eps, conf 1-delta;
                          # returns exact when cheaply available, else MC
    "independent",
    "inversion-free",     # exact, requires an inversion-free certificate on the root
    "mobius",             # exact, requires a Möbius (gate_mobius) root
    "tree-decomposition",
    "possible-worlds",
    "sieve",              # exact inclusion-exclusion over a monotone DNF
    "d-tree",             # deterministic anytime d-tree (exact; certified interval)
    "monte-carlo",
    "karp-luby",          # FPRAS for DNF-shaped circuits (relative (eps,delta))
    "stopping-rule",      # whole-circuit relative (eps,delta) FPRAS (any circuit)
    "compilation",
    "wmc",
    "weightmc",           # legacy alias for wmc/weightmc
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
    extra_gucs: dict[str, str] | None = None,
    condition_uuid: str | None = None,
) -> dict:
    """Run a compiled-semiring or probability evaluation against `token`.
    Returns `{result, kind}` ready to JSON-encode. Raises ValueError on
    bad input and propagates psycopg.Error from the SQL call.

    The caller is responsible for catching psycopg errors and translating
    them to HTTP : we don't shape them here so the route can also surface
    the underlying SQLSTATE / message verbatim.

    `condition_uuid` is the conditioning gate for scalar evaluators
    (`distribution-profile`, `moment`, `sample`).  When provided, it's
    spliced as the `prov` argument to provsql.rv_moment / rv_support /
    rv_histogram / rv_sample so the truncated distribution is returned
    instead of the unconditional one.  Ignored by every other semiring."""
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
                """SELECT format_type(p.prorettype, NULL)
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
    elif semiring == "distribution-profile":
        # Scalar-only evaluator: fan out to rv_support / rv_moment(_, 1, false)
        # / rv_moment(_, 2, true) / rv_histogram in one round-trip, package
        # the four results as a single dict so the strip can render the
        # support / expectation / variance / histogram panel from one
        # response.  Calls the internal C kernels directly rather than the
        # polymorphic `support` / `expected` / `variance` dispatchers
        # because (a) there is no uuid -> random_variable cast (the type
        # carries a (uuid, value) pair, not just a uuid), and (b) the
        # frontend filters the option to scalar root gates only, so the
        # polymorphic agg_token / numeric branches don't apply here.  For
        # non-scalar gates rv_support falls back to the conservative
        # all-real interval and rv_moment raises with the underlying type
        # mismatch, which surfaces as a 500 with the diagnostic.
        bins_str = (arguments or "30").strip()
        try:
            bins = int(bins_str)
        except (TypeError, ValueError):
            raise ValueError(
                f"distribution-profile: bins must be an integer (got {bins_str!r})"
            )
        if bins <= 0:
            raise ValueError(
                f"distribution-profile: bins must be positive (got {bins})"
            )
        with pool.connection() as conn, conn.cursor() as cur:
            cur.execute(
                sql.SQL("SET LOCAL statement_timeout = {}").format(
                    sql.Literal(statement_timeout)
                )
            )
            # Same search_path / tool_search_path / panel-GUC composition
            # as the main branch below; duplicated here because we don't
            # share the trailer.  rv_histogram honours rv_mc_samples and
            # monte_carlo_seed; rv_moment honours the same seed for its
            # MC fallback.
            if (search_path or "").strip():
                target_path = compose_search_path(search_path, "")
            else:
                cur.execute("SHOW search_path")
                target_path = compose_search_path("", cur.fetchone()[0])
            cur.execute(
                "SELECT set_config('search_path', %s, true)",
                (target_path,),
            )
            apply_tool_search_path(cur, tool_search_path)
            for guc_name, guc_val in (extra_gucs or {}).items():
                if guc_name not in _EXTRA_GUC_WHITELIST:
                    continue
                cur.execute(
                    sql.SQL("SET LOCAL {} = {}").format(
                        sql.Identifier(*guc_name.split(".")),
                        sql.Literal(guc_val),
                    )
                )
            # When condition_uuid is set, splice it as the prov arg of
            # every RV-side call so the strip shows the truncated
            # distribution-profile.  rv_moment / rv_support /
            # rv_histogram all default `prov` to gate_one() (the
            # unconditional case), so unset is a no-op at the SQL
            # layer.
            cond_expr = (
                sql.SQL("{}::uuid").format(sql.Literal(condition_uuid))
                if condition_uuid
                else sql.SQL("provsql.gate_one()")
            )
            cur.execute(
                sql.SQL("""
                    SELECT s.lo, s.hi,
                           provsql.rv_moment({tok}::uuid, 1, false, {cond}),
                           provsql.rv_moment({tok}::uuid, 2, true,  {cond}),
                           provsql.rv_histogram({tok}::uuid, {bins}, {cond}),
                           provsql.rv_analytical_curves(
                             {tok}::uuid, 100, {cond})
                      FROM provsql.rv_support({tok}::uuid, {cond}) s
                """).format(
                    tok=sql.Literal(token), bins=sql.Literal(bins),
                    cond=cond_expr,
                )
            )
            row = cur.fetchone()
        if row:
            lo, hi, exp_val, var_val, hist, curves = row
        else:
            lo, hi, exp_val, var_val, hist, curves = (None,) * 6
        return {
            "result": {
                "support": [_to_jsonable(lo), _to_jsonable(hi)],
                "expected": _to_jsonable(exp_val),
                "variance": _to_jsonable(var_val),
                # rv_histogram returns jsonb; psycopg surfaces it as a
                # parsed Python list of {bin_lo, bin_hi, count} dicts that
                # is already JSON-native, so bypass `_to_jsonable` (which
                # would stringify the list).
                "histogram": hist if isinstance(hist, (list, dict)) else json.loads(hist) if hist else [],
                "bins": bins,
                # rv_analytical_curves returns jsonb {pdf: [...], cdf: [...]}
                # for closed-form bare-gate_rv roots (optionally truncated),
                # NULL otherwise.  Forward the NULL verbatim so the frontend
                # can skip the overlay without a special case.
                "analytical_curves":
                    curves if isinstance(curves, (list, dict))
                    else json.loads(curves) if curves else None,
            },
            "kind": "distribution-profile",
        }
    elif semiring == "moment":
        # Scalar-only evaluator that calls provsql.rv_moment(token, k,
        # central) directly.  Arguments arrive as `"k;central"` where
        # central is `"raw"` or `"central"`; we parse both halves and
        # validate before splicing.  Returns a single float8 result,
        # rendered by the eval strip alongside Probability and the
        # numeric compiled semirings.
        arg = (arguments or "").strip()
        if ";" in arg:
            k_str, central_str = arg.split(";", 1)
        else:
            k_str, central_str = arg, "raw"
        k_str = k_str.strip()
        central_str = central_str.strip().lower()
        try:
            k_value = int(k_str)
        except (TypeError, ValueError):
            raise ValueError(
                f"moment: k must be a non-negative integer (got {k_str!r})"
            )
        if k_value < 0:
            raise ValueError(
                f"moment: k must be non-negative (got {k_value})"
            )
        if central_str in ("central", "true", "1", "yes"):
            central = True
        elif central_str in ("raw", "false", "0", "no", ""):
            central = False
        else:
            raise ValueError(
                f"moment: central must be 'raw' or 'central' (got {central_str!r})"
            )
        # Splice the conditioning event when set; otherwise the `prov`
        # default of gate_one() makes the call unconditional.
        cond_expr = (
            sql.SQL("{}::uuid").format(sql.Literal(condition_uuid))
            if condition_uuid
            else sql.SQL("provsql.gate_one()")
        )
        # Aggregate roots (and conditioned aggregates) have an *exact*
        # moment, computed by the agg_token dispatcher (moment /
        # central_moment -> agg_raw_moment, which enumerates the
        # aggregate's per-row contributions and calls the exact
        # probability_evaluate).  rv_moment, by contrast, has no
        # analytical arm for an `agg` gate and falls back to Monte Carlo
        # (so it depends on provsql.rv_mc_samples and errors when that is
        # 0).  Route aggregate roots to the exact path so Circuit-mode
        # moments match the notebook's expected()/variance() and stay
        # correct at any sample budget; everything else stays on
        # rv_moment (continuous RVs: analytical when closed-form, else MC).
        agg_fn = (
            sql.SQL("provsql.central_moment") if central
            else sql.SQL("provsql.moment")
        )
        sql_stmt = sql.SQL(
            "SELECT CASE"
            "  WHEN provsql.get_gate_type(t) = 'agg'"
            "       OR (provsql.get_gate_type(t) = 'conditioned'"
            "           AND provsql.get_gate_type((provsql.get_children(t))[1])"
            "               IN ('agg', 'semimod'))"
            "  THEN {agg_fn}(provsql.agg_token_make(t, 0), {k}, {cond})"
            "  ELSE provsql.rv_moment(t, {k}, {central}, {cond})"
            " END"
            " FROM (SELECT {tok}::uuid AS t) q"
        ).format(
            agg_fn=agg_fn, k=sql.Literal(k_value),
            central=sql.Literal(central), cond=cond_expr,
            tok=sql.Literal(token),
        )
        params = ()
    elif semiring == "sample":
        # Scalar-only evaluator returning a list of MC samples for the
        # gate, optionally conditioned on `condition_uuid`.  The
        # `arguments` field carries the sample count `n` as a positive
        # integer (default 100).  Each row of provsql.rv_sample is one
        # float, which we collect into a JSON-able list for the
        # frontend Sample tab.
        n_str = (arguments or "100").strip()
        try:
            n_value = int(n_str)
        except (TypeError, ValueError):
            raise ValueError(
                f"sample: n must be a positive integer (got {n_str!r})"
            )
        if n_value <= 0:
            raise ValueError(f"sample: n must be positive (got {n_value})")
        cond_expr = (
            sql.SQL("{}::uuid").format(sql.Literal(condition_uuid))
            if condition_uuid
            else sql.SQL("provsql.gate_one()")
        )
        with pool.connection() as conn, conn.cursor() as cur:
            cur.execute(
                sql.SQL("SET LOCAL statement_timeout = {}").format(
                    sql.Literal(statement_timeout)
                )
            )
            if (search_path or "").strip():
                target_path = compose_search_path(search_path, "")
            else:
                cur.execute("SHOW search_path")
                target_path = compose_search_path("", cur.fetchone()[0])
            cur.execute(
                "SELECT set_config('search_path', %s, true)",
                (target_path,),
            )
            apply_tool_search_path(cur, tool_search_path)
            for guc_name, guc_val in (extra_gucs or {}).items():
                if guc_name not in _EXTRA_GUC_WHITELIST:
                    continue
                cur.execute(
                    sql.SQL("SET LOCAL {} = {}").format(
                        sql.Identifier(*guc_name.split(".")),
                        sql.Literal(guc_val),
                    )
                )
            cur.execute(
                sql.SQL(
                    "SELECT v FROM provsql.rv_sample({}::uuid, {}, {}) AS v"
                ).format(sql.Literal(token), sql.Literal(n_value), cond_expr)
            )
            rows = cur.fetchall()
        return {
            "result": {
                "samples": [_to_jsonable(r[0]) for r in rows],
                "n_requested": n_value,
                "n_returned": len(rows),
            },
            "kind": "sample",
        }
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
        elif spec.get("nonneg"):
            # The nonnegative (absorptive) min-plus variant.
            sql_stmt = sql.SQL(
                "SELECT {}({}::uuid, {}::regclass, true)"
            ).format(fn, sql.Literal(token), sql.Literal(mapping))
        else:
            sql_stmt = sql.SQL("SELECT {}({}::uuid, {}::regclass)").format(
                fn, sql.Literal(token), sql.Literal(mapping)
            )
        params = ()
    else:
        raise ValueError(f"unknown semiring: {semiring!r}")

    notices: list[str] = []
    resolved_method: str | None = None
    def _on_notice(diag):
        msg = diag.message_primary or ""
        if "__prov" in msg or "__wprov" in msg:
            return
        notices.append(msg)
    with pool.connection() as conn, conn.cursor() as cur:
        conn.add_notice_handler(_on_notice)
        try:
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
            # Probability methods (compilation, wmc) and PROV-XML export
            # may shell out to d4 / d4v2 / c2d / minic2d / dsharp / Panini /
            # ganak / sharpsat-td / dmc / weightmc / graph-easy.
            # provsql.tool_search_path lets the user point ProvSQL at a
            # non-default install location for those binaries.
            apply_tool_search_path(cur, tool_search_path)
            # Panel-managed GUCs (provsql.rv_mc_samples,
            # provsql.monte_carlo_seed, provsql.simplify_on_load, ...) must
            # apply here too, otherwise the evaluate-strip's probability
            # call ignores the user's panel overrides (e.g. setting
            # rv_mc_samples=0 to disable the MC fallback still gets MC).
            # exec_batch already does this for batched queries; mirror it.
            for guc_name, guc_val in (extra_gucs or {}).items():
                if guc_name not in _EXTRA_GUC_WHITELIST:
                    continue
                cur.execute(
                    sql.SQL("SET LOCAL {} = {}").format(
                        sql.Identifier(*guc_name.split(".")),
                        sql.Literal(guc_val),
                    )
                )
            # AFTER applying the user's runtime_gucs, ensure
            # provsql.verbose_level is at least 5 so the eval-strip's
            # probability calls surface the level-5 informational
            # notices (e.g., "gate_cmp shortcut by probability-side
            # pre-pass"). The user's own GUC still wins when it is
            # *higher* than 5 (so verbose=100 keeps everything
            # chatty), but a panel-configured silence (verbose=0)
            # gets bumped up to 5 here.
            cur.execute(
                "SELECT set_config('provsql.verbose_level', "
                "GREATEST(5, current_setting('provsql.verbose_level', true)::int)::text, true)"
            )
            # Clear provsql.last_eval_method so that, after the call, it holds
            # exactly the method the chooser settled on for THIS evaluation
            # (the extension appends to it; it accumulates across a session).
            # Session-level (is_local=false) to match how the extension records
            # it, so we read back the value the call set rather than a shadowing
            # SET LOCAL.  The next eval resets it again, so the pooled
            # connection does not accumulate.
            if semiring == "probability":
                cur.execute(
                    "SELECT set_config('provsql.last_eval_method', '', false)"
                )
            cur.execute(sql_stmt, params)
            row = cur.fetchone()
            if semiring == "probability":
                cur.execute(
                    "SELECT current_setting('provsql.last_eval_method', true)"
                )
                rm = cur.fetchone()
                resolved_method = (rm[0] or None) if rm else None
        finally:
            try:
                conn.remove_notice_handler(_on_notice)
            except Exception:
                pass
    value = row[0] if row else None
    out = {
        "result": _to_jsonable(value),
        "kind": _result_kind(semiring),
    }
    # The method the chooser actually used (e.g. a 'relative'/'additive'/default
    # request that resolved to an exact method when one was cheap, or to an
    # estimator otherwise).  Surfaced by the eval strip next to the result.
    if resolved_method:
        out["resolved_method"] = resolved_method
    if notices:
        out["notices"] = notices
    if semiring == "custom":
        out["function"] = function
        out["type_name"] = rettype
    return out


def contributions(
    pool: ConnectionPool,
    *,
    token: str,
    measure: str,
    method: str | None = None,
    arguments: str | None = None,
    mapping: str | None = None,
    statement_timeout: str,
    search_path: str = "",
    tool_search_path: str = "",
    extra_gucs: dict[str, str] | None = None,
) -> dict:
    """Per-input Shapley / Banzhaf contributions toward `token`.

    Backs Contributions mode. Runs `provsql.shapley_all_vars(token, method,
    arguments, banzhaf)` once -- one C function serves both measures via its
    `banzhaf` flag (`banzhaf_all_vars` is just `shapley_all_vars(.., 't')`)
    -- and returns one entry per input variable, sorted by value descending:
    `{variable, value, label}`. `value` is the contribution; `label` is the
    mapping's `value` column when a provenance `mapping` is given (the CS2
    §15 `JOIN ON provenance = variable` idiom), else None so the front-end
    lazily resolves the source row via /api/leaf, exactly like the
    circuit-mode leaf chips.

    Returns `{result, kind, measure, method, mapping}` ready to JSON-encode.
    Raises ValueError on bad input; propagates psycopg.Error to the caller
    (the route shapes it into HTTP, as for /api/evaluate)."""
    measure = (measure or "").strip().lower()
    if measure not in ("shapley", "banzhaf"):
        raise ValueError(f"unknown contribution measure: {measure!r}")
    m = (method or "").strip() or None
    a = (arguments or "").strip() or None

    # When a mapping is requested, validate it against the same discovery
    # rule the picker uses so a crafted payload can't read an arbitrary
    # relation, then resolve its (schema, name) for a safe Identifier.
    map_ident: sql.Identifier | None = None
    if mapping:
        match = next(
            (x for x in list_provenance_mappings(pool) if x["qname"] == mapping),
            None,
        )
        if match is None:
            raise ValueError(f"unknown provenance mapping: {mapping!r}")
        map_ident = sql.Identifier(match["schema"], match["name"])

    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            sql.SQL("SET LOCAL statement_timeout = {}").format(
                sql.Literal(statement_timeout)
            )
        )
        if (search_path or "").strip():
            target_path = compose_search_path(search_path, "")
        else:
            cur.execute("SHOW search_path")
            target_path = compose_search_path("", cur.fetchone()[0])
        cur.execute("SELECT set_config('search_path', %s, true)", (target_path,))
        apply_tool_search_path(cur, tool_search_path)
        for guc_name, guc_val in (extra_gucs or {}).items():
            if guc_name not in _EXTRA_GUC_WHITELIST:
                continue
            cur.execute(
                sql.SQL("SET LOCAL {} = {}").format(
                    sql.Identifier(*guc_name.split(".")),
                    sql.Literal(guc_val),
                )
            )

        cur.execute(
            sql.SQL(
                "SELECT variable::text, value "
                "FROM provsql.shapley_all_vars({tok}::uuid, {method}, {args}, {bz})"
            ).format(
                tok=sql.Literal(token),
                method=sql.Literal(m),
                args=sql.Literal(a),
                bz=sql.Literal(measure == "banzhaf"),
            )
        )
        rows = cur.fetchall()

        labels: dict[str, object] = {}
        if map_ident is not None:
            # Cast the mapping's value column to text for display; a single
            # variable can in principle appear more than once, last wins.
            cur.execute(
                sql.SQL(
                    "SELECT provenance::text, value::text FROM {}"
                ).format(map_ident)
            )
            labels = {prov: val for prov, val in cur.fetchall()}

    result = [
        {
            "variable": variable,
            "value": _to_jsonable(value),
            "label": labels.get(variable),
        }
        for variable, value in rows
    ]
    # Sort by contribution descending. `_to_jsonable` may stringify a
    # non-finite float ("NaN" / "Infinity"); treat any non-numeric (and
    # NULL) value as missing so it sinks to the end without a TypeError.
    def _sortkey(r):
        v = r["value"]
        numeric = isinstance(v, (int, float)) and not isinstance(v, bool)
        return (not numeric, -(v if numeric else 0.0))

    result.sort(key=_sortkey)
    return {
        "result": result,
        "kind": "contributions",
        "measure": measure,
        "method": m,
        "mapping": mapping,
    }


def list_temporal_relations(pool: ConnectionPool) -> list[dict]:
    """Candidate relations for Temporal mode's relation picker.

    The time-travel SRFs (`timeslice` / `timetravel` / `history`) run over any
    provenance-tracked relation -- a table with `add_provenance`, or a view /
    materialised view that projects such tables (CS4's `person_position`). We
    surface, from non-system / non-ProvSQL schemas, relations that either carry
    a `tstzmultirange` column (the validity convention) or are a view / matview.
    Returns `{schema, name, qname, display_name}`, qname-sorted."""
    q = """
        SELECT n.nspname AS schema, c.relname AS name
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname NOT IN ('pg_catalog', 'information_schema', 'provsql')
          AND n.nspname NOT LIKE 'pg_%'
          AND c.relkind IN ('r', 'v', 'm', 'p')
          AND (
            c.relkind IN ('v', 'm')
            OR EXISTS (
              SELECT 1 FROM pg_attribute a
              WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
                AND a.atttypid = 'pg_catalog.tstzmultirange'::regtype
            )
          )
        ORDER BY n.nspname, c.relname
    """
    out: list[dict] = []
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(q)
        for schema, name in cur.fetchall():
            qname = f"{schema}.{name}" if schema != "public" else name
            out.append(
                {"schema": schema, "name": name, "qname": qname, "display_name": qname}
            )
    return out


def list_temporal_mappings(pool: ConnectionPool) -> list[dict]:
    """Validity mappings for the `query` submode (sr_temporal's 2nd argument).

    A temporal mapping carries `(provenance uuid, value <multi>range)` -- the
    per-token validity. `create_provenance_mapping_view(... 'validity')` makes
    one; `provsql.time_validity_view` is the canonical union of all of them.
    Returns `{schema, name, qname, display_name}`, qname-sorted, with
    time_validity_view first when present (the usual choice)."""
    q = """
        SELECT n.nspname AS schema, c.relname AS name
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE c.relkind IN ('r', 'v', 'm')
          AND n.nspname NOT LIKE 'pg_%'
          AND EXISTS (SELECT 1 FROM pg_attribute a
                      WHERE a.attrelid = c.oid AND a.attname = 'provenance'
                        AND NOT a.attisdropped)
          AND EXISTS (SELECT 1 FROM pg_attribute a
                      WHERE a.attrelid = c.oid AND a.attname = 'value'
                        AND NOT a.attisdropped
                        AND format_type(a.atttypid, -1) LIKE '%%multirange')
        ORDER BY (c.relname = 'time_validity_view') DESC, n.nspname, c.relname
    """
    out: list[dict] = []
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(q)
        for schema, name in cur.fetchall():
            qname = f"{schema}.{name}"
            out.append(
                {"schema": schema, "name": name, "qname": qname, "display_name": qname}
            )
    return out


def _parse_multirange(value) -> list[dict]:
    """Normalise a `tstzmultirange` value to `[{lower, upper}, ...]`.

    psycopg returns it as a `Multirange` of `Range[datetime]`; each disjoint
    sub-range becomes one `{lower, upper}` entry (ISO-8601 strings, or None for
    an unbounded side) so the front-end can draw one timeline bar per range.
    Falls back to a light text parse if a driver hands back the raw literal."""
    if value is None:
        return []
    out: list[dict] = []
    try:
        for r in value:  # Multirange is iterable over its Range parts
            lo = getattr(r, "lower", None)
            hi = getattr(r, "upper", None)
            out.append(
                {
                    "lower": lo.isoformat() if hasattr(lo, "isoformat") else (
                        str(lo) if lo is not None else None),
                    "upper": hi.isoformat() if hasattr(hi, "isoformat") else (
                        str(hi) if hi is not None else None),
                }
            )
        return out
    except TypeError:
        pass
    # Fallback: parse `{["lo","hi"),...}` style text.
    text = str(value).strip().lstrip("{").rstrip("}")
    for part in re.findall(r"[\[(]([^,]*),([^\])]*)[\])]", text):
        lo = part[0].strip().strip('"') or None
        hi = part[1].strip().strip('"') or None
        out.append({"lower": lo, "upper": hi})
    return out


_TEMPORAL_TIMEOPS = ("asof", "during", "full")
# The canonical token->validity mapping ProvSQL ships (maintained by
# update_provenance); the default mapping for a relation source. The user may
# override it with any mapping in either source.
_TEMPORAL_DEFAULT_MAPPING = "provsql.time_validity_view"


def _resolve_temporal_mapping(
    pool: ConnectionPool, mapping: str | None, *, default: str | None = None
) -> str:
    """Validate a chosen validity mapping, falling back to `default`.

    Returns the canonical qname. A non-empty `mapping` must match a known
    mapping (else ValueError); an empty one yields `default`, or raises when no
    default is given (the query source has no canonical mapping)."""
    if (mapping or "").strip():
        m = next(
            (x for x in list_temporal_mappings(pool) if x["qname"] == mapping), None
        )
        if m is None:
            raise ValueError(f"unknown temporal mapping: {mapping!r}")
        return m["qname"]
    if default is None:
        raise ValueError("a validity mapping is required")
    return default


def temporal(
    pool: ConnectionPool,
    *,
    source: str,
    timeop: str,
    relation: str = "",
    query: str | None = None,
    mapping: str | None = None,
    at_time: str | None = None,
    from_time: str | None = None,
    to_time: str | None = None,
    statement_timeout: str,
    search_path: str = "",
    tool_search_path: str = "",
    extra_gucs: dict[str, str] | None = None,
) -> dict:
    """Place a relation's or a query's rows on a validity timeline.

    One uniform mechanism for both sources: the underlying SQL is wrapped with
    `sr_temporal(provenance(), mapping) AS valid_time`, and the time operation
    is applied as a filter on that computed column. (This supersedes ProvSQL's
    `timetravel` / `timeslice` / `history` SRFs, which remain the convenience
    surface for raw SQL.)

    `source` is `'relation'` (a tracked table / temporal view -- the SQL is
    `SELECT * FROM <relation>`, mapped through `mapping`, defaulting to the
    canonical `time_validity_view`) or `'query'` (arbitrary SQL, `mapping`
    required). In both sources the user may pick any validity mapping.
    `timeop` is the time operation:
      * `'asof'`   -- rows valid at a single instant (`at_time`)
      * `'during'` -- rows valid during a window (`from_time`..`to_time`)
      * `'full'`   -- every row, with its full validity (no time filter)
    Each `valid_time` is parsed into `[{lower, upper}]` intervals.

    Returns `{result, kind, source, timeop, columns, sql}` ready to
    JSON-encode. Raises ValueError on bad input; propagates psycopg.Error."""
    source = (source or "").strip().lower()
    timeop = (timeop or "").strip().lower()
    if source not in ("relation", "query"):
        raise ValueError(f"unknown temporal source: {source!r}")
    if timeop not in _TEMPORAL_TIMEOPS:
        raise ValueError(f"unknown temporal time operation: {timeop!r}")

    if source == "query":
        if not (query or "").strip():
            raise ValueError("query source requires a SQL query")
        # The query source has no canonical mapping (the SQL is arbitrary), so a
        # mapping must be chosen.
        map_qname = _resolve_temporal_mapping(pool, mapping)
    else:
        match = next(
            (x for x in list_temporal_relations(pool) if x["qname"] == relation), None
        )
        if match is None:
            raise ValueError(f"unknown temporal relation: {relation!r}")
        # time_validity_view is the default (it is the mapping update_provenance
        # maintains), but the user may select any mapping -- e.g. a single
        # table's own *_validity view.
        map_qname = _resolve_temporal_mapping(
            pool, mapping, default=_TEMPORAL_DEFAULT_MAPPING
        )

    if timeop == "asof":
        if not (at_time or "").strip():
            raise ValueError("the 'as of' operation requires an instant (at_time)")
    elif timeop == "during":
        if not (from_time or "").strip() or not (to_time or "").strip():
            raise ValueError("the 'during' operation requires from_time and to_time")

    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            sql.SQL("SET LOCAL statement_timeout = {}").format(
                sql.Literal(statement_timeout)
            )
        )
        if (search_path or "").strip():
            target_path = compose_search_path(search_path, "")
        else:
            cur.execute("SHOW search_path")
            target_path = compose_search_path("", cur.fetchone()[0])
        cur.execute("SELECT set_config('search_path', %s, true)", (target_path,))
        apply_tool_search_path(cur, tool_search_path)
        # Validity intervals are stored at UTC midnight (the temporal data
        # convention, cf. CS4); render them at UTC so the timeline gets clean
        # ISO instants regardless of the server's timezone.
        cur.execute("SET LOCAL timezone TO 'UTC'")
        for guc_name, guc_val in (extra_gucs or {}).items():
            cur.execute(
                sql.SQL("SET LOCAL {} = {}").format(
                    sql.Identifier(*guc_name.split(".")),
                    sql.Literal(guc_val),
                )
            )

        # One mechanism for both sources, mirroring the SRF bodies: project the
        # underlying columns plus sr_temporal AS `valid_time`, and apply the
        # time op as a base-level filter that RE-EVALUATES sr_temporal (not the
        # computed alias). Filtering at the same level as provenance() -- rather
        # than in an outer query over a tracked subquery -- is what keeps
        # ProvSQL's hook from mis-tracking the multirange operator on a table's
        # provsql column. ProvSQL appends a single `provsql` last, so the record
        # is (the underlying columns, valid_time, provsql), read off
        # cur.description.
        srt = sql.SQL("provsql.sr_temporal(provsql.provenance(), {m})").format(
            m=sql.Literal(map_qname)
        )
        if source == "query":
            star = sql.SQL("t0.*")
            from_clause = sql.SQL("(\n{q}\n  ) t0").format(
                q=sql.SQL((query or "").strip().rstrip(";"))
            )
        else:
            rel = sql.Identifier(match["schema"], match["name"])
            star = sql.SQL("{}.*").format(rel)
            from_clause = rel
        if timeop == "asof":
            where = sql.SQL("\nWHERE {srt} @> {at}::timestamptz").format(
                srt=srt, at=sql.Literal(at_time)
            )
        elif timeop == "during":
            where = sql.SQL(
                "\nWHERE {srt} && tstzrange({f}::timestamptz, {t}::timestamptz)"
            ).format(srt=srt, f=sql.Literal(from_time), t=sql.Literal(to_time))
        else:  # full
            where = sql.SQL("")
        full_query = sql.SQL(
            "SELECT {star},\n       {srt} AS valid_time\n  FROM {frm}{where}"
        ).format(star=star, srt=srt, frm=from_clause, where=where)
        cur.execute(full_query)
        rows = cur.fetchall()
        names_in_order = [d.name for d in cur.description]
        sql_text = full_query.as_string(conn)

        # Locate roles by name (shared by both paths). An untracked query may
        # yield no provsql column; tolerate that.
        valid_idx = names_in_order.index("valid_time")
        prov_idx = (
            names_in_order.index("provsql") if "provsql" in names_in_order else None
        )
        col_names_out = [
            n for n in names_in_order if n not in ("valid_time", "provsql")
        ]

    # Project each record by the by-name roles computed above: display columns
    # (everything but valid_time / provsql), the parsed validity, the token.
    display = [(i, n) for i, n in enumerate(names_in_order)
               if n not in ("valid_time", "provsql")]
    result = []
    for row in rows:
        entry = {
            n: (str(row[i]) if row[i] is not None else None) for i, n in display
        }
        entry["valid_time"] = _parse_multirange(row[valid_idx])
        entry["provsql"] = (
            str(row[prov_idx])
            if prov_idx is not None and row[prov_idx] is not None
            else None
        )
        result.append(entry)

    return {
        "result": result,
        "kind": "temporal",
        "source": source,
        "timeop": timeop,
        "relation": relation,
        "columns": col_names_out,
        "sql": sql_text,
    }


def _result_kind(semiring: str) -> str:
    if semiring in ("probability", "moment", "tropical", "viterbi", "lukasiewicz"):
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


# Dump-style data-loading statement: `COPY <table> ... FROM stdin ...;`
# sitting alone on its line, as pg_dump writes it. Matched both by
# app._split_statements (which carves the statement plus its inline data
# rows out of the batch as a single unit, dropping the `\.` terminator)
# and by _execute_statement below (which routes such a unit through the
# COPY sub-protocol -- cursor.execute() refuses COPY and, worse, leaves
# the connection wedged in COPY state).
COPY_FROM_STDIN_RE = re.compile(
    r"^\s*COPY\s+.*\bFROM\s+stdin\b.*?;?\s*$", re.IGNORECASE)


def _execute_statement(cur: psycopg.Cursor, stmt: str) -> None:
    """cursor.execute(), except for COPY-FROM-stdin units from the
    splitter (first line the COPY statement, remaining lines the raw
    data rows -- text or csv, the protocol does not care), which run
    through cursor.copy()."""
    head, _, data = stmt.partition("\n")
    if not COPY_FROM_STDIN_RE.match(head):
        cur.execute(stmt)
        return
    with cur.copy(head.strip().rstrip(";")) as copy:
        if data:
            copy.write(data if data.endswith("\n") else data + "\n")


def exec_batch(
    pool: ConnectionPool,
    statements: list[str],
    *,
    statement_timeout: str,
    where_provenance: bool,
    update_provenance: bool = False,
    boolean_provenance: bool = False,
    absorptive_provenance: bool = False,
    wrap_last: bool,
    extra_gucs: dict[str, str] | None = None,
    on_pid=None,
    search_path: str = "",
    tool_search_path: str = "",
    max_result_rows: int | None = None,
) -> tuple[list[StatementResult], StatementResult | None, dict]:
    """Run `statements` on a pooled connection, in a single transaction.

    Thin transaction-owning wrapper around :func:`exec_batch_on`, which
    documents the return shape: the pool's connection context commits
    on release, so one /api/exec request is one transaction. The
    notebook kernel path calls :func:`exec_batch_on` directly with its
    pinned connection instead, owning the per-cell transaction.
    """
    with pool.connection() as conn:
        try:
            return exec_batch_on(
                conn,
                statements,
                statement_timeout=statement_timeout,
                where_provenance=where_provenance,
                update_provenance=update_provenance,
                boolean_provenance=boolean_provenance,
                absorptive_provenance=absorptive_provenance,
                wrap_last=wrap_last,
                extra_gucs=extra_gucs,
                on_pid=on_pid,
                search_path=search_path,
                tool_search_path=tool_search_path,
                max_result_rows=max_result_rows,
            )
        finally:
            # Safety net: a statement that broke off mid-protocol (e.g.
            # a COPY TO STDOUT fed through execute()) leaves the
            # connection ACTIVE; the pool's COMMIT on release would then
            # die with "another command is already in progress", turning
            # the SQL error we just caught into an opaque 500. Close the
            # connection instead: Connection.__exit__ skips closed
            # connections, the pool discards and replaces it, and the
            # caught error result reaches the client. (The kernel path
            # has its own counterpart: it closes the kernel and reports
            # it dead.)
            try:
                if (not conn.closed
                        and conn.pgconn.transaction_status
                        == psycopg.pq.TransactionStatus.ACTIVE):
                    conn.close()
            except Exception:
                pass


def exec_batch_on(
    conn: psycopg.Connection,
    statements: list[str],
    *,
    statement_timeout: str,
    where_provenance: bool,
    update_provenance: bool = False,
    boolean_provenance: bool = False,
    absorptive_provenance: bool = False,
    wrap_last: bool,
    extra_gucs: dict[str, str] | None = None,
    on_pid=None,
    search_path: str = "",
    tool_search_path: str = "",
    max_result_rows: int | None = None,
) -> tuple[list[StatementResult], StatementResult | None, dict]:
    """Run `statements` on `conn` with SET LOCAL settings.

    Transaction boundaries belong to the caller: under
    :func:`exec_batch` the pooled-connection context commits on release
    (today's /api/exec semantics); the notebook kernel wraps each call
    in an explicit per-cell `conn.transaction()`. Either way the
    `SET LOCAL` prelude applies to exactly this batch. The caller is
    also responsible for discarding a connection left ACTIVE by a
    mid-protocol failure (see exec_batch's safety net).

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
            # The provenance class is a single enum GUC (extension >=
            # 1.10); the classes are mutually exclusive, so at most one
            # flag is set. Order from most to least specialised:
            # 'boolean' implies 'absorptive', which implies 'semiring';
            # 'where' is the semiring class plus cell-level tracking.
            cur.execute(
                "SET LOCAL provsql.provenance = "
                + (
                    "'boolean'"
                    if boolean_provenance
                    else "'where'" if where_provenance
                    else "'absorptive'" if absorptive_provenance
                    else "'semiring'"
                )
            )
            cur.execute(
                "SET LOCAL provsql.update_provenance = "
                + ("on" if update_provenance else "off")
            )
            # Studio always wants the classifier NOTICE for the
            # user's outermost SELECT so the result-pane header
            # can render the certified kind (TID / BID / OPAQUE)
            # as a prov-* badge. Older extension versions don't
            # know this GUC ; we wrap the SET LOCAL in a
            # SAVEPOINT so the "unrecognized configuration
            # parameter" error doesn't abort the surrounding
            # transaction (Studio still works against pre-1.6.0
            # servers, just without the badge).
            cur.execute("SAVEPOINT classify_guc")
            try:
                cur.execute(
                    "SET LOCAL provsql.classify_top_level = on"
                )
            except psycopg.errors.UndefinedObject:
                cur.execute("ROLLBACK TO SAVEPOINT classify_guc")
            else:
                cur.execute("RELEASE SAVEPOINT classify_guc")
            # provsql.tool_search_path: prepended to $PATH when
            # provsql spawns external tools (d4 / c2d / weightmc /
            # graph-easy). set_config parameterises the value so
            # the user-supplied portion never reaches PG as raw SQL.
            apply_tool_search_path(cur, tool_search_path)
            # Config-panel GUCs (provsql.active, provsql.verbose_level)
            # apply on top of the per-query toggles so the user can keep
            # the rewriter off via the panel without having to remember
            # it on every query.
            for guc_name, guc_val in (extra_gucs or {}).items():
                if guc_name not in _EXTRA_GUC_WHITELIST:
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
                    _execute_statement(cur, stmt)
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
            else:
                # No wrap (circuit mode or unwrappable last). The user's
                # query runs once with capture enabled : its planner-hook
                # notices describe the user's literal SQL.
                try:
                    _execute_statement(cur, last)
                except psycopg.Error as e:
                    return [], _user_error_result(e, meta, statement_timeout), meta

            # Post-processing pass : silence the notice handler so
            # Studio-internal lookups (the per-column `_type_name`
            # pg_type probe behind `_result_from_cursor`, the
            # `_resolve_agg_display` agg_token name resolution) don't
            # add their own classifier NOTICEs to the captured stream.
            # Each of those lookups is a `SELECT ... FROM <tbl>`
            # against an untracked catalog table, which the planner-
            # hook classifier (when on) would tag as
            # "TID (no provenance-tracked sources)". With Studio's
            # "last NOTICE wins" pill logic, a single user query that
            # introduced new column types would clobber the user's
            # OPAQUE / BID verdict with a stale TID until the per-
            # connection type-name cache warmed up.
            capture[0] = False
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
        # The connection outlives the batch (pool or pinned kernel), so
        # leaving the per-batch handler attached would accumulate one
        # handler per request.
        try:
            conn.remove_notice_handler(_on_notice)
        except Exception:
            pass

    return intermediate, final, meta


def open_kernel_connection(dsn: str | None) -> psycopg.Connection:
    """Open the pinned connection backing one notebook kernel.

    Same session defaults as the pool's connections
    (`configure_connection`), then flipped to autocommit so nothing
    dangles between cells: each cell runs in an explicit
    `conn.transaction()` inside `exec_kernel_cell`, and anything the
    user commits there (or any session-scoped object: temp tables,
    plain `SET`s, prepared statements) persists for the kernel's
    lifetime -- the Jupyter-like state the notebook mode is about."""
    conn = psycopg.connect(dsn or "")
    configure_connection(conn)
    conn.autocommit = True
    return conn


def exec_kernel_cell(
    conn: psycopg.Connection,
    statements: list[str],
    *,
    statement_timeout: str,
    where_provenance: bool,
    update_provenance: bool = False,
    boolean_provenance: bool = False,
    absorptive_provenance: bool = False,
    wrap_last: bool,
    extra_gucs: dict[str, str] | None = None,
    search_path: str = "",
    tool_search_path: str = "",
    max_result_rows: int | None = None,
) -> tuple[list[StatementResult], StatementResult | None, dict, bool]:
    """Run one notebook cell on a pinned kernel connection.

    The cell executes inside its own transaction, preserving
    `exec_batch_on`'s within-batch semantics verbatim (SET LOCAL
    prelude, classifier savepoint, where-wrap, COPY blocks,
    halt-on-first-error); a failed cell rolls back cleanly
    (psycopg's transaction block turns the INERROR state into a
    silent rollback) while committed cells persist for later ones.

    Returns `(intermediate, final, meta, kernel_dead)`. `kernel_dead`
    flags a connection left unusable -- wedged mid-protocol (e.g.
    `COPY ... TO STDOUT` through execute()), broken socket, server
    restart; the connection is closed here and the caller must
    discard the kernel. A COMMIT-time SQL failure on a healthy
    connection (e.g. a deferred constraint firing) is *not* death:
    the transaction rolled back and the kernel lives on."""
    result = None
    try:
        with conn.transaction():
            result = exec_batch_on(
                conn,
                statements,
                statement_timeout=statement_timeout,
                where_provenance=where_provenance,
                update_provenance=update_provenance,
                boolean_provenance=boolean_provenance,
                absorptive_provenance=absorptive_provenance,
                wrap_last=wrap_last,
                extra_gucs=extra_gucs,
                search_path=search_path,
                tool_search_path=tool_search_path,
                max_result_rows=max_result_rows,
            )
    except psycopg.Error as e:
        # Raised at the transaction boundary. Distinguish a wedged /
        # broken connection from a legitimate COMMIT-time failure by
        # the connection's state.
        dead = True
        try:
            dead = (conn.closed or conn.broken
                    or conn.pgconn.transaction_status
                    == psycopg.pq.TransactionStatus.ACTIVE)
        except Exception:
            pass
        if dead:
            try:
                conn.close()
            except Exception:
                pass
        if result is not None:
            intermediate, final, meta = result
        else:
            intermediate, final = [], None
            meta = {"wrapped": False, "notices": []}
        # Keep the cell's own error block when there is one (the
        # commit failure is its consequence, not the story); surface
        # the boundary error otherwise.
        has_cell_error = (
            (final is not None and final.kind == "error")
            or any(r.kind == "error" for r in intermediate)
        )
        if not has_cell_error:
            final = _user_error_result(e, meta, statement_timeout)
        return intermediate, final, meta, dead
    intermediate, final, meta = result
    return intermediate, final, meta, False


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
# per-query through the form toggles (the provenance class,
# update_provenance) and those managed by the Config panel (active,
# verbose_level). The split matters because the two groups are applied at
# different points in exec_batch, and the panel must not expose the
# toggle GUCs (they would silently override what the user picks per
# query).
_TOGGLE_GUCS = {
    "provsql.provenance",
    "provsql.update_provenance",
}
_PANEL_GUCS = {
    "provsql.active",
    "provsql.verbose_level",
    "provsql.monte_carlo_seed",
    "provsql.rv_mc_samples",
    "provsql.simplify_on_load",
    "provsql.hybrid_evaluation",
    "provsql.fallback_compiler",
}

# Session-sticky modes the app keeps in app.config["SESSION_MODES"] and
# injects into every backend call's extra_gucs.  Distinct from
# _PANEL_GUCS so they stay invisible to the Config panel API ; the
# Boolean-mode selector in the toolbar owns the lifecycle.  Allowed
# through the same SET LOCAL pipeline as the panel GUCs.
_SESSION_MODE_GUCS = {
    "provsql.provenance",
}
# Every key accepted as an `extra_gucs` payload (the union the
# downstream filter functions check).
_EXTRA_GUC_WHITELIST = _PANEL_GUCS | _SESSION_MODE_GUCS
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


def validate_panel_guc(
    name: str, value: str, fallback_compilers: frozenset[str] | None = None
) -> str:
    """Validate (name, value) for the Config panel and return the canonical
    value as it should be stored. Raises ValueError on rejection.

    ``fallback_compilers``, when given, is the live set of registered
    compile-tool names (from provsql.tools) used to validate
    ``provsql.fallback_compiler``; when ``None`` the value is accepted as
    long as it is non-empty (the catalog could not be consulted)."""
    if name not in _PANEL_GUCS:
        raise ValueError(f"GUC not user-configurable: {name}")
    v = (value or "").strip().lower()
    if name == "provsql.active":
        if v in ("on", "true", "1", "yes"):
            return "on"
        if v in ("off", "false", "0", "no"):
            return "off"
        raise ValueError("provsql.active must be on or off")
    if name == "provsql.verbose_level":
        try:
            n = int(v)
        except (TypeError, ValueError):
            raise ValueError("provsql.verbose_level must be an integer 0..100")
        if not (0 <= n <= 100):
            raise ValueError("provsql.verbose_level must be between 0 and 100")
        return str(n)
    if name == "provsql.monte_carlo_seed":
        # -1 means "seed from std::random_device" per src/provsql.c; any
        # other int (including 0) is a literal seed for the mt19937_64
        # used by the Bernoulli and gate_rv sampling paths.
        try:
            n = int(v)
        except (TypeError, ValueError):
            raise ValueError("provsql.monte_carlo_seed must be an integer (-1 for non-deterministic)")
        if n < -1:
            raise ValueError("provsql.monte_carlo_seed must be -1 or a non-negative integer")
        return str(n)
    if name == "provsql.rv_mc_samples":
        try:
            n = int(v)
        except (TypeError, ValueError):
            raise ValueError("provsql.rv_mc_samples must be a non-negative integer")
        if n < 0:
            raise ValueError("provsql.rv_mc_samples must be non-negative (0 disables the MC fallback)")
        return str(n)
    if name in ("provsql.simplify_on_load", "provsql.hybrid_evaluation"):
        if v in ("on", "true", "1", "yes"):
            return "on"
        if v in ("off", "false", "0", "no"):
            return "off"
        raise ValueError(f"{name} must be on or off")
    if name == "provsql.fallback_compiler":
        # Validate against the live registry (the caller passes the
        # registered compile-tool names from provsql.tools), not a
        # hardcoded list, so an admin-registered compiler is accepted.
        # Compare the raw value, not the lower-cased v: the SQL GUC takes
        # whatever string verbatim and tool names are already lowercase.
        raw = (value or "").strip()
        if not raw:
            raise ValueError("provsql.fallback_compiler must not be empty")
        if fallback_compilers is not None and raw not in fallback_compilers:
            raise ValueError(
                "provsql.fallback_compiler must be one of: "
                + ", ".join(sorted(fallback_compilers))
            )
        return raw
    raise ValueError(f"GUC not user-configurable: {name}")


def set_guc(pool: ConnectionPool, name: str, value: str) -> None:
    """Legacy, no longer wired by the API: kept for callers that may still
    set a session GUC on a transient connection. Raises ValueError if
    `name` is not whitelisted."""
    if name not in _GUC_WHITELIST:
        raise ValueError(f"GUC not whitelisted: {name}")
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(f"SET {name} = %s", (value,))
