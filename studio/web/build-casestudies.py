#!/usr/bin/env python3
"""Convert the tutorial / case-study setup.sql scripts (under doc/) into SQL the
browser PGlite build can run via pg.exec().

Two constructs in the canonical scripts are psql client features that PGlite's
query protocol does not accept, so they are rewritten to plain INSERTs:

  * ``COPY <tbl> (<cols>) FROM stdin;`` + tab-separated rows + ``\\.``
  * ``\\copy <tbl> FROM '<file>.csv' WITH CSV HEADER;`` (the CSV ships in the
    study's data/ directory)

Everything else (CREATE / INSERT / ALTER / SET / SELECT add_provenance ...) is
passed through unchanged. One case study is skipped: casestudy3 loads a large
GTFS dataset that must be downloaded separately.

Outputs (git-ignored) into studio/web/casestudies/:
  <name>.sql       one per database (tutorial, cs1, cs2, cs4, cs5, cs6, cs7)
  manifest.json    [{name, file, title}, ...] consumed by studio-boot.js
"""
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DOC = os.path.normpath(os.path.join(HERE, "..", "..", "doc"))
OUT = os.path.join(HERE, "casestudies")

# (database name, source directory under doc/, human title). casestudy3 is
# intentionally absent: its GTFS data is a separate multi-megabyte download.
STUDIES = [
    ("tutorial", "tutorial",   "Tutorial"),
    ("cs1",      "casestudy1", "CS1: The Intelligence Agency"),
    ("cs2",      "casestudy2", "CS2: Medical Evidence Synthesis"),
    ("cs4",      "casestudy4", "CS4: Government Ministers (temporal)"),
    ("cs5",      "casestudy5", "CS5: Wildlife Photo Archive"),
    ("cs6",      "casestudy6", "CS6: City Air-Quality Sensor Network"),
    ("cs7",      "casestudy7", "CS7: Peer-Review Assignment"),
]

# Notebooks that build their own tables inline (no doc/<dir>/setup.sql): they
# only need an empty, provsql-enabled database to exist in the cluster so the
# ?nb=<name> deep link binds to an existing database rather than offering a
# "create" banner. The notebook's own setup cells seed the data. (name, title)
SELF_SEEDING = [
    ("cs8", "CS8: ProvSQL as a Probability Calculator"),
]

_COPY_RE = re.compile(r"^\s*COPY\s+(\S+)\s*\(([^)]*)\)\s+FROM\s+stdin\s*;", re.I)
_PSQL_COPY_RE = re.compile(
    r"^\s*\\copy\s+(\S+)\s+FROM\s+'([^']+)'\s+WITH\s+CSV\s+HEADER\s*;?", re.I)


def _lit(v):
    """A single COPY/CSV field as a SQL literal (``\\N`` / empty -> NULL)."""
    if v is None or v == r"\N":
        return "NULL"
    # COPY text format escapes; the case-study data only uses \\N, plain tabs
    # as the delimiter and literal text, so a quote-escape is enough.
    return "'" + v.replace("\\\\", "\\").replace("'", "''") + "'"


def _inserts(table, cols, rows):
    """Emit one multi-row INSERT (chunked) for the captured rows."""
    out = []
    collist = "(%s)" % ", ".join(cols) if cols else ""
    for i in range(0, len(rows), 200):
        chunk = rows[i:i + 200]
        values = ",\n  ".join("(" + ", ".join(_lit(c) for c in r) + ")" for r in chunk)
        out.append("INSERT INTO %s %s VALUES\n  %s;" % (table, collist, values))
    return out


