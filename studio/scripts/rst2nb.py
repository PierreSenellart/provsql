#!/usr/bin/env python3
"""Generate Studio example notebooks (.ipynb) from the user-guide
tutorial / case-study .rst sources.

The .rst files stay the single source of truth; this script converts
each annotated file into a Jupyter-nbformat-v4 notebook that ProvSQL
Studio serves under "Open example…" (GET /api/nb/examples). Run via
`make notebooks` at the repository root; the generated files are
committed under studio/provsql_studio/notebooks/.

Annotation: reStructuredText *comments* of the form `.. nb:<key>[: v]`
(single colon, so Sphinx treats them as plain comments and the rendered
docs are unaffected):

  .. nb:name: tutorial          file-level: generate this file under
                                that name (required; unannotated files
                                are ignored)
  .. nb:database: tutorial      file-level: the database binding stamped
                                in metadata.provsql (default: the name)
  .. nb:setup: ../../tutorial/setup.sql
                                positional: splice the setup file at
                                this point as SQL cells (split on blank
                                lines, COPY ... FROM stdin blocks kept
                                whole -- the notebook runs them through
                                the COPY sub-protocol)
  .. nb:skip                    positional: drop the NEXT
                                code-block:: postgresql (display-only
                                snippets that should not become cells)
                                or admonition (e.g. the "try it in the
                                Playground" tips, meaningless inside a
                                notebook)
  .. nb:omit-begin / .. nb:omit-end
                                positional: drop everything between the
                                pair (e.g. the "download setup.sql and
                                run psql" preamble that the spliced
                                setup cells replace)
  .. nb:sql: SET timezone TO 'UTC';
                                positional: inject a one-statement SQL
                                cell (notebook-only replacements for
                                omitted psql-era passages)
  .. nb:md: Get familiar with the tables.
                                positional: inject a one-line Markdown
                                cell (notebook-only prose)
  .. nb:scheme: boolean         positional: run the NEXT postgresql
                                block under that provenance scheme
                                (semiring / where / boolean) -- the
                                cell-level counterpart of Studio's
                                three-way selector, e.g. for cs7's
                                boolean-provenance narrative
  .. nb:stop                    positional: ignore the rest of the file

Mapping:
  * `code-block:: postgresql` at top level  -> SQL cell
  * everything else (prose, bash/text blocks, lists, tables) -> Markdown
    cells via pandoc (rst -> gfm), one cell per run between SQL cells
  * admonitions (note/tip/warning) -> a bold label + blockquoted body
  * figure/image directives -> dropped (Studio screenshots; the prose
    around them stands alone); raw:: html blocks too (the docs'
    <details> spoiler wrappers: a notebook shows solutions inline)

Requires pandoc on PATH; the committed notebooks are generated with the
version pinned in .github/workflows/studio.yml (pandoc's rst -> gfm
rendering changes across releases, so the drift job fails on output
from any other version).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

SENTINEL = "NBCELLxSPLITx"  # survives pandoc as a plain paragraph

NB_DIRECTIVE_RE = re.compile(r"^\.\. nb:([a-z-]+)(?::\s*(.*))?\s*$")
CODE_BLOCK_RE = re.compile(r"^(\s*)\.\. code-block::\s*(\S+)\s*$")
ADMONITION_RE = re.compile(
    r"^\.\. (note|tip|warning|important|caution|hint|seealso)::\s*$")
ADMONITION_LABELS = {"seealso": "See also"}
FIGURE_RE = re.compile(r"^\.\. (figure|image|raw)::")
COPY_STDIN_RE = re.compile(r"^\s*COPY\s.+\bFROM\s+stdin\b.*;\s*$", re.IGNORECASE)


def _indent_of(line: str) -> int:
    return len(line) - len(line.lstrip())


def _consume_indented(lines: list[str], i: int, indent: int) -> tuple[list[str], int]:
    """Collect the directive body: lines blank or indented more than
    `indent`, stopping at the first non-blank line at <= indent."""
    body: list[str] = []
    while i < len(lines):
        line = lines[i]
        if line.strip() == "" or _indent_of(line) > indent:
            body.append(line)
            i += 1
        else:
            break
    # trim trailing blanks
    while body and body[-1].strip() == "":
        body.pop()
    return body, i


def _dedent(body: list[str]) -> list[str]:
    indents = [_indent_of(line) for line in body if line.strip()]
    cut = min(indents) if indents else 0
    return [line[cut:] if line.strip() else "" for line in body]


def split_setup_sql(text: str) -> list[str]:
    """Split a setup.sql into cell-sized chunks: blank lines separate
    chunks, except inside a COPY ... FROM stdin block (statement line +
    data rows + the backslash-dot terminator stay together).

    psql-loading boilerplate is dropped: `SET client_encoding` matters
    when piping the file through psql, not on a notebook kernel
    connection (psycopg is UTF-8 throughout); `CREATE EXTENSION`
    is covered by the binding banner's create-database action (which
    installs provsql CASCADE, pulling uuid-ossp along); and
    `SET search_path` is applied per cell by the kernel (Studio always
    keeps provsql reachable on the path)."""
    text = re.sub(r"^SET client_encoding\s*=.*;\s*$", "", text, flags=re.M)
    text = re.sub(r"^CREATE EXTENSION[^;]*;\s*$", "", text, flags=re.M)
    text = re.sub(r"^SET search_path[^;]*;\s*$", "", text, flags=re.M)
    chunks: list[str] = []
    cur: list[str] = []
    in_copy = False
    for line in text.splitlines():
        if in_copy:
            cur.append(line)
            if line.rstrip() == "\\.":
                in_copy = False
            continue
        if line.lstrip().startswith("\\"):
            # psql meta-commands (\echo banners, \copy): psql-session
            # furniture, meaningless in a notebook cell.
            continue
        if COPY_STDIN_RE.match(line):
            in_copy = True
            cur.append(line)
            continue
        if not line.strip():
            if cur:
                chunks.append("\n".join(cur))
                cur = []
            continue
        cur.append(line)
    if cur:
        chunks.append("\n".join(cur))
    # Drop comment-only chunks? No: leading comments document the cell.
    return [c for c in chunks if c.strip()]


def parse_rst(path: Path) -> tuple[dict, list[tuple[str, str]]] | None:
    """Parse one annotated .rst into (file_options, segments).

    segments is an ordered list of ("rst", text) prose runs,
    ("md", text) ready-made markdown (pandoc bypassed), and
    ("sql", text) cells. Returns None when the file carries no
    `.. nb:name:` annotation."""
    lines = path.read_text().splitlines()
    opts: dict[str, str] = {}
    segments: list[tuple] = []
    prose: list[str] = []
    skip_next = False
    next_scheme: str | None = None
    i = 0

    def flush_prose() -> None:
        nonlocal prose
        if any(line.strip() for line in prose):
            segments.append(("rst", "\n".join(prose)))
        prose = []

    while i < len(lines):
        line = lines[i]

        m = NB_DIRECTIVE_RE.match(line)
        if m:
            key, value = m.group(1), (m.group(2) or "").strip()
            if key in ("name", "database"):
                opts[key] = value
            elif key == "setup":
                setup_path = (path.parent / value).resolve()
                flush_prose()
                segments.append(("md",
                    "*The following cells set up the database with all "
                    "the content this notebook requires; run them first, "
                    "ideally on a fresh database.*"))
                for chunk in split_setup_sql(setup_path.read_text()):
                    chunk_lines = chunk.splitlines()
                    if all(ln.lstrip().startswith("--")
                           for ln in chunk_lines if ln.strip()):
                        # Comment-only chunk: prose, not a runnable
                        # cell. The setup files' header banners (title
                        # + "run psql -f setup.sql" instructions)
                        # duplicate the notebook title and the psql
                        # loading story, so those are dropped; interior
                        # comment headers convert to markdown.
                        md = "\n".join(
                            re.sub(r"^\s*--\s?", "", ln)
                            for ln in chunk_lines)
                        if re.match(r"\s*(Case Study \d|Tutorial)", md):
                            pass
                        else:
                            segments.append(("md", md))
                    else:
                        segments.append(("sql", chunk))
            elif key == "sql":
                flush_prose()
                segments.append(("sql", value))
            elif key == "md":
                flush_prose()
                segments.append(("md", value))
            elif key == "scheme":
                if value not in ("semiring", "where", "boolean"):
                    sys.exit(f"{path}:{i + 1}: invalid nb:scheme {value!r}")
                next_scheme = value
            elif key == "skip":
                skip_next = True
            elif key == "omit-begin":
                j = i + 1
                while j < len(lines):
                    mm = NB_DIRECTIVE_RE.match(lines[j])
                    if mm and mm.group(1) == "omit-end":
                        break
                    j += 1
                else:
                    sys.exit(f"{path}:{i + 1}: nb:omit-begin without nb:omit-end")
                i = j + 1
                continue
            elif key == "omit-end":
                sys.exit(f"{path}:{i + 1}: nb:omit-end without nb:omit-begin")
            elif key == "stop":
                break
            else:
                sys.exit(f"{path}:{i + 1}: unknown nb directive {key!r}")
            i += 1
            continue

        m = CODE_BLOCK_RE.match(line)
        if m and m.group(1) == "" and m.group(2) in ("postgresql", "sql"):
            body, j = _consume_indented(lines, i + 1, 0)
            # drop directive options (:caption: ...) heading the body
            while body and re.match(r"^\s*:\w+:", body[0]):
                body.pop(0)
            while body and body[0].strip() == "":
                body.pop(0)
            if skip_next:
                # Display-only snippet: keep it in the prose as a fence.
                prose.append(line)
                prose.extend(lines[i + 1:j])
                skip_next = False
            else:
                flush_prose()
                if next_scheme:
                    segments.append(("sql", "\n".join(_dedent(body)),
                                     {"scheme": next_scheme}))
                    next_scheme = None
                else:
                    segments.append(("sql", "\n".join(_dedent(body))))
            i = j
            continue

        m = ADMONITION_RE.match(line)
        if m:
            body, j = _consume_indented(lines, i + 1, 0)
            if skip_next:
                skip_next = False
                i = j
                continue
            label = ADMONITION_LABELS.get(m.group(1), m.group(1).capitalize())
            prose.append("")
            prose.append(f"**{label}:**")
            prose.append("")
            prose.extend(body)  # stays indented -> pandoc blockquote
            prose.append("")
            i = j
            continue

        if FIGURE_RE.match(line):
            _, j = _consume_indented(lines, i + 1, 0)
            i = j
            continue

        prose.append(line)
        i += 1

    flush_prose()
    if "name" not in opts:
        return None
    return opts, segments


def rst_prose_to_markdown_cells(prose_segments: list[str]) -> list[str]:
    """pandoc the prose runs as ONE document (heading levels depend on
    seeing every underline style in order), separated by sentinel
    paragraphs, then split the gfm output back into per-run cells."""
    joined = ("\n\n" + SENTINEL + "\n\n").join(prose_segments)
    # rst+smart applies typographic substitutions the docs get from Sphinx's
    # smart_quotes: ``--`` becomes an en-dash, straight quotes curl, ``...``
    # becomes an ellipsis -- and it is code-aware, so an inline literal such
    # as ``--wrap=none`` is left untouched.
    out = subprocess.run(
        ["pandoc", "-f", "rst+smart", "-t", "gfm", "--wrap=none"],
        input=joined.encode(), capture_output=True, check=True,
    ).stdout.decode()
    parts = [p.strip() for p in re.split(rf"^{SENTINEL}$", out, flags=re.M)]
    # pandoc -t gfm emits GitHub-flavoured math -- inline ``$`...`$`` and
    # display fenced ```` ```math ... ``` ```` -- but the Jupyter / Studio
    # notebook renderer is MathJax, which wants ``$...$`` / ``$$...$$``;
    # convert so the LaTeX renders instead of appearing as verbatim source.
    parts = [re.sub(r"```+\s*math\s*\n(.*?)\n```+", r"$$\n\1\n$$", p, flags=re.S)
             for p in parts]
    parts = [re.sub(r"\$`([^`]+?)`\$", r"$\1$", p) for p in parts]
    # pandoc litter that adds nothing in a notebook context
    parts = [re.sub(r"^<!-- end list -->$", "", p, flags=re.M) for p in parts]
    # Sphinx roles pandoc cannot resolve. Explicit-target references
    # (:ref:`Step 13 <step-13-shapley>`, :doc:`the Studio chapter
    # <studio>`) keep their target in the output; render just the link
    # text, as the docs do. Bare-target :doc:/:ref: come out
    # indistinguishable from inline code, so the .rst sources give a
    # readable explicit title to every such reference that reaches a
    # notebook.
    parts = [re.sub(r"`([^`<>]+?)\s<[\w./-]+>`(?!_)",
                    lambda m: " ".join(m.group(1).split()), p)
             for p in parts]
    # :cite: keys come out as bare DBLP:... tokens; drop them (the
    # surrounding prose is phrased to carry the reference on its own).
    parts = [re.sub(r"\s*\bDBLP:[\w/-]+", "", p) for p in parts]
    return parts


def build_notebook(opts: dict, segments: list[tuple[str, str]]) -> dict:
    prose_runs = [seg[1] for seg in segments if seg[0] == "rst"]
    md_cells = rst_prose_to_markdown_cells(prose_runs) if prose_runs else []
    md_iter = iter(md_cells)

    def to_lines(text: str) -> list[str]:
        parts = text.split("\n")
        return [p + "\n" for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])

    cells = []
    for seg in segments:
        kind, text = seg[0], seg[1]
        seg_meta = seg[2] if len(seg) > 2 else {}
        if kind == "rst":
            md = next(md_iter)
            if md.strip():
                cells.append({"cell_type": "markdown", "metadata": {},
                              "source": to_lines(md)})
        elif kind == "md":
            cells.append({"cell_type": "markdown", "metadata": {},
                          "source": to_lines(text)})
        else:
            cells.append({"cell_type": "code", "execution_count": None,
                          "metadata": {"provsql": dict(seg_meta)},
                          "source": to_lines(text), "outputs": []})
    return {
        "nbformat": 4,
        "nbformat_minor": 5,
        "metadata": {
            "kernelspec": {"name": "provsql-studio",
                           "display_name": "ProvSQL (SQL)", "language": "sql"},
            "language_info": {"name": "sql"},
            "provsql": {
                "scheme": "semiring",
                "database": opts.get("database", opts["name"]),
                "generated_from": opts["source"],
            },
        },
        "cells": cells,
    }


def main() -> None:
    repo = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", type=Path, default=repo / "doc" / "source" / "user",
                    help="directory scanned for annotated .rst files")
    ap.add_argument("--out", type=Path,
                    default=repo / "studio" / "provsql_studio" / "notebooks",
                    help="output directory for the .ipynb files")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    generated = []
    for rst in sorted(args.src.glob("*.rst")):
        parsed = parse_rst(rst)
        if parsed is None:
            continue
        opts, segments = parsed
        opts["source"] = str(rst.relative_to(repo))
        nb = build_notebook(opts, segments)
        out_path = args.out / f"{opts['name']}.ipynb"
        out_path.write_text(json.dumps(nb, indent=1, ensure_ascii=False) + "\n")
        n_sql = sum(1 for c in nb["cells"] if c["cell_type"] == "code")
        n_md = len(nb["cells"]) - n_sql
        generated.append(opts["name"])
        print(f"{rst.name} -> {out_path.relative_to(repo)} "
              f"({n_sql} SQL + {n_md} Markdown cells)")
    if not generated:
        sys.exit("no annotated .rst files found (missing `.. nb:name:` markers?)")


if __name__ == "__main__":
    main()
