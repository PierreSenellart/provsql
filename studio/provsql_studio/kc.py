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


def tseytin_cnf(pool: ConnectionPool, token: str, weighted: bool) -> str:
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.tseytin_cnf(%s::uuid, %s)", (token, weighted),
        )
        (cnf,) = cur.fetchone()
    return cnf


def compile_to_ddnnf(pool: ConnectionPool, token: str, compiler: str) -> dict:
    """Return ``{"dot", "svg"}`` for the compiled d-DNNF."""
    with pool.connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT provsql.compile_to_ddnnf_dot(%s::uuid, %s)",
            (token, compiler),
        )
        (dot,) = cur.fetchone()
    return {"dot": dot, "svg": _render_svg(dot)}


def tree_decomposition(pool: ConnectionPool, token: str) -> dict:
    """Return ``{"dot", "svg", "treewidth"}`` for the tree decomposition.

    The first line of ``provsql.tree_decomposition_dot`` is a
    ``// treewidth=<n>`` comment we tack on top of GraphViz's body; we
    surface the parsed treewidth alongside the rendered SVG.
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
    # Render the GraphViz body without the leading comment so `dot`
    # doesn't choke on the non-DOT prefix.
    return {"dot": body, "svg": _render_svg(body), "treewidth": tw}


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
