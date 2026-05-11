"""Server-side circuit-DAG layout for /api/circuit/<uuid>.

Pulls the BFS subgraph of a provenance circuit via `provsql.circuit_subgraph`,
hands a DOT description to `dot -Tjson` for layered top-down placement, and
returns a `{nodes, edges}` JSON shape ready for the SVG renderer in the
front-end. The layout is bound by `--max-circuit-depth` (BFS depth) and
`--max-circuit-nodes` (response cap, rejected with 413 when exceeded).

`circuit_subgraph` returns one row per (parent, node) edge : provenance
circuits are DAGs, so a node with k parents within the bound is reported
k times. We dedup by node id to build the `nodes` list and treat every
row with a non-null parent as an edge.
"""
from __future__ import annotations

import json
import subprocess
from collections import OrderedDict

from psycopg_pool import ConnectionPool

# Gate-type label glyphs. Stage 3 only consumes the structural shape; richer
# rendering (info1/info2 callouts, leaf row previews) lives in the front-end.
_GATE_LABEL = {
    "input":    "ι",
    "plus":     "⊕",
    "times":    "⊗",
    "monus":    "⊖",
    "project":  "Π",
    "zero":     "𝟘",
    "one":      "𝟙",
    "eq":       "⋈",
    "agg":      "Σ",
    "semimod":  "⋆",
    "cmp":      "≷",
    "delta":    "δ",
    "value":    "v",
    "mulinput": "⋮",
    "update":   "υ",
    "rv":       "ξ",
    "arith":    "α",
    "mixture":  "Mix",
}

# PROVSQL_ARITH_* enum tags (src/provsql_utils.h) → in-circle glyph. The
# unary-minus tag is rendered as a bare minus: with a single child the
# context is unambiguous, and adding "x" would burn precious circle
# real estate.
_ARITH_OP_GLYPH = {
    0: "+",
    1: "×",
    2: "−",
    3: "÷",
    4: "−",
}

# Distribution-kind initials used in the in-circle label for gate_rv.
# Same logic as gate_value: the full encoding lives in `extra`; the
# circle just needs a glance-recognisable hint.
_RV_KIND_INITIAL = {
    "normal":      "N",
    "uniform":     "U",
    "exponential": "Exp",
    "erlang":      "Erl",
}


def _gate_label(row: dict) -> str:
    """Resolve the in-circle glyph for a row from the BFS subgraph.

    For most gate types this is the static map above. Three gates carry
    payload that's more informative than a generic glyph:
      * agg: info1_name is the aggregate function's `proname` (e.g. "sum")
      * cmp: info1_name is the operator's `oprname` (e.g. ">", "<>")
      * value: extra is the scalar text-encoded by `set_extra`
    Truncate so anything longer than the node circle can fit collapses to
    "<n chars>…"; the full payload is still available in the inspector
    via the info1 / extra fields.
    """
    t = row["gate_type"]
    if t == "agg" and row.get("info1_name"):
        # SQL convention: aggregate functions are written in uppercase
        # (SUM, COUNT, AVG…) even though pg_proc.proname stores them
        # lowercase. SUM is special-cased to its math glyph Σ since the
        # symbol is universally understood and saves precious circle
        # real estate.
        name = row["info1_name"].upper()
        return "Σ" if name == "SUM" else _truncate(name)
    if t == "cmp" and row.get("info1_name"):
        # PostgreSQL stores comparison operators in pg_operator under their
        # ASCII names ("<=", ">=", "<>"). Render the math glyphs in the
        # circle so the eye picks them up faster, mirroring what Fira Code
        # does for the same operators in the editor.
        return _truncate(_CMP_GLYPH.get(row["info1_name"], row["info1_name"]))
    if t == "value" and row.get("extra"):
        return row["extra"]
    if t == "rv" and row.get("extra"):
        return _format_rv_label(row["extra"])
    if t == "arith":
        # circuit_subgraph returns info1 as TEXT (uniform-typed column); coerce
        # to int before the enum-tag lookup. Anything unparseable falls
        # through to the generic 'α' glyph below.
        try:
            tag = int(row["info1"]) if row.get("info1") is not None else None
        except (TypeError, ValueError):
            tag = None
        glyph = _ARITH_OP_GLYPH.get(tag)
        if glyph is not None:
            return glyph
    return _GATE_LABEL.get(t, t)