def split_statements(sql):
    """Split a SQL script into individual statements on top-level semicolons.

    Needed because PGlite runs a whole multi-statement exec() as ONE
    transaction, so an ``ON COMMIT DROP`` temp table created by
    provsql.create_provenance_mapping (and re-created on the next call) would
    collide. Run one statement per exec instead -> each autocommits and the
    temp table is dropped between calls, matching how psql loads the script.

    Tracks single-quoted strings, double-quoted identifiers, dollar-quoted
    blocks and -- / block comments so a semicolon inside any of them does not
    split. (The case-study scripts use none of dollar-quotes, but it is cheap
    to be correct.)"""
    stmts, buf = [], []
    i, n = 0, len(sql)
    sq = dq = lc = bc = False
    dollar = None
    while i < n:
        c = sql[i]
        two = sql[i:i + 2]
        if lc:
            buf.append(c)
            if c == "\n":
                lc = False
        elif bc:
            buf.append(c)
            if two == "*/":
                buf.append("/")
                i += 2
                bc = False
                continue
        elif sq:
            buf.append(c)
            if c == "'":
                sq = False
        elif dq:
            buf.append(c)
            if c == '"':
                dq = False
        elif dollar is not None:
            if sql.startswith(dollar, i):
                buf.append(dollar)
                i += len(dollar)
                dollar = None
                continue
            buf.append(c)
        elif two == "--":
            buf.append(two)
            i += 2
            lc = True
            continue
        elif two == "/*":
            buf.append(two)
            i += 2
            bc = True
            continue
        elif c == "'":
            buf.append(c)
            sq = True
        elif c == '"':
            buf.append(c)
            dq = True
        elif c == "$":
            m = re.match(r"\$[A-Za-z_0-9]*\$", sql[i:])
            if m:
                dollar = m.group(0)
                buf.append(dollar)
                i += len(dollar)
                continue
            buf.append(c)
        elif c == ";":
            s = "".join(buf).strip()
            if s:
                stmts.append(s)
            buf = []
        else:
            buf.append(c)
        i += 1
    tail = "".join(buf).strip()
    if tail:
        stmts.append(tail)
    # Drop statements that are only comments / whitespace.
    return [s for s in stmts if not all(
        ln.strip() == "" or ln.strip().startswith("--")
        for ln in s.splitlines())]


def convert(src_dir):
    path = os.path.join(DOC, src_dir, "setup.sql")
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()

    out = []
    i = 0
    while i < len(lines):
        line = lines[i]

        m = _COPY_RE.match(line)
        if m:
            table, cols = m.group(1), [c.strip() for c in m.group(2).split(",")]
            i += 1
            rows = []
            while i < len(lines) and lines[i].strip() != r"\.":
                rows.append(lines[i].split("\t"))
                i += 1
            i += 1  # skip the \. terminator
            out.extend(_inserts(table, cols, rows))
            continue

        m = _PSQL_COPY_RE.match(line)
        if m:
            table, fname = m.group(1), m.group(2)
            csv_path = os.path.join(DOC, src_dir, "data", fname)
            with open(csv_path, encoding="utf-8", newline="") as cf:
                reader = csv.reader(cf)
                header = next(reader)
                rows = [[None if c == "" else c for c in row] for row in reader]
            out.extend(_inserts(table, header, rows))
            i += 1
            continue

        # Drop psql meta-commands (\echo, ...): they are not SQL. COPY / \copy
        # blocks are handled above, so any remaining backslash line is a
        # client-only directive with no effect on the loaded data.
        if re.match(r"^\s*\\", line):
            i += 1
            continue

        out.append(line)
        i += 1

    return "\n".join(out) + "\n"


def main():
    os.makedirs(OUT, exist_ok=True)
    manifest = []
    for name, src_dir, title in STUDIES:
        stmts = split_statements(convert(src_dir))
        with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as f:
            json.dump(stmts, f)
        manifest.append({"name": name, "file": name + ".json", "title": title})
        print("  %-10s <- doc/%s/setup.sql (%d statements)" % (name, src_dir, len(stmts)))
    for name, title in SELF_SEEDING:
        with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as f:
            json.dump([], f)
        manifest.append({"name": name, "file": name + ".json", "title": title})
        print("  %-10s <- (empty; notebook self-seeds its tables)" % name)
    with open(os.path.join(OUT, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print("build-casestudies.py: wrote %d databases + manifest.json to %s"
          % (len(manifest), OUT))


if __name__ == "__main__":
    sys.exit(main())
