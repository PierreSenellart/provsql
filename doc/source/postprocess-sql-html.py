#!/usr/bin/env python3
"""Post-process Doxygen HTML/JS for the SQL API.

Transforms the C-style Doxygen output into idiomatic SQL rendering:
- Restore SQL type names (DOUBLE PRECISION, type[], SETOF type)
- Use dot notation for schema-qualified names (provsql.f instead of provsql::f)
- Uppercase SQL types (uuid -> UUID, text -> TEXT, etc.)
- Replace C keywords with SQL equivalents (struct -> TYPE, enum -> ENUM)
- Replace & (C reference) with OUT (SQL output parameter)
"""

import re
import sys
from pathlib import Path

# Ordered list of (pattern, replacement) applied globally.
# Order matters: more specific patterns must come before general ones.
REPLACEMENTS = [
    # Restore multi-word and array types (from filter normalization)
    (r"\bDOUBLE_PRECISION\b", "DOUBLE PRECISION"),
    (r"\bCHARACTER_VARYING\b", "CHARACTER VARYING"),
    (r"\b(\w+)_array\b", r"\1[]"),
    (r"\bSETOF_(\w+)\b", r"SETOF \1"),

    # Schema separator: :: -> . (but not inside URLs or HTML entities)
    (r"provsql::", "provsql."),

    # C keywords -> SQL keywords
    (r"\bstruct\b", "TYPE"),
    (r"\benum\b", "ENUM"),
    # "Struct Reference" page title/heading -> TABLE Reference for tables, TYPE Reference for types
    (r"\bupdate_provenance\b Struct Reference", "update_provenance TABLE Reference"),
    (r"Struct Reference", "TYPE Reference"),

    # C-style CAST function names -> SQL CAST syntax
    # e.g. CAST_agg_token_TO_UUID -> CAST(AGG_TOKEN AS UUID)
    (r'\bCAST_([a-z_]+)_TO_([A-Za-z_]+)\b',
     lambda m: f'CAST({m.group(1).upper()} AS {m.group(2).upper()})'),

    # Uppercase SQL types (only whole words, case-sensitive for lowercase)
    (r"\bvoid\b", "VOID"),
    (r"\btimestamp\b", "TIMESTAMP"),
    (r"\bbool\b", "BOOL"),
    (r"\bboolean\b", "BOOLEAN"),
    (r"\bint\b", "INT"),
    (r"\binteger\b", "INTEGER"),
    (r"\btext\b", "TEXT"),
    (r"\buuid\b", "UUID"),
    (r"\brecord\b", "RECORD"),
    (r"\bcstring\b", "CSTRING"),
    (r"\bnumeric\b", "NUMERIC"),
    (r"\bregclass\b", "REGCLASS"),
    (r"\bregproc\b", "REGPROC"),
    (r"\bregtype\b", "REGTYPE"),
    (r"\btimestamptz\b", "TIMESTAMPTZ"),
    (r"\btstzmultirange\b", "TSTZMULTIRANGE"),
    (r"\banyelement\b", "ANYELEMENT"),
    (r"\bagg_token\b", "AGG_TOKEN"),
    (r"\bgate_with_desc\b", "GATE_WITH_DESC"),
    (r"\bprovenance_gate\b", "PROVENANCE_GATE"),
    (r"\bquery_type_enum\b", "QUERY_TYPE_ENUM"),

    # OUT parameters: "TYPE &amp;" or "TYPE &name" -> "OUT TYPE" / "OUT TYPE name"
    # Handle multi-word types like DOUBLE PRECISION & before single-word types
    (r"(DOUBLE PRECISION)\s+&amp;\s*(\w+)", r"OUT \1 \2"),
    (r"(DOUBLE PRECISION)\s+&amp;", r"OUT \1"),
    (r"(\b[A-Z_]+)\s+&amp;\s*(\w+)", r"OUT \1 \2"),
    (r"(\b[A-Z_]+)\s+&amp;", r"OUT \1"),

    # C++ terminology -> SQL terminology
    (r"Data Structure Index", "Type Index"),
    (r"Data Structures", "Types"),
    (r"Data Fields", "Attributes"),
    (r"Namespace Members", "Schema Members"),
    (r"Namespace Reference", "Schema Reference"),
    (r"Namespace List", "Schema List"),
    (r"Namespaces", "Schemas"),

    # Remove Doxygen's auto-generated "Definition at line X of file Y." links.
    # These use filter-output line numbers (not original SQL lines) and are
    # replaced by our custom @par Source code links with correct line numbers.
    (r'<p class="definition">Definition at line.*?</p>\n?', ""),

    # Fix false positives: restore HTML/MIME types broken by uppercasing
    (r"TEXT/javascript", "text/javascript"),
    (r"TEXT/css", "text/css"),
    (r"TEXT/html", "text/html"),
    (r"TEXT/xml", "text/xml"),
    (r"TEXT/plain", "text/plain"),
    (r"TEXT/xhtml", "text/xhtml"),
    (r'"TEXT"', '"text"'),
    (r"\bTEXT:", "text:"),
]


def process_file(path):
    content = path.read_text(encoding="utf-8")
    original = content
    for pattern, replacement in REPLACEMENTS:
        content = re.sub(pattern, replacement, content)
    if content != original:
        path.write_text(content, encoding="utf-8")
        return True
    return False


def main():
    html_dir = Path(__file__).parent.parent / "doxygen-sql" / "html"
    if not html_dir.is_dir():
        print(f"Error: {html_dir} not found", file=sys.stderr)
        sys.exit(1)

    # Only process HTML and Doxygen-generated content JS files.
    # Exclude framework JS (jquery.js, navtree.js, etc.) to avoid corruption.
    content_js_patterns = [
        "group__*.js", "namespace*.js", "struct*.js", "provsql_*.js",
        "topics.js", "navtreedata.js", "navtreeindex*.js",
        "annotated_dup.js", "files_dup.js", "namespaces_dup.js",
        "menudata.js", "dir_*.js",
        "search/*.js",
    ]
    files = list(html_dir.rglob("*.html"))
    for pattern in content_js_patterns:
        files.extend(html_dir.glob(pattern))

    modified = 0
    for f in files:
        if process_file(f):
            modified += 1

    print(f"Post-processed {modified} HTML/JS files in {html_dir}")


if __name__ == "__main__":
    main()
