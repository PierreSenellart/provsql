"""Backend for the Knowledge-Compilation demo helpers.

Wraps the four SQL surfaces added in extension 1.7.0:

* ``provsql.tseytin_cnf(token, weighted)``
* ``provsql.compile_to_ddnnf_dot(token, compiler)``
* ``provsql.tree_decomposition_dot(token)``
* ``provsql.probability_benchmark(token, samples, compilers)``

For the two functions that emit GraphViz DOT we also render it to SVG
through a local ``dot`` subprocess (the same binary Studio already uses
for circuit-mode layout in ``circuit.py``), so the front-end can just
inline the result without depending on a JS Graphviz renderer.
"""
from __future__ import annotations

import json
import re
import subprocess

from psycopg_pool import ConnectionPool


def _render_svg(dot_src: str) -> str:
    """Render a DOT source to inlineable SVG via ``dot -Tsvg``."""
    proc = subprocess.run(
        ["dot", "-Tsvg"],
        input=dot_src,
        capture_output=True,
        text=True,
        check=True,
    )
    return proc.stdout


def _run_dot_json(dot_src: str) -> dict:
    """Run ``dot -Tjson`` and parse the position layout output.

    Same pipeline ``circuit_mod._run_dot`` uses for provenance-circuit
    layout; we reuse it here so d-DNNF / TD nodes get the same
    layered-top-down positions the Studio canvas expects.
    """
    proc = subprocess.run(
        ["dot", "-Tjson"],
        input=dot_src,
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(proc.stdout)


# Regex over a single DOT node-definition line:
#   <id> [label="<...>", shape=<...>, ...]
# Captures the node id and the literal label string (preserving
# embedded "\n" sequences from the DOT source).
_DOT_NODE_RE = re.compile(
    r'^\s*([A-Za-z_][\w]*|\d+)\s*\[label="([^"]*)"', re.MULTILINE
)
# Regex over a single DOT edge line:
#   <src> -> <dst>;
_DOT_EDGE_RE = re.compile(
    r'^\s*([A-Za-z_][\w]*|\d+)\s*->\s*([A-Za-z_][\w]*|\d+)\s*;?', re.MULTILINE
)


def _classify_ddnnf_node(label: str) -> tuple[str, str, str | None]:
    """Map a d-DNNF DOT label to (type, display_label, info1).

    The C++ side emits:
      * ``∧`` for AND gates,
      * ``∨`` for OR gates,
      * ``¬`` for NOT gates,
      * ``<uuid8>\\np=0.500`` for IN leaves.
    We collapse the leaf label to the UUID short prefix and surface the
    probability through ``info1`` so the front-end can render it on the
    meta line under the node circle, matching how IN leaves of the
    provenance circuit display their source row id.
    """
    if label == "∧":
        return ("kc-and", "∧", None)
    if label == "∨":
        return ("kc-or", "∨", None)
    if label == "¬":
        return ("kc-not", "¬", None)
    # IN leaf: "<uuid8>\np=0.500". Use a distinct type so the provenance
    # circuit's meta-line dispatch (which formats provenance INPUT
    # leaves as "tbl ...") doesn't fire on these probabilistic literals.
    first, _, rest = label.partition("\\n")
    info1 = rest.strip() if rest else None
    return ("kc-input", first.strip(), info1)


def _ddnnf_scene_from_dot(dot_src: str, *, original_token: str) -> dict:
    """Translate a compiled-d-DNNF DOT source into the canvas scene shape."""
    layout = _run_dot_json(dot_src)
    # The d-DNNF DOT marks its root with ``penwidth=2``; look the line up
    # to know which node id to flag as the scene root.
    root_id: str | None = None
    for line in dot_src.splitlines():
        if "penwidth=2" in line:
            m = re.match(r'^\s*([A-Za-z_][\w]*|\d+)\s*\[', line)
            if m:
                root_id = m.group(1)
                break

    pos: dict[str, tuple[float, float]] = {}
    raw_labels: dict[str, str] = {}
    for obj in layout.get("objects", []):
        name = obj.get("name")
        if "pos" in obj and name:
            x_str, y_str = obj["pos"].split(",")
            pos[name] = (float(x_str), float(y_str))
        if "label" in obj and name:
            raw_labels[name] = obj["label"]
    # Flip y so root sits at the top (smaller y) — matches the
    # provenance-circuit convention.
    if pos:
        max_y = max(y for _, y in pos.values())
        pos = {k: (x, max_y - y) for k, (x, y) in pos.items()}

    # Parse the DOT source for nodes (more reliable than the json
    # `objects` label for the SVG-escaped ∧ ∨ ¬ glyphs) and edges.
    nodes_seen: set[str] = set()
    nodes: list[dict] = []
    for m in _DOT_NODE_RE.finditer(dot_src):
        nid = m.group(1)
        label = m.group(2)
        if nid in nodes_seen:
            continue
        nodes_seen.add(nid)
        type_, display, info1 = _classify_ddnnf_node(label)
        x, y = pos.get(nid, (0.0, 0.0))
        nodes.append({
            "id":    nid,
            "type":  type_,
            "label": display,
            "info1": info1,
            "info2": None,
            "info1_name": None,
            "info2_name": None,
            "extra": None,
            "depth": 0,
            "x":     x,
            "y":     y,
            "frontier": False,
            "tracked_input": False,
            "boolean_assumed": False,
        })
    edges = []
    for m in _DOT_EDGE_RE.finditer(dot_src):
        edges.append({"from": m.group(1), "to": m.group(2), "child_pos": None})

    # Fallback root marker: the first node in source order if no
    # penwidth=2 hint came through (older / future DOT emitters that
    # drop the marker). Used to highlight the d-DNNF root in the
    # rendered scene (stashed under `kc_root_id` so the front-end can
    # find it). The scene's `root` proper is the original provenance
    # UUID so the eval-strip's target stays meaningful after the
    # canvas swap.
    if root_id is None and nodes:
        root_id = nodes[0]["id"]

    return {
        "nodes": nodes,
        "edges": edges,
        "root": original_token,
        "kc_root_id": root_id,
        "depth": 0,
    }


def _td_scene_from_dot(dot_src: str, treewidth: int | None, *, original_token: str) -> dict:
    """Translate a tree-decomposition DOT source into the canvas scene shape.

    TD bags are labelled ``{a,b,c}``; we keep the literal label so the
    in-circle glyph reads as the bag content (the renderer auto-shrinks
    the font to fit). Pick the bag with no incoming edges as the scene
    root (TreeDecomposition::toDot emits a single tree).
    """
    layout = _run_dot_json(dot_src)
    pos: dict[str, tuple[float, float]] = {}
    for obj in layout.get("objects", []):
        name = obj.get("name")
        if "pos" in obj and name:
            x_str, y_str = obj["pos"].split(",")
            pos[name] = (float(x_str), float(y_str))
    if pos:
        max_y = max(y for _, y in pos.values())
        pos = {k: (x, max_y - y) for k, (x, y) in pos.items()}

    nodes_seen: set[str] = set()
    nodes: list[dict] = []
    for m in _DOT_NODE_RE.finditer(dot_src):
        nid = m.group(1)
        label = m.group(2)
        if nid in nodes_seen:
            continue
        nodes_seen.add(nid)
        x, y = pos.get(nid, (0.0, 0.0))
        nodes.append({
            "id":    nid,
            "type":  "kc-bag",
            "label": label,
            "info1": None,
            "info2": None,
            "info1_name": None,
            "info2_name": None,
            "extra": None,
            "depth": 0,
            "x":     x,
            "y":     y,
            "frontier": False,
            "tracked_input": False,
            "boolean_assumed": False,
        })
    edges = []
    targets: set[str] = set()
    for m in _DOT_EDGE_RE.finditer(dot_src):
        src, dst = m.group(1), m.group(2)
        edges.append({"from": src, "to": dst, "child_pos": None})
        targets.add(dst)

    # Root = a bag that no edge points to. The TreeDecomposition::toDot
    # output uses parent → child edges, so a bag without an incoming
    # edge is the friendly-form root.
    root_id: str | None = None
    for n in nodes:
        if n["id"] not in targets:
            root_id = n["id"]
            break
    if root_id is None and nodes:
        root_id = nodes[0]["id"]

    out = {
        "nodes": nodes,
        "edges": edges,
        "root": original_token,
        "kc_root_id": root_id,
        "depth": 0,
    }
    if treewidth is not None:
        out["treewidth"] = treewidth
    return out


def tseytin_cnf(pool: ConnectionPool, token: str, weighted: bool) -> str:
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.tseytin_cnf(%s::uuid, %s)", (token, weighted),
        )
        (cnf,) = cur.fetchone()
    return cnf