def _format_rv_label(extra: str) -> str:
    """Render the in-circle label for a gate_rv leaf from its extra text.

    Extra is "<kind>:<p1>[,<p2>]" (see src/RandomVariable.{h,cpp}). We
    return the full kind-initial + parenthesised parameter list (e.g.
    "N(2.5,0.5)") without truncating; the front-end shrinks the
    font-size to fit so the user keeps the full parameter list rather
    than seeing an ellipsis. The full text is still surfaced by the
    inspector under the `distribution` row.
    """
    s = str(extra).strip()
    kind, _, params = s.partition(":")
    label = _RV_KIND_INITIAL.get(kind.strip().lower())
    if label is None:
        return s
    p = params.strip()
    return f"{label}({p})" if p else label


def _truncate(s: str, n: int = 6) -> str:
    s = str(s)
    return s if len(s) <= n else s[: n - 1] + "…"


# pg_operator.oprname → math glyph for the cmp gate label.
_CMP_GLYPH = {
    ">=": "≥",
    "<=": "≤",
    "<>": "≠",
    "!=": "≠",
}


class _SimplifiedNotAvailable(Exception):
    """Raised by _fetch_subgraph when the running provsql lacks
    `simplified_circuit_subgraph` (older extension version)."""


class CircuitTooLarge(Exception):
    def __init__(self, node_count: int, cap: int, depth: int, depth_1_size: int):
        super().__init__(f"circuit too large: {node_count} > {cap}")
        self.node_count = node_count
        self.cap = cap
        self.depth = depth
        # Size of the BFS-frontier at depth 1 (root + direct children).
        # The front-end uses this to decide whether to offer a "Render
        # at depth 1" retry: meaningful only when this value fits under
        # the cap (otherwise the circuit is wide-bound and reducing
        # depth wouldn't help).
        self.depth_1_size = depth_1_size


def get_circuit(
    pool: ConnectionPool,
    *,
    root: str,
    depth: int,
    max_nodes: int,
    simplified: bool = True,
    extra_gucs: dict[str, str] | None = None,
) -> dict:
    """Return `{nodes, edges, root, depth}` for the BFS-bounded subgraph rooted
    at `root` (a UUID-formatted string). Raises CircuitTooLarge if the cap is
    exceeded so the route can translate to HTTP 413.

    Frontier flag: we ask circuit_subgraph for `depth + 1` and then discard
    the deepest layer, but use the fact that any node with a depth-`depth+1`
    child is a frontier (i.e. it has at least one outgoing edge whose
    target is past our render bound). The BFS-depth invariant guarantees
    those parents sit exactly at depth `depth`, so collecting parent UUIDs
    from depth-`depth+1` edge rows yields the frontier set directly."""
    try:
        overshot = _fetch_subgraph(
            pool, root, depth + 1,
            simplified=simplified, extra_gucs=extra_gucs)
    except _SimplifiedNotAvailable:
        # Older provsql lacks the new accessor; degrade to the
        # persisted-DAG path so the panel still renders something.
        overshot = _fetch_subgraph(
            pool, root, depth + 1,
            simplified=False, extra_gucs=extra_gucs)
    if not overshot:
        return {"nodes": [], "edges": [], "root": root, "depth": depth}
    raw = [r for r in overshot if r["depth"] <= depth]
    # circuit_subgraph emits one row per (parent, node) edge, so the cap
    # is on the count of distinct nodes within the kept depth, not on
    # row count.
    unique_nodes = {r["node"] for r in raw}
    if len(unique_nodes) > max_nodes:
        # Probe the depth-1 frontier from the same overshoot data : root
        # + direct children. If this fits under the cap, the front-end
        # can offer a single-click "Render at depth 1" retry that drops
        # the user into a small initial view they can frontier-expand
        # from. If it doesn't fit (root has thousands of children, e.g.
        # an aggregation), the circuit is width-bound and depth cuts
        # don't help : the front-end suppresses the button entirely.
        depth_1_size = len({r["node"] for r in raw if r["depth"] <= 1})
        raise CircuitTooLarge(
            node_count=len(unique_nodes),
            cap=max_nodes,
            depth=depth,
            depth_1_size=depth_1_size,
        )

    # A depth-`depth` node is a frontier iff at least one node in `overshot`
    # at depth+1 has it as a parent.
    has_deeper: set[str] = {
        r["parent"] for r in overshot if r["depth"] == depth + 1 and r["parent"] is not None
    }
    return _layout(raw, root=root, depth=depth, frontier_uuids=has_deeper)


