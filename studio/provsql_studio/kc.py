"""Backend for the Knowledge-Compilation demo helpers.

Wraps the knowledge-compilation SQL surfaces added in extension 1.7.0:

* ``provsql.tseytin_cnf(token, weighted)``
* ``provsql.compile_to_ddnnf_dot(token, compiler)``
* ``provsql.tree_decomposition_dot(token)``

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
    # label and from info1; the inspector fetches it (and the source
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
    # Flip y so root sits at the top (smaller y), matching the
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
    # IN leaf, so the depth computation below uses the canvas id.
    if root_id is not None and root_id in syn_to_uuid:
        root_id = syn_to_uuid[root_id]

    # Fallback root marker: the first node in source order if no
    # penwidth=2 hint came through (older / future DOT emitters that
    # drop the marker). The scene's `root` proper is the original
    # provenance UUID so the eval-strip's target stays meaningful after
    # the canvas swap; `root_id` here is only used to seed the
    # longest-path depth pass below.
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
        "depth": 0,
    }
    if treewidth is not None:
        out["treewidth"] = treewidth
    return out


def tseytin_cnf(
    pool: ConnectionPool, token: str, weighted: bool, mapping: bool = False,
) -> str:
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.tseytin_cnf(%s::uuid, %s, %s)",
            (token, weighted, mapping),
        )
        (cnf,) = cur.fetchone()
    return cnf


def compile_to_ddnnf(
    pool: ConnectionPool, token: str, compiler: str,
    statement_timeout: str = "30s",
) -> dict:
    """Return ``{"dot", "scene", "milliseconds"}`` for the compiled d-DNNF.

    ``scene`` matches the shape of ``circuit_mod.get_circuit`` (nodes /
    edges / root / depth) so the Studio canvas renderer can paint it
    with the same pan / zoom / drag / inspector affordances as the
    provenance circuit.  The compilation runs under @p statement_timeout
    (a long external compiler or a structured-d-DNNF build is bounded like
    any other evaluation) and its server-side wall-clock is reported.
    """
    with pool.connection() as conn:
        with conn.transaction():
            with conn.cursor() as cur:
                cur.execute(sql.SQL("SET LOCAL statement_timeout = {}").format(
                    sql.Literal(statement_timeout)))
                t0 = time.perf_counter()
                cur.execute(
                    "SELECT provsql.compile_to_ddnnf_dot(%s::uuid, %s)",
                    (token, compiler),
                )
                (dot,) = cur.fetchone()
                ms = (time.perf_counter() - t0) * 1000.0
    return {
        "dot": dot,
        "scene": _ddnnf_scene_from_dot(dot, original_token=token),
        "milliseconds": ms,
    }


def compile_to_ddnnf_nnf(pool: ConnectionPool, token: str, compiler: str) -> str:
    """Return the compiled d-DNNF as c2d/d4 ".nnf" text.

    The text companion to ``compile_to_ddnnf`` (which returns DOT for the
    canvas): this is the machine-readable interchange form, with variable
    numbering matching ``tseytin_cnf``.
    """
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.compile_to_ddnnf(%s::uuid, %s)",
            (token, compiler),
        )
        (nnf,) = cur.fetchone()
    return nnf


def tree_decomposition(pool: ConnectionPool, token: str) -> dict:
    """Return ``{"dot", "scene", "treewidth"}`` for the tree decomposition.

    The DOT body produced by ``provsql.tree_decomposition_dot`` is
    prefixed by two comment lines:

      // treewidth=<n>
      // inputs: <idx>=<uuid> <idx>=<uuid> ...

    The treewidth is surfaced on the scene payload so the canvas
    subtitle can show it; the inputs map associates each
    BooleanCircuit gate index that survives as an input leaf with its
    original provenance UUID, so the front-end can resolve "what row
    is variable 4?" via the existing /api/leaf endpoint.  Indices
    absent from the map are post-Tseytin auxiliary gates with no
    source row.
    """
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.tree_decomposition_dot(%s::uuid)", (token,),
        )
        (full,) = cur.fetchone()
    lines = full.split("\n")
    tw: int | None = None
    bag_inputs: dict[str, str] = {}
    body_start = 0
    for i, line in enumerate(lines):
        if line.startswith("// treewidth="):
            try:
                tw = int(line.removeprefix("// treewidth="))
            except ValueError:
                tw = None
            body_start = i + 1
        elif line.startswith("// inputs:"):
            payload = line.removeprefix("// inputs:").strip()
            for tok in payload.split():
                idx, eq, uuid = tok.partition("=")
                if eq and idx and uuid:
                    bag_inputs[idx] = uuid
            body_start = i + 1
        else:
            break
    body = "\n".join(lines[body_start:])
    scene = _td_scene_from_dot(body, tw, original_token=token)
    # Attach the BC-index → UUID map so the front-end can resolve
    # input-leaf variables back to source rows in the bag inspector.
    if bag_inputs:
        scene["bag_inputs"] = bag_inputs
    return {
        "dot": body,
        "scene": scene,
        "treewidth": tw,
    }


# Probability-evaluation methods that run fully in-process (no external
# tool), so they are always available.  These are *methods*, not tools, so
# they are not in ``provsql.tools``; the benchmark lists them alongside the
# external compilers / counters the registry advertises.  ``inversion-free``
# is in-process too but only meaningful on a query certified inversion-free
# (its root carries the certificate), so the benchmark adds it conditionally
# rather than listing it here.
_INPROCESS_METHODS: tuple[tuple[str, str | None], ...] = (
    ("independent",        None),
    ("possible-worlds",    None),
    ("tree-decomposition", None),
    ("monte-carlo",        None),
)

# In-process meta-routes offered in the compilation dropdown beyond the
# registered external compilers (all dispatched through makeDD); always
# available since they invoke no external tool.  ``inversion-free`` builds the
# structured d-DNNF over the query-derived order (buildInversionFreeDDNNF); it
# only succeeds on a circuit certified inversion-free, so the front-end offers
# it only when the root carries the certificate, but it is a known in-process
# compiler so the registry / route validation accepts it.
_INPROCESS_COMPILERS: tuple[str, ...] = (
    "tree-decomposition", "interpret-as-dd", "inversion-free", "default",
)

def query_catalog(pool: ConnectionPool) -> dict[str, list[dict]]:
    """The external-tool catalog from ``provsql.tools`` (extension >=
    1.8.0), grouped by the operations Studio dispatches to.

    Returns ``{"compile": [...], "wmc": [...]}`` where each entry is
    ``{"name", "available", "preference"}``.  Disabled tools are omitted
    (an administrator has turned them off); the rows are ordered by
    descending preference, the registry's own selection order.
    ``available`` reflects the backend's resolved ``PATH`` plus
    ``provsql.tool_search_path`` (and, for multi-binary tools like dpmc,
    every dependency).
    """
    compile_tools: list[dict] = []
    wmc_tools: list[dict] = []
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT name, operations, available, preference "
            "FROM provsql.tools WHERE enabled "
            "ORDER BY preference DESC, name"
        )
        for name, operations, available, preference in cur.fetchall():
            entry = {
                "name": name,
                "available": bool(available),
                "preference": preference,
            }
            if "compile" in operations:
                compile_tools.append(entry)
            if "wmc" in operations:
                wmc_tools.append(entry)
    return {"compile": compile_tools, "wmc": wmc_tools}


def query_registry(pool: ConnectionPool) -> list[dict]:
    """Every row of ``provsql.tools`` (extension >= 1.8.0), full columns,
    for the Studio Tools panel.

    Unlike :func:`query_catalog` (which feeds the dropdowns and lists only
    enabled tools by name/availability), this returns the whole registry --
    disabled tools included -- so the panel can manage it.  Ordered by
    descending preference then name, the registry's own selection order.
    """
    rows: list[dict] = []
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT name, kind, executable, operations, input_formats, "
            "       output_format, parser, preference, enabled, endpoint, "
            "       argtpl, argtpl_circuit, available "
            "FROM provsql.tools ORDER BY preference DESC, name"
        )
        for (name, kind, executable, operations, input_formats, output_format,
             parser, preference, enabled, endpoint, argtpl, argtpl_circuit,
             available) in cur.fetchall():
            rows.append({
                "name": name,
                "kind": kind,
                "executable": executable or "",
                "operations": list(operations or []),
                "input_formats": list(input_formats or []),
                "output_format": output_format or "",
                "parser": parser or "",
                "preference": preference,
                "enabled": bool(enabled),
                "endpoint": endpoint or "",
                "argtpl": argtpl or "",
                "argtpl_circuit": argtpl_circuit or "",
                "available": bool(available),
            })
    return rows


def can_manage_tools(pool: ConnectionPool) -> bool:
    """True iff the connected role may edit the registry: the mutators are
    superuser-only, so the panel disables its controls otherwise."""
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT current_setting('is_superuser') = 'on'")
        return bool(cur.fetchone()[0])


def set_tool_enabled(pool: ConnectionPool, name: str, enabled: bool) -> None:
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.set_tool_enabled(%s, %s)", (name, enabled))


def set_tool_preference(pool: ConnectionPool, name: str, preference: int) -> None:
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.set_tool_preference(%s, %s)",
                    (name, preference))


def unregister_tool(pool: ConnectionPool, name: str) -> None:
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT provsql.unregister_tool(%s)", (name,))


def register_tool(pool: ConnectionPool, spec: dict) -> None:
    """Create or replace a registry tool from a Studio form ``spec``.

    Empty strings for the optional text fields are passed as SQL NULL so the
    ``register_tool`` defaults apply (e.g. executable defaults to the name);
    empty arrays stay empty arrays.
    """
    def _t(key):                       # text field, "" -> NULL
        v = (spec.get(key) or "").strip()
        return v or None

    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.register_tool("
            "  name => %s, executable => %s, kind => %s, operations => %s, "
            "  input_formats => %s, output_format => %s, parser => %s, "
            "  argtpl => %s, argtpl_circuit => %s, preference => %s, "
            "  enabled => %s, endpoint => %s)",
            (
                (spec.get("name") or "").strip(),
                _t("executable"),
                (spec.get("kind") or "cli").strip(),
                list(spec.get("operations") or []),
                list(spec.get("input_formats") or []),
                _t("output_format"),
                _t("parser"),
                _t("argtpl"),
                _t("argtpl_circuit"),
                int(spec.get("preference") or 0),
                bool(spec.get("enabled", True)),
                _t("endpoint"),
            ),
        )


def missing_tools_for_compiler(
    pool: ConnectionPool, compiler: str
) -> tuple[str, ...]:
    """Return ``(compiler,)`` if the named compiler is not currently
    usable, else ``()``.

    In-process meta-routes are always usable.  An external compiler is
    usable iff it is registered, enabled, and ``provsql.tools`` reports it
    as available (binary + any dependencies on PATH).  Used by the
    /api/kc/ddnnf route to return a clean 501 before invoking the SQL
    function would 500 on a missing binary.
    """
    if compiler in _INPROCESS_COMPILERS:
        return ()
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT available FROM provsql.tools "
            "WHERE name = %s AND enabled AND 'compile' = ANY(operations)",
            (compiler,),
        )
        row = cur.fetchone()
    return () if (row is not None and row[0]) else (compiler,)


def missing_tools_for_names(
    pool: ConnectionPool, names: tuple[str, ...]
) -> tuple[str, ...]:
    """Tools among ``names`` that ``provsql.tool_available`` reports as
    missing.  Used by routes that depend on a bare binary outside the
    registry (e.g. ``dot`` for the tree-decomposition DOT renderer)."""
    if not names:
        return ()
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT name FROM unnest(%s::text[]) AS name "
            "WHERE NOT provsql.tool_available(name)",
            (list(names),),
        )
        return tuple(row[0] for row in cur.fetchall())


def tools_status(pool: ConnectionPool) -> dict:
    """Payload for the Studio ``/api/kc/tools`` endpoint.

    Returns the live external-tool catalog (``compile`` and ``wmc``
    lists from ``provsql.tools``, each entry carrying ``available``) plus
    the always-available in-process meta-routes, so the JS builds the
    compiler / wmc dropdowns and the benchmark directly from the
    registry -- a newly registered tool appears with no Studio change.
    Requires ``provsql.tools`` (extension >= 1.8.0).
    """
    cat = query_catalog(pool)
    return {
        "compile": cat["compile"],
        "wmc": cat["wmc"],
        "inprocess_compilers": list(_INPROCESS_COMPILERS),
    }


def known_compilers(pool: ConnectionPool) -> set[str]:
    """Every compiler name the ``compilation`` method accepts: the
    in-process meta-routes plus the enabled external compile tools in
    ``provsql.tools``.  Used to 400 an unknown compiler before dispatch."""
    names = set(_INPROCESS_COMPILERS)
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT name FROM provsql.tools "
            "WHERE enabled AND 'compile' = ANY(operations)"
        )
        names.update(row[0] for row in cur.fetchall())
    return names


def _root_is_inversion_free(pool: ConnectionPool, token: str) -> bool:
    """True when @p token's root is an inversion-free certificate carrier.

    The planner stamps the serialised certificate as a ``C``-prefixed ``extra``
    on a transparent ``gate_annotation`` root; that is exactly the case where
    the ``'inversion-free'`` probability method applies, so the benchmark uses
    it to decide whether to include that method's row.
    """
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.get_gate_type(%(t)s::uuid) = 'annotation' "
            "AND left(provsql.get_extra(%(t)s::uuid), 1) = 'C'",
            {"t": token},
        )
        row = cur.fetchone()
    return bool(row and row[0])


def probability_benchmark(
    pool: ConnectionPool,
    token: str,
    samples: int,
    statement_timeout: str = "30s",
    boolean_provenance: bool = False,
) -> dict:
    """Time every probability-evaluation method and return rows + notices.

    Drives the loop from Python so each method gets its own ``SET LOCAL
    statement_timeout`` budget.  Two PL/pgSQL limitations make per-row
    enforcement impossible in a SQL function: ``SET LOCAL
    statement_timeout`` inside a function does not reset PG's
    per-statement timer, and ``EXCEPTION WHEN OTHERS`` does not catch
    ``query_canceled`` (57014), so a SQL version would abort the whole
    table instead of recording one timeout row.

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

    # The benchmark lists every available tool: the in-process methods
    # (always runnable) plus each enabled, available compiler (as the
    # "compilation" method) and weighted model counter (as "wmc"), taken
    # live from provsql.tools.  Unavailable tools are skipped so the
    # benchmark does not spend a row reproducing "X not found on PATH".
    # 'inversion-free' is in-process but only meaningful when the root
    # carries the inversion-free certificate (the explicit method errors
    # otherwise), so it is added only then.
    cat = query_catalog(pool)
    runnable: list[tuple[str, str | None]] = list(_INPROCESS_METHODS)
    if _root_is_inversion_free(pool, token):
        runnable.append(("inversion-free", None))
    runnable += [("compilation", t["name"]) for t in cat["compile"]
                 if t["available"]]
    runnable += [("wmc", t["name"]) for t in cat["wmc"] if t["available"]]

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
                    # Evaluate the stored circuit under the same
                    # boolean_provenance state the user is in: the
                    # load-time Boolean fold (foldBooleanIdentities) is
                    # gated on this GUC, so the benchmark must match the
                    # Marginal-probability / circuit-view path to report
                    # the same circuit the user sees.
                    cur.execute(
                        "SET LOCAL provsql.boolean_provenance = "
                        + ("on" if boolean_provenance else "off")
                    )
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
                    # For methods that build a d-DNNF, attach its size so
                    # the table compares structure alongside time. Done
                    # AFTER the timer (a second compile via ddnnf_stats)
                    # so it never inflates the reported milliseconds.
                    nodes = edges = None
                    if error is None:
                        compiler = None
                        if method == "tree-decomposition":
                            compiler = "tree-decomposition"
                        elif method == "compilation":
                            compiler = call_args
                        if compiler is not None:
                            try:
                                with conn.transaction():
                                    with conn.cursor() as cur:
                                        cur.execute(
                                            sql.SQL("SET LOCAL statement_timeout = {}")
                                            .format(sql.Literal(statement_timeout)))
                                        cur.execute(
                                            "SELECT provsql.ddnnf_stats(%s::uuid, %s)",
                                            (token, compiler))
                                        st = cur.fetchone()[0]
                                if st:
                                    nodes = st.get("nodes")
                                    edges = st.get("edges")
                            except (psycopg.Error, KeyError, TypeError):
                                pass
                    rows.append({
                        "method": method,
                        "args": call_args,
                        "probability": probability,
                        "milliseconds": ms,
                        "nodes": nodes,
                        "edges": edges,
                        "error": error,
                    })
        finally:
            try:
                conn.remove_notice_handler(_on_notice)
            except Exception:
                pass

    # Match the old SQL ORDER BY method, args NULLS FIRST.
    rows.sort(key=lambda r: (r["method"], r["args"] is not None, r["args"] or ""))

    # Deduplicate identical notices: each method emits the same
    # shortcut notice, so the same line repeats once per benchmark row.
    seen: set[str] = set()
    uniq: list[str] = []
    for msg in notices:
        if msg in seen:
            continue
        seen.add(msg)
        uniq.append(msg)
    return {"rows": rows, "notices": uniq}
