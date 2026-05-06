"""Server-side circuit-DAG layout for /api/circuit/<uuid>.

Pulls the BFS subgraph of a provenance circuit via `provsql.circuit_subgraph`,
hands a DOT description to `dot -Tjson` for layered top-down placement, and
returns a `{nodes, edges}` JSON shape ready for the SVG renderer in the
front-end. The layout is bound by `--max-circuit-depth` (BFS depth) and
`--max-circuit-nodes` (response cap, rejected with 413 when exceeded).

`circuit_subgraph` returns one row per (parent, node) edge — provenance
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
        return _truncate(row["extra"])
    return _GATE_LABEL.get(t, t)


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


class CircuitTooLarge(Exception):
    def __init__(self, node_count: int, cap: int):
        super().__init__(f"circuit too large: {node_count} > {cap}")
        self.node_count = node_count
        self.cap = cap


def get_circuit(
    pool: ConnectionPool,
    *,
    root: str,
    depth: int,
    max_nodes: int,
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
    overshot = _fetch_subgraph(pool, root, depth + 1)
    if not overshot:
        return {"nodes": [], "edges": [], "root": root, "depth": depth}
    raw = [r for r in overshot if r["depth"] <= depth]
    # circuit_subgraph emits one row per (parent, node) edge, so the cap
    # is on the count of distinct nodes within the kept depth, not on
    # row count.
    unique_nodes = {r["node"] for r in raw}
    if len(unique_nodes) > max_nodes:
        raise CircuitTooLarge(node_count=len(unique_nodes), cap=max_nodes)

    # A depth-`depth` node is a frontier iff at least one node in `overshot`
    # at depth+1 has it as a parent.
    has_deeper: set[str] = {
        r["parent"] for r in overshot if r["depth"] == depth + 1 and r["parent"] is not None
    }
    return _layout(raw, root=root, depth=depth, frontier_uuids=has_deeper)


def _fetch_subgraph(pool: ConnectionPool, root: str, depth: int) -> list[dict]:
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
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(sql, (root, depth))
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