def _fetch_subgraph(
    pool: ConnectionPool, root: str, depth: int,
    *, simplified: bool = False,
    extra_gucs: dict[str, str] | None = None,
) -> list[dict]:
    # `%s::int` is required: psycopg may bind small Python ints as smallint,
    # and PG's function-overload resolution then can't reach
    # provsql.circuit_subgraph(uuid, int).
    # circuit_subgraph only exposes the integer info1/info2 pair; the
    # text-encoded `extra` (project's input→output column mapping, value's
    # / agg's scalar) lives on a separate accessor, so fetch it inline.
    # For agg / cmp gates we also dereference info1 (an oid) to its
    # human-readable name (`pg_proc.proname` / `pg_operator.oprname`) so
    # the in-circle label can read "sum" / "<=" instead of a generic Σ / ≷.
    # The CASE only evaluates the branch matching gate_type, so non-int
    # info1 values are never cast.
    if simplified:
        # The simplified function returns jsonb (one object per row);
        # expand it via jsonb_array_elements and project the same
        # columns as the persisted-DAG query.  `extra` is inlined in
        # the jsonb (the simplifier may introduce extras not visible
        # in the persisted store), so we read it from there rather
        # than calling get_extra (which would hit the mmap).
        sql = (
            "WITH src AS ("
            "  SELECT (e->>'node')        AS node,"
            "         (e->>'parent')      AS parent,"
            "         (e->>'child_pos')::int AS child_pos,"
            "         (e->>'gate_type')   AS gate_type,"
            "         (e->>'info1')       AS info1,"
            "         (e->>'info2')       AS info2,"
            "         (e->>'extra')       AS extra,"
            "         (e->>'depth')::int  AS depth"
            "  FROM jsonb_array_elements("
            "         provsql.simplified_circuit_subgraph(%s::uuid, %s::int)) AS e"
            ") "
            "SELECT cs.node, cs.parent, cs.child_pos, cs.gate_type,"
            "       cs.info1, cs.info2, cs.extra,"
            "       CASE WHEN cs.gate_type = 'agg' THEN"
            "              (SELECT proname::text FROM pg_proc WHERE oid = cs.info1::int)"
            "            WHEN cs.gate_type = 'cmp' THEN"
            "              (SELECT oprname::text FROM pg_operator WHERE oid = cs.info1::int)"
            "            ELSE NULL END AS info1_name,"
            "       CASE WHEN cs.gate_type = 'agg' THEN"
            "              (SELECT typname::text FROM pg_type WHERE oid = cs.info2::int)"
            "            ELSE NULL END AS info2_name,"
            "       cs.depth "
            "FROM src cs"
        )
    else:
        sql = (
            "SELECT cs.node::text, cs.parent::text, cs.child_pos, cs.gate_type::text, "
            "       cs.info1, cs.info2, provsql.get_extra(cs.node)::text AS extra, "
            "       CASE WHEN cs.gate_type = 'agg' THEN "
            "              (SELECT proname::text FROM pg_proc WHERE oid = cs.info1::int) "
            "            WHEN cs.gate_type = 'cmp' THEN "
            "              (SELECT oprname::text FROM pg_operator WHERE oid = cs.info1::int) "
            "            ELSE NULL END AS info1_name, "
            "       CASE WHEN cs.gate_type = 'agg' THEN "
            "              (SELECT typname::text FROM pg_type WHERE oid = cs.info2::int) "
            "            ELSE NULL END AS info2_name, "
            "       cs.depth "
            "FROM provsql.circuit_subgraph(%s::uuid, %s::int) AS cs"
        )
    out: list[dict] = []
    import psycopg
    from psycopg import sql as pg_sql
    with pool.connection() as conn, conn.cursor() as cur:
        # Apply panel-managed GUCs (provsql.simplify_on_load, ...) so
        # the user's panel choice controls what
        # simplified_circuit_subgraph returns.  Mirrors evaluate_circuit
        # / exec_batch; lazy-import the whitelist to avoid a circular
        # import at module load.
        if extra_gucs:
            from . import db as _db
            for guc_name, guc_val in extra_gucs.items():
                if guc_name not in _db._PANEL_GUCS:
                    continue
                cur.execute(
                    pg_sql.SQL("SET LOCAL {} = {}").format(
                        pg_sql.Identifier(*guc_name.split(".")),
                        pg_sql.Literal(guc_val),
                    )
                )
        try:
            cur.execute(sql, (root, depth))
        except psycopg.errors.UndefinedFunction as e:
            # provsql.simplified_circuit_subgraph wasn't installed on
            # this database; signal the caller to retry against
            # circuit_subgraph (the persisted DAG).  Only convert the
            # specific missing-function diagnostic; everything else
            # propagates.
            if simplified and "simplified_circuit_subgraph" in str(e):
                raise _SimplifiedNotAvailable() from e
            raise
        for node, parent, child_pos, gate_type, info1, info2, extra, info1_name, info2_name, d in cur.fetchall():
            out.append({
                "node": node,
                "parent": parent,
                "child_pos": child_pos,
                "gate_type": gate_type,
                "info1": info1,
                "info2": info2,
                "extra": extra,
                "info1_name": info1_name,
                "info2_name": info2_name,
                "depth": d,
            })
    return out


