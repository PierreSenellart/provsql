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
from pyodide.ffi import JsProxy, run_sync, to_js


def _to_py(v):
    # PGlite returns jsonb / array columns as JS objects/arrays, which reach
    # Python as JsProxy. Convert them to native dict/list so the values are
    # JSON-serialisable (jsonify) and behave like real psycopg results.
    return v.to_py() if isinstance(v, JsProxy) else v

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
    rows = [tuple(_to_py(getattr(r, n)) for n, _ in fields) for r in res.rows]
    return fields, rows, res.affected, notices


def _prepare(q, params):
    """Translate psycopg placeholders to PGlite's positional $1,$2,...

    Returns (sql, ordered_params). Supports both styles db.py / circuit.py /
    kc.py use: positional `%s` with a tuple/list, and named `%(name)s` with a
    dict. A repeated `%(name)s` is emitted as a fresh $n bound to the same
    value (PGlite has no named binds), which is functionally equivalent."""
    if not params:
        return q, None
    if isinstance(params, dict):
        order = []

        def repl(m):
            order.append(params[m.group(1)])
            return "$" + str(len(order))

        return _re.sub(r"%\(([^)]+)\)s", repl, q), order

    seq = list(params)
    i = [0]

    def repl(_):
        i[0] += 1
        return "$" + str(i[0])

    return _re.sub(r"%s", repl, q), seq


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
        qsql, qparams = _prepare(s, params)
        if not _TXCTRL.match(s):
            self._c._ensure_tx()
        try:
            f, rows, aff, notices = _run(qsql, qparams)
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


class _Info:
    """Stand-in for psycopg's `conn.info`; server_version and dbname (the
    kernel-session reply reports which database the kernel is pinned to)
    are the only fields consumed."""

    def __init__(self):
        self._sv = None
        self._db = None

    @property
    def server_version(self):
        if self._sv is None:
            try:
                _, rows, _, _ = _run("SHOW server_version_num", None)
                self._sv = int(rows[0][0])
            except Exception:
                self._sv = 170000
        return self._sv

    @property
    def dbname(self):
        if self._db is None:
            try:
                _, rows, _, _ = _run("SELECT current_database()", None)
                self._db = rows[0][0]
            except Exception:
                self._db = ""
        return self._db

    @property
    def backend_pid(self):
        # One single-process backend; the kernel chip and the cancel
        # endpoint only display / compare this.
        try:
            _, rows, _, _ = _run("SELECT pg_backend_pid()", None)
            return rows[0][0]
        except Exception:
            return 0


class _Tx:
    """psycopg-style `conn.transaction()` context manager.

    On a non-autocommit connection it maps onto a SAVEPOINT inside the
    connection's lazy transaction (the pool-request shape db.py uses;
    force_rollback=True always undoes the block, used for read-only
    probes).  On an AUTOCOMMIT connection -- the pinned notebook-kernel
    connections -- psycopg's transaction() opens a real transaction and
    commits/rolls back at block exit; mirror that, otherwise the
    SAVEPOINT would run outside any transaction and each cell would
    silently lose its all-or-nothing semantics."""

    _n = [0]

    def __init__(self, conn, force_rollback=False):
        self._conn = conn
        self._force = force_rollback
        self._sp = None
        self._own = False

    def __enter__(self):
        if self._conn.autocommit and not self._conn._in_tx:
            self._own = True
            self._conn._raw("BEGIN")
            self._conn._in_tx = True
            return self
        self._conn._ensure_tx()
        _Tx._n[0] += 1
        self._sp = "psy_sp_%d" % _Tx._n[0]
        self._conn._raw("SAVEPOINT " + self._sp)
        return self

    def __exit__(self, exc_t, exc_v, tb):
        if self._own:
            rollback = exc_t is not None or self._force
            self._conn._raw("ROLLBACK" if rollback else "COMMIT")
            self._conn._in_tx = False
            return False
        if exc_t is not None or self._force:
            self._conn._raw("ROLLBACK TO SAVEPOINT " + self._sp)
        self._conn._raw("RELEASE SAVEPOINT " + self._sp)
        return False


class _PGconn:
    """Stand-in for psycopg's `conn.pgconn`; db.py's kernel-death check
    reads transaction_status to tell a wedged connection (ACTIVE, stuck
    mid-protocol) from a healthy post-rollback one.  The shim is never
    wedged -- every query completes synchronously -- so report IDLE."""

    @property
    def transaction_status(self):
        return psycopg.pq.TransactionStatus.IDLE


class _Conn:
    def __init__(self):
        self.autocommit = False
        self.closed = False
        self.broken = False
        self.prepare_threshold = None
        self._in_tx = False
        self._handlers = []
        self.info = _Info()
        self.pgconn = _PGconn()

    def add_notice_handler(self, cb):
        self._handlers.append(cb)

    def remove_notice_handler(self, cb):
        try:
            self._handlers.remove(cb)
        except ValueError:
            pass

    def transaction(self, savepoint_name=None, force_rollback=False):
        return _Tx(self, force_rollback)

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
        """Kernel restart / close, mapped onto the one shared session.

        The native build closes the pinned kernel connection and opens a
        fresh one; PGlite has a single backend session shared by every
        connection object, so 'fresh' is expressed as DISCARD ALL: temp
        tables, prepared statements and session GUCs go away -- including
        the search_path the shell's per-open PREP set, which is restored
        here (it must match shell-boot.js's PREP).  Single-session
        caveat: kernel state is shared across notebook tabs, so closing
        or restarting any kernel resets them all."""
        if self.closed:
            return
        if self._in_tx:
            self._raw("ROLLBACK")
            self._in_tx = False
        self._raw("DISCARD ALL")
        self._raw("SET search_path TO public, provsql")
        self.closed = True


