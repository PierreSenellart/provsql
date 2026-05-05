"""Server-side circuit-DAG layout for /api/circuit/<uuid>.

Pulls the BFS subgraph of a provenance circuit via `provsql.circuit_subgraph`,
hands a DOT description to `dot -Tjson` for layered top-down placement, and
returns a `{nodes, edges}` JSON shape ready for the SVG renderer in the
front-end. The layout is bound by `--max-circuit-depth` (BFS depth) and
`--max-circuit-nodes` (response cap, rejected with 413 when exceeded).
"""
from __future__ import annotations

import json
import subprocess
from collections import OrderedDict

from psycopg_pool import ConnectionPool

# Gate-type label glyphs. Stage 3 only consumes the structural shape; richer
# rendering (info1/info2 callouts, leaf row previews) lives in the front-end.
_GATE_LABEL = {
    "input":    "·",
    "plus":     "+",
    "times":    "×",
    "monus":    "−",
    "project":  "π",
    "zero":     "⊥",
    "one":      "⊤",
    "eq":       "=",
    "agg":      "Σ",
    "semimod":  "⊙",
    "cmp":      "≷",
    "delta":    "δ",
    "value":    "v",
    "mulinput": "⋮",
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
    the deepest layer, but use the fact that a depth-`depth` node has at least
    one depth-`depth+1` child to mark it as a frontier. This avoids a second
    round-trip and works around a quirk in the provsql backend cache where
    `get_gate_type` (called at the tail of `circuit_subgraph`) populates the
    process-local cache with an empty children list, masking later direct
    `get_children` calls in the same backend."""
    overshot = _fetch_subgraph(pool, root, depth + 1)
    if not overshot:
        return {"nodes": [], "edges": [], "root": root, "depth": depth}
    raw = [r for r in overshot if r["depth"] <= depth]
    if len(raw) > max_nodes:
        raise CircuitTooLarge(node_count=len(raw), cap=max_nodes)

    # A depth-`depth` node is a frontier iff at least one node in `overshot`
    # at depth+1 has it as a parent.
    has_deeper: set[str] = {
        r["parent"] for r in overshot if r["depth"] == depth + 1 and r["parent"] is not None
    }
    return _layout(raw, root=root, depth=depth, frontier_uuids=has_deeper)


def _fetch_subgraph(pool: ConnectionPool, root: str, depth: int) -> list[dict]:
    sql = (
        "SELECT node::text, parent::text, child_pos, gate_type::text, info1, info2, depth "
        "FROM provsql.circuit_subgraph(%s::uuid, %s)"
    )
    out: list[dict] = []
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(sql, (root, depth))
        for node, parent, child_pos, gate_type, info1, info2, d in cur.fetchall():
            out.append({
                "node": node,
                "parent": parent,
                "child_pos": child_pos,
                "gate_type": gate_type,
                "info1": info1,
                "info2": info2,
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

    nodes = [
        {
            "id":        r["node"],
            "type":      r["gate_type"],
            "label":     _GATE_LABEL.get(r["gate_type"], r["gate_type"]),
            "info1":     r["info1"],
            "info2":     r["info2"],
            "depth":     r["depth"],
            "x":         pos.get(r["node"], (0, 0))[0],
            "y":         pos.get(r["node"], (0, 0))[1],
            "frontier":  r["node"] in frontier_uuids,
        }
        for r in rows
    ]
    edges = [
        {"from": r["parent"], "to": r["node"], "child_pos": r["child_pos"]}
        for r in rows
        if r["parent"] is not None
    ]
    return {"nodes": nodes, "edges": edges, "root": root, "depth": depth}


def _to_dot(rows: list[dict]) -> str:
    """Build a DOT source for `dot -Tjson` consumption. We only need positions,
    so styling is minimal; the front-end paints with brand colours."""
    lines = [
        "digraph G {",
        '  rankdir=TB;',
        '  graph [nodesep=0.4, ranksep=0.6];',
        '  node  [shape=circle, fixedsize=true, width=0.55, label=""];',
        '  edge  [arrowhead=none];',
    ]
    for r in rows:
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
