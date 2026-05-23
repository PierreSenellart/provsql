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
import time

import psycopg
from psycopg import sql
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
# Tooltip attribute on IN nodes: `tooltip="<full-uuid>"`. dDNNF::toDot()
# emits this so we can map the in-DOT synthetic node id back to the
# original provenance circuit's UUID and resolve the source row.
_DOT_TOOLTIP_RE = re.compile(
    r'^\s*([A-Za-z_][\w]*|\d+)\s*\[[^\]]*tooltip="([^"]*)"', re.MULTILINE
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
    AND / OR / NOT nodes are mapped to their math glyphs, IN leaves to
    the ``ι`` glyph used by the provenance-circuit renderer for
    @c gate_input. The leaf's @c info1 is left empty; the front-end
    treats kc-input nodes as regular provenance input leaves and
    surfaces the probability + source row via the same
    @c fetchLeafRow path as for @c input gates.
    """
    if label == "∧":
        return ("kc-and", "∧", None)
    if label == "∨":
        return ("kc-or", "∨", None)
    if label == "¬":
        return ("kc-not", "¬", None)
    # IN leaf: "<uuid8>\np=0.500". Drop the probability text from the
    # label and from info1 — the inspector fetches it (and the source
    # row) via /api/leaf/<uuid>, matching the provenance circuit's
    # input-gate behaviour.
    return ("kc-input", "ι", None)


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

    # Pre-extract the per-IN-node tooltip mapping so we can rewrite
    # kc-input synthetic ids to the full provenance UUID. The synthetic
    # id (`n3`, etc.) is only used by the DOT itself; the front-end
    # treats the UUID as the gate identity and uses it to resolve the
    # source row via /api/leaf/<uuid>.
    syn_to_uuid: dict[str, str] = {
        m.group(1): m.group(2) for m in _DOT_TOOLTIP_RE.finditer(dot_src)
    }

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
        node_id = nid
        if type_ == "kc-input" and nid in syn_to_uuid:
            node_id = syn_to_uuid[nid]
        nodes.append({
            "id":    node_id,
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
        # Remap edge endpoints to the UUID for kc-input nodes so the
        # canvas wiring matches the rewritten node ids above.
        edges.append({
            "from": syn_to_uuid.get(m.group(1), m.group(1)),
            "to":   syn_to_uuid.get(m.group(2), m.group(2)),
            "child_pos": None,
        })

    # Remap the root marker in the (rare) case the root is itself an
    # IN leaf, so kc_root_id refers to the same id the canvas uses.
    if root_id is not None and root_id in syn_to_uuid:
        root_id = syn_to_uuid[root_id]

    # Fallback root marker: the first node in source order if no
    # penwidth=2 hint came through (older / future DOT emitters that
    # drop the marker). Used to highlight the d-DNNF root in the
    # rendered scene (stashed under `kc_root_id` so the front-end can
    # find it). The scene's `root` proper is the original provenance
    # UUID so the eval-strip's target stays meaningful after the
    # canvas swap.
    if root_id is None and nodes:
        root_id = nodes[0]["id"]

    # Longest-path depth from the root (the canonical circuit-depth
    # notion), computed via Kahn's-style topological relaxation: each
    # node is finalised once all its in-edges have contributed a
    # candidate `parent_depth + 1`, and we keep the max.
    if root_id is not None:
        adj: dict[str, list[str]] = {n["id"]: [] for n in nodes}
        for e in edges:
            if e["from"] in adj:
                adj[e["from"]].append(e["to"])
        indeg: dict[str, int] = {n["id"]: 0 for n in nodes}
        for e in edges:
            if e["to"] in indeg and e["from"] in adj:
                indeg[e["to"]] += 1
        depth_of: dict[str, int] = {root_id: 0}
        frontier: list[str] = [root_id]
        while frontier:
            nxt: list[str] = []
            for n in frontier:
                d = depth_of[n]
                for c in adj.get(n, []):
                    cand = d + 1
                    if depth_of.get(c, -1) < cand:
                        depth_of[c] = cand
                    indeg[c] -= 1
                    if indeg[c] == 0:
                        nxt.append(c)
            frontier = nxt
        for n in nodes:
            if n["id"] in depth_of:
                n["depth"] = depth_of[n["id"]]

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

    # Longest-path depth from the TD root, mirroring the d-DNNF scene
    # path. The TD bag DAG is in practice a tree, so longest and
    # shortest coincide; we use the same Kahn's-style relaxation as the
    # d-DNNF case for code symmetry and to stay correct if a future TD
    # exporter ever shares bags.
    if root_id is not None:
        adj: dict[str, list[str]] = {n["id"]: [] for n in nodes}
        for e in edges:
            if e["from"] in adj:
                adj[e["from"]].append(e["to"])
        indeg: dict[str, int] = {n["id"]: 0 for n in nodes}
        for e in edges:
            if e["to"] in indeg and e["from"] in adj:
                indeg[e["to"]] += 1
        depth_of: dict[str, int] = {root_id: 0}
        frontier: list[str] = [root_id]
        while frontier:
            nxt: list[str] = []
            for n in frontier:
                d = depth_of[n]
                for c in adj.get(n, []):
                    cand = d + 1
                    if depth_of.get(c, -1) < cand:
                        depth_of[c] = cand
                    indeg[c] -= 1
                    if indeg[c] == 0:
                        nxt.append(c)
            frontier = nxt
        for n in nodes:
            if n["id"] in depth_of:
                n["depth"] = depth_of[n["id"]]

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


# External-tool dependency of every benchmark method.  Empty tuple
# means the method is fully in-process and always available; a non-
# empty tuple lists the bare executable names looked up through
# ``provsql.tool_available`` (which honours ``provsql.tool_search_path``
# and the backend's resolved ``PATH``).  ``dpmc`` is the only entry
# with two dependencies: the kernel needs both ``htb`` (tree-
# decomposition) and ``dmc`` (DPMC's project-join solver) on PATH.
_METHOD_TOOL_DEPS: dict[tuple[str, str | None], tuple[str, ...]] = {
    ("independent",        None): (),
    ("possible-worlds",    None): (),
    ("tree-decomposition", None): (),
    ("monte-carlo",        None): (),
    ("compilation", "d4"):              ("d4",),
    ("compilation", "d4v2"):            ("d4v2",),
    ("compilation", "c2d"):             ("c2d",),
    ("compilation", "minic2d"):         ("minic2d",),
    ("compilation", "dsharp"):          ("dsharp",),
    ("compilation", "panini-obdd"):     ("panini",),
    ("compilation", "panini-obdd-and"): ("panini",),
    ("compilation", "panini-decdnnf"):  ("panini",),
    ("compilation", "panini-r2d2"):     ("panini",),
    ("compilation", "panini-ccdd"):     ("panini",),
    ("wmc", "weightmc;0.8;0.2"):        ("weightmc",),
    ("wmc", "ganak"):                   ("ganak",),
    ("wmc", "sharpsat-td"):             ("sharpsat-td",),
    ("wmc", "dpmc"):                    ("htb", "dmc"),
}

# Methods and per-method arguments exercised by the benchmark.  Mirrors
# the body of ``provsql.probability_benchmark`` in
# ``sql/provsql.common.sql`` but lives here because we drive the loop
# from Python: each method needs its own ``SET LOCAL
# statement_timeout`` to get a per-row time budget, and PL/pgSQL's
# ``WHEN OTHERS`` does not catch ``query_canceled`` (57014) so a
# timeout inside the SQL helper aborts the whole table instead of
# producing one timeout row.
_BENCHMARK_METHODS: tuple[tuple[str, str | None], ...] = tuple(_METHOD_TOOL_DEPS.keys())

# Compiler values offered by the Studio eval-strip "Compilation"
# dropdown.  Includes options outside the benchmark surface
# (``tree-decomposition``, ``interpret-as-dd``, ``default``) so the
# /api/kc/tools payload covers every dropdown entry in one shot.
_COMPILER_TOOL_DEPS: dict[str, tuple[str, ...]] = {
    "d4":                 ("d4",),
    "d4v2":               ("d4v2",),
    "c2d":                ("c2d",),
    "minic2d":            ("minic2d",),
    "dsharp":             ("dsharp",),
    "panini-obdd":        ("panini",),
    "panini-obdd-and":    ("panini",),
    "panini-decdnnf":     ("panini",),
    "panini-r2d2":        ("panini",),
    "panini-ccdd":        ("panini",),
    "tree-decomposition": (),
    "interpret-as-dd":    (),
    "default":            (),
}

# WMC-tool values offered by the eval-strip "wmc" dropdown.  The
# ``weightmc;ε;δ`` form embeds default arguments; the tool itself is
# just ``weightmc``.
_WMC_TOOL_DEPS: dict[str, tuple[str, ...]] = {
    "ganak":            ("ganak",),
    "sharpsat-td":      ("sharpsat-td",),
    "dpmc":             ("htb", "dmc"),
    "weightmc;0.8;0.2": ("weightmc",),
}

# Every distinct tool name any dropdown / benchmark row can depend on.
# Queried in a single SQL round-trip via ``provsql.tool_available``.
_KNOWN_TOOLS: tuple[str, ...] = tuple(sorted({
    tool
    for deps in (
        *_METHOD_TOOL_DEPS.values(),
        *_COMPILER_TOOL_DEPS.values(),
        *_WMC_TOOL_DEPS.values(),
    )
    for tool in deps
}))


def query_tool_availability(pool: ConnectionPool) -> dict[str, bool]:
    """Return ``{tool_name: available}`` for every external tool the KC
    demo helpers and probability benchmark can dispatch to.

    Uses ``provsql.tool_available`` so the result reflects the
    backend's resolved ``PATH`` plus the ``provsql.tool_search_path``
    GUC, not Studio's process environment.
    """
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT name, provsql.tool_available(name) "
            "FROM unnest(%s::text[]) AS name",
            (list(_KNOWN_TOOLS),),
        )
        return {name: bool(avail) for name, avail in cur.fetchall()}


def tools_status(pool: ConnectionPool) -> dict:
    """Structured payload for the Studio ``/api/kc/tools`` endpoint.

    Mirrors the option lists of the eval-strip dropdowns so the JS
    side can hide unavailable entries without replicating the
    method → tool mapping.  ``tools`` is the raw map (one entry per
    distinct tool); ``options.compilation`` / ``options.wmc`` are the
    per-dropdown derived maps; ``methods`` is keyed by
    ``"<method>|<args>"`` (args ``""`` for None) and used by both
    the probability benchmark filter and a hypothetical future
    ``probability_evaluate`` UI gate.
    """
    tools = query_tool_availability(pool)
    def _avail(deps: tuple[str, ...]) -> bool:
        return all(tools.get(t, False) for t in deps)
    return {
        "tools": tools,
        "options": {
            "compilation": {opt: _avail(deps) for opt, deps in _COMPILER_TOOL_DEPS.items()},
            "wmc":         {opt: _avail(deps) for opt, deps in _WMC_TOOL_DEPS.items()},
        },
        "methods": {
            f"{method}|{args or ''}": _avail(deps)
            for (method, args), deps in _METHOD_TOOL_DEPS.items()
        },
    }


def probability_benchmark(
    pool: ConnectionPool,
    token: str,
    samples: int,
    statement_timeout: str = "30s",
) -> dict:
    """Time every probability-evaluation method and return rows + notices.

    Mirrors the surface of ``provsql.probability_benchmark`` (see
    ``sql/provsql.common.sql``) but drives the loop from Python so each
    method gets its own ``SET LOCAL statement_timeout`` budget.  Two
    PL/pgSQL limitations make per-row enforcement impossible from the
    SQL helper: ``SET LOCAL statement_timeout`` inside a function does
    not reset PG's per-statement timer, and ``EXCEPTION WHEN OTHERS``
    does not catch ``query_canceled`` (57014), so the SQL version
    aborts the whole table instead of recording one timeout row.

    Each method runs inside its own savepoint so a per-method
    ``SET LOCAL statement_timeout`` and any error (timeout, missing
    compiler, non-independent circuit, …) stay scoped to that method;
    the next iteration continues with a fresh budget.

    Notices emitted by ProvSQL during any of the inner
    ``probability_evaluate`` calls (typically the "gate_cmp shortcut
    by probability-side pre-pass" warning) are collected via the
    connection's notice handler and deduplicated before being returned
    alongside the rows.
    """
    notices: list[str] = []

    def _on_notice(diag):
        msg = diag.message_primary or ""
        if "__prov" in msg or "__wprov" in msg:
            return
        notices.append(msg)

    # Skip methods whose external tools are missing on the backend.
    # The benchmark would otherwise spend ~ms per row reproducing the
    # same "X not found on PATH" error.  Methods with no dependency
    # (independent / possible-worlds / tree-decomposition / monte-
    # carlo) always survive the filter.
    tools = query_tool_availability(pool)
    runnable = [
        (m, a) for (m, a) in _BENCHMARK_METHODS
        if all(tools.get(t, False) for t in _METHOD_TOOL_DEPS[(m, a)])
    ]

    rows: list[dict] = []
    with pool.connection() as conn:
        conn.add_notice_handler(_on_notice)
        try:
            # Outer transaction: GUCs that should hold for the whole
            # benchmark, applied once with SET LOCAL so they revert when
            # the connection returns to the pool.  Each method below
            # opens a savepoint inside this transaction.
            with conn.transaction():
                with conn.cursor() as cur:
                    cur.execute("SET LOCAL provsql.active = off")
                    # Same verbose-level floor as evaluate_circuit:
                    # guarantee that level-5 informational notices (e.g.
                    # shortcut warnings) reach the client even when the
                    # user has silenced ProvSQL via verbose_level=0.
                    cur.execute(
                        "SELECT set_config('provsql.verbose_level', "
                        "GREATEST(5, current_setting('provsql.verbose_level', "
                        "true)::int)::text, true)"
                    )
                for method, args in runnable:
                    call_args = str(samples) if method == "monte-carlo" else args
                    t0 = time.perf_counter()
                    probability: float | None = None
                    error: str | None = None
                    try:
                        # Nested transaction => SAVEPOINT.  SET LOCAL
                        # statement_timeout is rolled back with the
                        # savepoint on timeout, so the next method
                        # starts from a clean GUC state.
                        with conn.transaction():
                            with conn.cursor() as cur:
                                cur.execute(
                                    sql.SQL(
                                        "SET LOCAL statement_timeout = {}"
                                    ).format(sql.Literal(statement_timeout))
                                )
                                cur.execute(
                                    "SELECT provsql.probability_evaluate("
                                    "%s::uuid, %s, %s)",
                                    (token, method, call_args),
                                )
                                row = cur.fetchone()
                                if row is not None and row[0] is not None:
                                    probability = float(row[0])
                    except psycopg.errors.QueryCanceled:
                        error = (
                            f"canceling statement due to statement timeout "
                            f"(> {statement_timeout})"
                        )
                    except psycopg.Error as e:
                        # Mirror SQLERRM: a single-line summary so the
                        # error column stays compact in the table.
                        diag = getattr(e, "diag", None)
                        msg = (diag.message_primary if diag else None) \
                            or str(e).strip()
                        error = msg.splitlines()[0] if msg else "unknown error"
                    ms = (time.perf_counter() - t0) * 1000.0
                    rows.append({
                        "method": method,
                        "args": call_args,
                        "probability": probability,
                        "milliseconds": ms,
                        "error": error,
                    })
        finally:
            try:
                conn.remove_notice_handler(_on_notice)
            except Exception:
                pass

    # Match the old SQL ORDER BY method, args NULLS FIRST.
    rows.sort(key=lambda r: (r["method"], r["args"] is not None, r["args"] or ""))

    # Deduplicate identical notices — each method emits the same
    # shortcut notice, so the same line repeats once per benchmark row.
    seen: set[str] = set()
    uniq: list[str] = []
    for msg in notices:
        if msg in seen:
            continue
        seen.add(msg)
        uniq.append(msg)
    return {"rows": rows, "notices": uniq}
