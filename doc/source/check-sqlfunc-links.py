#!/usr/bin/env python3
"""Check coherence between :sqlfunc: references, _SQL_FUNC_MAP, and Doxygen output."""

import re
import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
USER_DOCS = SCRIPT_DIR / "user"
CONF_PY = SCRIPT_DIR / "conf.py"
DOXYGEN_HTML = SCRIPT_DIR.parent / "doxygen-sql" / "html"

errors = []

# 1. Extract all :sqlfunc: references from .rst files
refs = set()
for rst in USER_DOCS.glob("*.rst"):
    for match in re.finditer(r":sqlfunc:`([^`]+)`", rst.read_text()):
        refs.add(match.group(1).rstrip("()"))

# 2. Extract _SQL_FUNC_MAP keys and URLs from conf.py
conf_text = CONF_PY.read_text()
map_block = conf_text.split("_SQL_FUNC_MAP")[1].split("}")[0]
map_entries = {}
for match in re.finditer(r"'([^']+)'\s*:\s*'([^']+)'", map_block):
    map_entries[match.group(1)] = match.group(2)

# 3. Check: every :sqlfunc: ref has an entry in the map
for ref in sorted(refs):
    if ref not in map_entries:
        errors.append(f":sqlfunc:`{ref}` used in docs but missing from _SQL_FUNC_MAP in conf.py")

# 4. Check: every map entry is referenced in docs
for func in sorted(map_entries):
    if func not in refs:
        errors.append(f"_SQL_FUNC_MAP['{func}'] is not referenced by any :sqlfunc: in the documentation")

# 5. Check: every map entry points to an existing Doxygen anchor
for func, url in sorted(map_entries.items()):
    # URL format: /doxygen-sql/html/group__xxx.html#gaXXX
    parts = url.split("#")
    if len(parts) != 2:
        errors.append(f"_SQL_FUNC_MAP['{func}']: URL has no anchor: {url}")
        continue
    html_file = DOXYGEN_HTML / parts[0].split("/doxygen-sql/html/")[-1]
    anchor = parts[1]
    if not html_file.exists():
        errors.append(f"_SQL_FUNC_MAP['{func}']: HTML file not found: {html_file}")
        continue
    html_text = html_file.read_text()
    if f'id="{anchor}"' not in html_text and f'name="{anchor}"' not in html_text:
        errors.append(f"_SQL_FUNC_MAP['{func}']: anchor #{anchor} not found in {html_file.name}")

# 6. Check: every Doxygen SQL function is in the map (or explicitly excluded)
#    Internal functions that are not user-facing and don't need documentation:
INTERNAL_FUNCTIONS = {
    # Triggers
    'add_gate_trigger', 'insert_statement_trigger',
    'update_statement_trigger', 'delete_statement_trigger',
    # agg_token type internals (I/O, casts, operators)
    'agg_token_in', 'agg_token_out', 'agg_token_cast',
    'agg_token_to_float', 'agg_token_to_int', 'agg_token_to_numeric',
    'agg_token_to_text', 'agg_token_uuid',
    'agg_token_eq_numeric', 'agg_token_ne_numeric',
    'agg_token_lt_numeric', 'agg_token_le_numeric',
    'agg_token_gt_numeric', 'agg_token_ge_numeric',
    'agg_token_comp_numeric',
    'numeric_eq_agg_token', 'numeric_ne_agg_token',
    'numeric_lt_agg_token', 'numeric_le_agg_token',
    'numeric_gt_agg_token', 'numeric_ge_agg_token',
    'numeric_comp_agg_token',
    # Internal circuit operations
    'provenance_plus', 'provenance_times', 'provenance_monus',
    'provenance_project', 'provenance_eq', 'provenance_cmp',
    'provenance_delta', 'provenance_semimod', 'provenance_aggregate',
    'provenance_evaluate_compiled',
    # Internal constants and utilities
    'uuid_ns_provsql', 'epsilon',
    'reset_constants_cache', 'get_nb_gates',
    # Internal circuit inspection
    'sub_circuit_for_where', 'sub_circuit_with_desc',
    # Internal gate manipulation (set_* are dangerous for users,
    # create_gate is low-level circuit construction)
    'set_extra', 'set_infos', 'create_gate',
    # Transition functions for aggregates
    'choose_function',
    'union_tstzintervals_plus', 'union_tstzintervals_plus_state',
    'union_tstzintervals_times', 'union_tstzintervals_times_state',
    'union_tstzintervals_monus',
    # Temporal internals
    'replace_the_circuit', 'update_provenance',
    # Doxygen artefacts (not actual functions)
    'org', 'sql', 'html',
}

if DOXYGEN_HTML.exists():
    doxygen_funcs = set()
    for html_file in DOXYGEN_HTML.glob("group__*.html"):
        for match in re.finditer(r'provsql\.([a-z_]+)', html_file.read_text()):
            fname = match.group(1)
            if fname not in INTERNAL_FUNCTIONS:
                doxygen_funcs.add(fname)

    for func in sorted(doxygen_funcs):
        if func not in map_entries:
            errors.append(f"Doxygen documents '{func}' but it is missing from _SQL_FUNC_MAP")

if errors:
    print("sqlfunc link check FAILED:", file=sys.stderr)
    for e in errors:
        print(f"  - {e}", file=sys.stderr)
    sys.exit(1)
else:
    print(f"sqlfunc link check OK: {len(refs)} doc references, {len(map_entries)} map entries, all anchors valid.")