def _connect(*a, **k):
    return _Conn()


psycopg.connect = _connect

_errors = types.ModuleType("psycopg.errors")
for _n in ("UndefinedFunction", "UndefinedObject", "InsufficientPrivilege", "SyntaxError"):
    setattr(_errors, _n, type(_n, (psycopg.Error,), {}))
psycopg.errors = _errors

# psycopg.pq.TransactionStatus: db.py's kernel-death check compares
# conn.pgconn.transaction_status against ACTIVE (wedged mid-protocol).
_pq = types.ModuleType("psycopg.pq")


class _TransactionStatus:
    IDLE, ACTIVE, INTRANS, INERROR, UNKNOWN = range(5)


_pq.TransactionStatus = _TransactionStatus
psycopg.pq = _pq

_sql = types.ModuleType("psycopg.sql")


def _as_str(x, ctx=None):
    return x.as_string(ctx) if hasattr(x, "as_string") else str(x)


class _Composable:
    # psycopg composables concatenate with `+` into a Composed.
    def __add__(self, other):
        return Composed([self, other])


class Composed(_Composable):
    def __init__(self, parts):
        self.parts = list(parts)

    def __add__(self, other):
        return Composed(self.parts + [other])

    def as_string(self, ctx=None):
        return "".join(_as_str(p, ctx) for p in self.parts)


class SQL(_Composable):
    def __init__(self, s):
        self.s = s

    def format(self, *args, **kw):
        out = self.s
        for a in args:
            out = out.replace("{}", _as_str(a), 1)
        for k, v in kw.items():
            out = out.replace("{" + k + "}", _as_str(v))
        return SQL(out)

    def join(self, parts):
        return SQL(self.s.join(_as_str(p) for p in parts))

    def as_string(self, ctx=None):
        return self.s


class Identifier(_Composable):
    def __init__(self, *p):
        self.p = p

    def as_string(self, ctx=None):
        return ".".join('"' + x.replace('"', '""') + '"' for x in self.p)


class Literal(_Composable):
    def __init__(self, v):
        self.v = v

    def as_string(self, ctx=None):
        if self.v is None:
            return "NULL"
        if isinstance(self.v, (int, float)):
            return str(self.v)
        return "'" + str(self.v).replace("'", "''") + "'"


_sql.SQL, _sql.Identifier, _sql.Literal, _sql.Composed = SQL, Identifier, Literal, Composed
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

# Fake `subprocess`: circuit.py shells out to `dot -Tjson` for graph layout.
# In WASM there are no processes, so route `dot` to the page's WASM Graphviz
# (js global `graphvizDot`) and report any other external tool (the optional
# knowledge compilers) as absent, which the registry-driven pickers tolerate.
_subprocess = types.ModuleType("subprocess")
_subprocess.PIPE, _subprocess.STDOUT, _subprocess.DEVNULL = -1, -2, -3


class CalledProcessError(Exception):
    def __init__(self, returncode, cmd, output=None, stderr=None):
        self.returncode, self.cmd = returncode, cmd
        self.output = self.stdout = output
        self.stderr = stderr
        super().__init__("Command %r returned non-zero exit status %d." % (cmd, returncode))


class CompletedProcess:
    def __init__(self, args, returncode, stdout=None, stderr=None):
        self.args, self.returncode, self.stdout, self.stderr = args, returncode, stdout, stderr

    def check_returncode(self):
        if self.returncode:
            raise CalledProcessError(self.returncode, self.args, self.stdout, self.stderr)


def _sp_run(args, input=None, capture_output=False, text=False, check=False, **kw):
    argv = list(args) if isinstance(args, (list, tuple)) else str(args).split()
    prog = argv[0] if argv else ""
    if prog == "dot":
        fmt = "json"
        for a in argv[1:]:
            if isinstance(a, str) and a.startswith("-T"):
                fmt = a[2:] or "json"
        import js
        out = run_sync(js.graphvizDot(input or "", fmt))
        return CompletedProcess(args, 0, stdout=out, stderr="")
    raise FileNotFoundError(2, "No such file or directory: %r" % prog)


def _sp_popen(*a, **k):
    raise NotImplementedError("subprocess.Popen is unavailable in the browser build")


_subprocess.run = _sp_run
_subprocess.Popen = _sp_popen
_subprocess.CalledProcessError = CalledProcessError
_subprocess.CompletedProcess = CompletedProcess

for _name, _mod in [
    ("psycopg", psycopg),
    ("psycopg.errors", _errors),
    ("psycopg.pq", _pq),
    ("psycopg.sql", _sql),
    ("psycopg.conninfo", _conninfo),
    ("psycopg_pool", _pool),
    ("subprocess", _subprocess),
]:
    sys.modules[_name] = _mod