def compile_to_ddnnf(pool: ConnectionPool, token: str, compiler: str) -> dict:
    """Return ``{"dot", "scene"}`` for the compiled d-DNNF.

    ``scene`` matches the shape of ``circuit_mod.get_circuit`` (nodes /
    edges / root / depth) so the Studio canvas renderer can paint it
    with the same pan / zoom / drag / inspector affordances as the
    provenance circuit.
    """
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.compile_to_ddnnf_dot(%s::uuid, %s)",
            (token, compiler),
        )
        (dot,) = cur.fetchone()
    return {
        "dot": dot,
        "scene": _ddnnf_scene_from_dot(dot, original_token=token),
    }


def tree_decomposition(pool: ConnectionPool, token: str) -> dict:
    """Return ``{"dot", "scene", "treewidth"}`` for the tree decomposition.

    The first line of ``provsql.tree_decomposition_dot`` is a
    ``// treewidth=<n>`` comment we tack on top of GraphViz's body;
    we surface the parsed treewidth alongside the scene payload so
    the front-end can show it in the canvas subtitle.
    """
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.tree_decomposition_dot(%s::uuid)", (token,),
        )
        (full,) = cur.fetchone()
    first, _, body = full.partition("\n")
    tw = None
    if first.startswith("// treewidth="):
        try:
            tw = int(first.removeprefix("// treewidth="))
        except ValueError:
            tw = None
    return {
        "dot": body,
        "scene": _td_scene_from_dot(body, tw, original_token=token),
        "treewidth": tw,
    }


def probability_benchmark(
    pool: ConnectionPool,
    token: str,
    samples: int,
    compilers: list[str],
) -> list[dict]:
    """Run ``probability_benchmark`` and return the rows as JSON.

    The planner-hook rewriter does not (yet) support ``RETURNS TABLE``
    functions with multiple output columns, so we ``SET LOCAL
    provsql.active = off`` for the duration of the call.
    """
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SET LOCAL provsql.active = off")
        cur.execute(
            "SELECT method, args, probability, milliseconds, error "
            "FROM provsql.probability_benchmark(%s::uuid, %s, %s) "
            "ORDER BY method, args NULLS FIRST",
            (token, samples, compilers),
        )
        rows = cur.fetchall()
    return [
        {
            "method": method,
            "args": args,
            "probability": probability,
            "milliseconds": milliseconds,
            "error": error,
        }
        for method, args, probability, milliseconds, error in rows
    ]