def _layout(rows: list[dict], *, root: str, depth: int, frontier_uuids: set[str]) -> dict:
    """Run dot to assign x/y per node, then translate into the JSON shape
    consumed by the front-end."""
    dot_src = _to_dot(rows)
    layout = _run_dot(dot_src)
    pos = {obj["name"]: _parse_pos(obj["pos"]) for obj in layout.get("objects", []) if "pos" in obj}

    # dot's coordinates have y growing upward; flip so the SVG renders the root
    # at the top (y small) and leaves at the bottom (y large).
    if pos:
        max_y = max(y for _, y in pos.values())
        pos = {k: (x, max_y - y) for k, (x, y) in pos.items()}

    # circuit_subgraph emits one row per incoming edge, so the same node
    # appears once per parent (and once with NULL parent only for the root).
    # Dedup by node id for the nodes list; keep every (parent, node) pair
    # with a non-null parent for the edges list.
    seen: set[str] = set()
    nodes: list[dict] = []
    for r in rows:
        if r["node"] in seen:
            continue
        seen.add(r["node"])
        nodes.append({
            "id":        r["node"],
            "type":      r["gate_type"],
            "label":     _gate_label(r),
            "info1":     r["info1"],
            "info2":     r["info2"],
            "info1_name": r["info1_name"],
            "info2_name": r["info2_name"],
            "extra":     r["extra"],
            "depth":     r["depth"],
            "x":         pos.get(r["node"], (0, 0))[0],
            "y":         pos.get(r["node"], (0, 0))[1],
            "frontier":  r["node"] in frontier_uuids,
        })
    edges = [
        {"from": r["parent"], "to": r["node"], "child_pos": r["child_pos"]}
        for r in rows
        if r["parent"] is not None
    ]
    return {"nodes": nodes, "edges": edges, "root": root, "depth": depth}


