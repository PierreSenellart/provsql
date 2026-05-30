# A minimal psycopg / psycopg_pool stand-in backed by an in-page PGlite,
# registered into sys.modules so the UNMODIFIED provsql_studio Python
# (app.py / db.py / circuit.py) runs in Pyodide against WASM Postgres.
#
# It covers exactly the surface db.py uses: ConnectionPool.connection() ->
# conn.cursor() -> execute / fetch{all,one,many} / description (Column with
# .name/.type_code) / rowcount / statusmessage; sql.SQL / Identifier /
# Literal; psycopg.errors.*; add_notice_handler; SET LOCAL inside a lazy
# transaction; rollback/commit.  cursor.execute synchronously awaits the
# async PGlite via run_sync (requires a callPromising entry / JSPI).
#
# Requires a JS global `pgQuery(sql, params)` -> Promise of
#   {ok, rows, fields:[{name,dataTypeID}], affected, message,
#    notices:[{severity,message_primary,message_detail,message_hint}]}.
import re as _re
import sys
import types

from js import pgQuery
from pyodide.ffi import run_sync, to_js

psycopg = types.ModuleType("psycopg")
psycopg.Error = type("Error", (Exception,), {})


class _Diag:
    def __init__(self, d):
        self.severity = d.severity
        self.message_primary = d.message_primary
        self.message_detail = d.message_detail
        self.message_hint = d.message_hint


def _run(sql_str, params):
    res = run_sync(pgQuery(sql_str, to_js(params) if params else None))
    notices = [_Diag(n) for n in res.notices] if res.notices else []
    if not res.ok:
        e = psycopg.Error(res.message)
        e._notices = notices
        raise e
    fields = [(f.name, f.dataTypeID) for f in res.fields] if res.fields else []
    rows = [tuple(getattr(r, n) for n, _ in fields) for r in res.rows]
    return fields, rows, res.affected, notices


def _xlate(q, params):
    """psycopg %s placeholders -> PGlite $1, $2, ..."""
    if not params:
        return q
    i = [0]

    def repl(_):
        i[0] += 1
        return "$" + str(i[0])

    return _re.sub(r"%s", repl, q)


class _Col(tuple):
    def __new__(cls, name, oid):
        return super().__new__(cls, (name, oid, None, None, None, None, None))

    def __init__(self, name, oid):
        self.name = name
        self.type_code = oid


_TXCTRL = _re.compile(r"^\s*(BEGIN|START|COMMIT|ROLLBACK|END)\b", _re.I)


class _Cursor:
    def __init__(self, conn):
        self._c = conn
        self.connection = conn
        self.description = None
        self.rowcount = -1
        self.statusmessage = "OK"
        self._rows = []
        self._i = 0

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def execute(self, query, params=None):
        s = query if isinstance(query, str) else query.as_string(None)
        if not _TXCTRL.match(s):
            self._c._ensure_tx()
        try:
            f, rows, aff, notices = _run(_xlate(s, params), params)
        except psycopg.Error as e:
            self._c._dispatch(getattr(e, "_notices", []))
            raise
        self.description = [_Col(n, o) for n, o in f] if f else None
        self._rows, self._i = rows, 0
        self.rowcount = len(rows) if f else aff
        self._c._dispatch(notices)

    def fetchall(self):
        r = self._rows[self._i:]
        self._i = len(self._rows)
        return r

    def fetchone(self):
        if self._i < len(self._rows):
            r = self._rows[self._i]
            self._i += 1
            return r
        return None

    def fetchmany(self, n=1):
        r = self._rows[self._i:self._i + n]
        self._i += n
        return r

    def __iter__(self):
        return iter(self.fetchall())


class _Conn:
    def __init__(self):
        self.autocommit = False
        self._in_tx = False
        self._handlers = []

    def add_notice_handler(self, cb):
        self._handlers.append(cb)

    def _dispatch(self, notices):
        for n in notices:
            for cb in self._handlers:
                try:
                    cb(n)
                except Exception:
                    pass

    def _raw(self, s):
        try:
            _run(s, None)
        except Exception:
            pass

    def _ensure_tx(self):
        if not self.autocommit and not self._in_tx:
            self._raw("BEGIN")
            self._in_tx = True

    def cursor(self, *a, **k):
        return _Cursor(self)

    def execute(self, q, p=None):
        cur = _Cursor(self)
        cur.execute(q, p)
        return cur

    def __enter__(self):
        return self

    def __exit__(self, exc_t, *a):
        if self._in_tx:
            self._raw("ROLLBACK" if exc_t else "COMMIT")
            self._in_tx = False
        return False

    def rollback(self):
        if self._in_tx:
            self._raw("ROLLBACK")
            self._in_tx = False

    def commit(self):
        if self._in_tx:
            self._raw("COMMIT")
            self._in_tx = False

    def close(self):
        pass


def _connect(*a, **k):
    return _Conn()


psycopg.connect = _connect

_errors = types.ModuleType("psycopg.errors")
for _n in ("UndefinedFunction", "UndefinedObject", "InsufficientPrivilege", "SyntaxError"):
    setattr(_errors, _n, type(_n, (psycopg.Error,), {}))
psycopg.errors = _errors

_sql = types.ModuleType("psycopg.sql")


class SQL:
    def __init__(self, s):
        self.s = s

    def format(self, *args, **kw):
        out = self.s
        for a in args:
            out = out.replace("{}", a.as_string(None) if hasattr(a, "as_string") else str(a), 1)
        for k, v in kw.items():
            out = out.replace("{" + k + "}", v.as_string(None) if hasattr(v, "as_string") else str(v))
        return SQL(out)

    def join(self, parts):
        return SQL(self.s.join(p.as_string(None) if hasattr(p, "as_string") else str(p) for p in parts))

    def as_string(self, ctx=None):
        return self.s


class Identifier:
    def __init__(self, *p):
        self.p = p

    def as_string(self, ctx=None):
        return ".".join('"' + x.replace('"', '""') + '"' for x in self.p)


class Literal:
    def __init__(self, v):
        self.v = v

    def as_string(self, ctx=None):
        if self.v is None:
            return "NULL"
        if isinstance(self.v, (int, float)):
            return str(self.v)
        return "'" + str(self.v).replace("'", "''") + "'"


_sql.SQL, _sql.Identifier, _sql.Literal = SQL, Identifier, Literal
psycopg.sql = _sql

_conninfo = types.ModuleType("psycopg.conninfo")
_conninfo.conninfo_to_dict = lambda s="": {}
_conninfo.make_conninfo = lambda *a, **k: ""
psycopg.conninfo = _conninfo

_pool = types.ModuleType("psycopg_pool")


class ConnectionPool:
    def __init__(self, *a, **k):
        pass

    def connection(self, *a, **k):
        return _Conn()

    def close(self):
        pass


_pool.ConnectionPool = ConnectionPool

for _name, _mod in [
    ("psycopg", psycopg),
    ("psycopg.errors", _errors),
    ("psycopg.sql", _sql),
    ("psycopg.conninfo", _conninfo),
    ("psycopg_pool", _pool),
]:
    sys.modules[_name] = _mod
