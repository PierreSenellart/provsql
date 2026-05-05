"""psycopg helpers: pool, statement-timeout-bounded execution, relation discovery."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Iterator

import psycopg
from psycopg import sql
from psycopg_pool import ConnectionPool


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
    """Open a connection pool. dsn=None means psycopg picks up PG* env vars."""
    return ConnectionPool(conninfo=dsn or "", min_size=1, max_size=8, open=True)


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
        return [], None, {"wrapped": False, "notice": None}

    *prelude, last = statements
    intermediate: list[StatementResult] = []
    meta: dict = {"wrapped": wrap_last, "notice": None}

    with pool.connection() as conn:
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

                # Run prelude statements; halt on first error.
                for stmt in prelude:
                    try:
                        cur.execute(stmt)
                    except psycopg.Error as e:
                        intermediate.append(_error_result(e))
                        return intermediate, None, meta

                wrapped_sql = (
                    f"SELECT *, "
                    f"provsql.provenance() AS __prov, "
                    f"provsql.where_provenance(provsql.provenance()) AS __wprov "
                    f"FROM ({last}) t"
                )

                if wrap_last:
                    # Try the wrapped form first under a savepoint so we can
                    # roll back and retry unwrapped if the user's query
                    # touches no provenance-tracked relation. The provsql
                    # backend raises a specific error in that case
                    # ("provenance() called on a table without provenance");
                    # any other error propagates.
                    cur.execute("SAVEPOINT before_wrap")
                    try:
                        cur.execute(wrapped_sql)
                    except psycopg.Error as e:
                        if _NO_PROV_MARKER in str(e):
                            cur.execute("ROLLBACK TO SAVEPOINT before_wrap")
                            meta["wrapped"] = False
                            meta["notice"] = (
                                "Source relation is not provenance-tracked; "
                                "where-provenance highlights are unavailable. "
                                "Run “SELECT add_provenance('…')” to enable."
                            )
                            try:
                                cur.execute(last)
                            except psycopg.Error as e2:
                                return [], _error_result(e2), meta
                        else:
                            return [], _error_result(e), meta
                else:
                    try:
                        cur.execute(last)
                    except psycopg.Error as e:
                        return [], _error_result(e), meta

                final = _result_from_cursor(cur)
        except psycopg.Error as e:
            return [], _error_result(e), meta

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


_GUC_WHITELIST = {
    "provsql.active",
    "provsql.where_provenance",
    "provsql.update_provenance",
    "provsql.verbose_level",
}


def get_gucs(pool: ConnectionPool, names: list[str]) -> dict[str, str]:
    """Read the named GUCs via SHOW. `names` must be a subset of the whitelist."""
    out: dict[str, str] = {}
    with pool.connection() as conn, conn.cursor() as cur:
        for name in names:
            if name not in _GUC_WHITELIST:
                continue
            try:
                cur.execute(f"SHOW {name}")  # whitelisted, safe
                out[name] = cur.fetchone()[0]
            except psycopg.Error:
                out[name] = ""
    return out


def set_guc(pool: ConnectionPool, name: str, value: str) -> None:
    """SET a whitelisted GUC. Raises ValueError if `name` is not whitelisted."""
    if name not in _GUC_WHITELIST:
        raise ValueError(f"GUC not whitelisted: {name}")
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(f"SET {name} = %s", (value,))