def _to_dot(rows: list[dict]) -> str:
    """Build a DOT source for `dot -Tjson` consumption. We only need positions,
    so styling is minimal; the front-end paints with brand colours.

    Rows from circuit_subgraph are denormalised on incoming edges (one row
    per (parent, node) pair), so dedup nodes by id before emitting them.
    """
    lines = [
        "digraph G {",
        '  rankdir=TB;',
        '  graph [nodesep=0.4, ranksep=0.6];',
        '  node  [shape=circle, fixedsize=true, width=0.55, label=""];',
        '  edge  [arrowhead=none];',
    ]
    seen: set[str] = set()
    for r in rows:
        if r["node"] in seen:
            continue
        seen.add(r["node"])
        lines.append(f'  "{r["node"]}";')
    for r in rows:
        if r["parent"] is not None:
            lines.append(f'  "{r["parent"]}" -> "{r["node"]}";')
    lines.append("}")
    return "\n".join(lines)


def _run_dot(dot_src: str) -> dict:
    proc = subprocess.run(
        ["dot", "-Tjson"],
        input=dot_src,
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(proc.stdout)


def _parse_pos(pos: str) -> tuple[float, float]:
    x, y = pos.split(",")
    return float(x), float(y)


# ───── leaf / agg_token resolution ─────────────────────────────────────

def resolve_input(pool: ConnectionPool, uuid: str) -> list[dict]:
    """Wrap `provsql.resolve_input`. Returns a list of {relation, row} dicts;
    empty when the UUID is not the provenance token of any tracked row."""
    out: list[dict] = []
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT relation::text, row_data FROM provsql.resolve_input(%s::uuid)",
            (uuid,),
        )
        for relation, row_data in cur.fetchall():
            out.append({"relation": relation, "row": row_data})
    return out


def get_prob(pool: ConnectionPool, uuid: str) -> float | None:
    """Best-effort probability fetch via `provsql.get_prob`. Returns None
    when the gate has no probability set, when the gate type doesn't
    support get_prob, or when the function is unavailable on this
    database. The inspector calls this for input / mulinput / update
    gates so the user sees the per-row probability alongside the
    resolved row, without having to query for it manually."""
    try:
        with pool.connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT provsql.get_prob(%s::uuid)", (uuid,))
            row = cur.fetchone()
            return float(row[0]) if row and row[0] is not None else None
    except Exception:
        return None


# ───── in-process layout cache ─────────────────────────────────────────

class LayoutCache:
    """Bounded LRU keyed on (root, depth). dot is not free, and the same root
    is often re-rendered as the user toggles UUIDs / formula. Bound is small
    by design: circuits are large and serializations even larger."""

    def __init__(self, capacity: int = 32):
        self._capacity = capacity
        self._store: OrderedDict[tuple[str, int], dict] = OrderedDict()

    def get(self, root: str, depth: int) -> dict | None:
        key = (root, depth)
        if key in self._store:
            self._store.move_to_end(key)
            return self._store[key]
        return None

    def put(self, root: str, depth: int, value: dict) -> None:
        key = (root, depth)
        self._store[key] = value
        self._store.move_to_end(key)
        while len(self._store) > self._capacity:
            self._store.popitem(last=False)

    def clear(self) -> None:
        """Drop every entry. Useful when a global parameter (the node
        cap, the depth) changes and previously-rendered scenes would
        no longer reflect the live setting."""
        self._store.clear()
