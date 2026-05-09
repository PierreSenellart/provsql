#!/usr/bin/env python3
"""Check coherence between :sqlfunc:/:cfunc: references, function maps, and Doxygen output."""

import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
USER_DOCS = SCRIPT_DIR / "user"
DEV_DOCS = SCRIPT_DIR / "dev"
CONF_PY = SCRIPT_DIR / "conf.py"
DOXYGEN_SQL_HTML = SCRIPT_DIR.parent / "doxygen-sql" / "html"
DOXYGEN_C_HTML = SCRIPT_DIR.parent / "doxygen-c" / "html"

errors = []
conf_text = CONF_PY.read_text()

# ============================================================================
# :sqlfunc: checks
# ============================================================================

# 1. Extract all :sqlfunc: references from .rst files (user + dev docs)
sql_refs = set()
for doc_dir in [USER_DOCS, DEV_DOCS]:
    if doc_dir.exists():
        for rst in doc_dir.glob("*.rst"):
            for match in re.finditer(r":sqlfunc:`([^`]+)`", rst.read_text()):
                sql_refs.add(match.group(1).rstrip("()"))

# 2. Extract _SQL_FUNC_MAP keys and URLs from conf.py
sql_map_block = conf_text.split("_SQL_FUNC_MAP")[1].split("}")[0]
sql_map_entries = {}
for match in re.finditer(r"'([^']+)'\s*:\s*'([^']+)'", sql_map_block):
    sql_map_entries[match.group(1)] = match.group(2)

# 3. Check: every :sqlfunc: ref has an entry in the map
for ref in sorted(sql_refs):
    if ref not in sql_map_entries:
        errors.append(f":sqlfunc:`{ref}` used in docs but missing from _SQL_FUNC_MAP in conf.py")

# 4. Check: every map entry is referenced in docs
for func in sorted(sql_map_entries):
    if func not in sql_refs:
        errors.append(f"_SQL_FUNC_MAP['{func}'] is not referenced by any :sqlfunc: in the documentation")

# 5. Check: every map entry points to an existing Doxygen anchor
for func, url in sorted(sql_map_entries.items()):
    parts = url.split("#")
    if len(parts) != 2:
        errors.append(f"_SQL_FUNC_MAP['{func}']: URL has no anchor: {url}")
        continue
    html_file = DOXYGEN_SQL_HTML / parts[0].split("/doxygen-sql/html/")[-1]
    anchor = parts[1]
    if not html_file.exists():
        errors.append(f"_SQL_FUNC_MAP['{func}']: HTML file not found: {html_file}")
        continue
    html_text = html_file.read_text()
    if f'id="{anchor}"' not in html_text and f'name="{anchor}"' not in html_text:
        errors.append(f"_SQL_FUNC_MAP['{func}']: anchor #{anchor} not found in {html_file.name}")

# 6. Check: every Doxygen SQL function is in the map (or explicitly excluded)
INTERNAL_FUNCTIONS = {
    # Triggers
    'insert_statement_trigger',
    'update_statement_trigger', 'delete_statement_trigger',
    # agg_token type internals (I/O, casts, operators)
    'agg_token_in', 'agg_token_out', 'agg_token_cast',
    'agg_token_to_float', 'agg_token_to_float8',
    'agg_token_to_int', 'agg_token_to_int4', 'agg_token_to_int8',
    'agg_token_to_numeric',
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
    'provenance_arith',
    'provenance_delta', 'provenance_semimod',
    'provenance_evaluate_compiled',
    # Internal constants and utilities
    'uuid_ns_provsql', 'epsilon',
    'reset_constants_cache', 'get_nb_gates',
    # Internal circuit inspection
    'sub_circuit_for_where', 'sub_circuit_with_desc',
    # Internal gate manipulation (set_* are dangerous for users)
    'set_extra', 'set_infos',
    # Transition functions for aggregates
    'choose_function',
    'union_tstzintervals_plus', 'union_tstzintervals_plus_state',
    'union_tstzintervals_times', 'union_tstzintervals_times_state',
    'union_tstzintervals_monus',
    # Temporal internals
    'update_provenance',
    # GUC variables (not functions)
    'aggtoken_text_as_uuid',
    # random_variable type internals (I/O, accessors, internal builder,
    # parameter-validation helper)
    'random_variable_in', 'random_variable_out',
    'random_variable_uuid', 'random_variable_value',
    'random_variable_make',
    'is_finite_float8',
    # User-facing constructors for continuous random variables.
    # Promote to _SQL_FUNC_MAP once the continuous-distributions
    # chapter of the user manual is written and references them via
    # :sqlfunc:.
    'normal', 'uniform', 'exponential', 'as_random',
    # random_variable arithmetic and comparison operator implementations
    # (invoked through the SQL operators + - * / < <= = <> >= >, not
    # called by name; promote alongside the constructors in priority 9).
    'random_variable_plus', 'random_variable_minus',
    'random_variable_times', 'random_variable_div',
    'random_variable_neg',
    'random_variable_lt', 'random_variable_le', 'random_variable_eq',
    'random_variable_ne', 'random_variable_ge', 'random_variable_gt',
    'random_variable_cmp_oid',
    # Doxygen artefacts (not actual functions)
    'org', 'sql', 'html',
}

if DOXYGEN_SQL_HTML.exists():
    doxygen_funcs = set()
    for html_file in DOXYGEN_SQL_HTML.glob("group__*.html"):
        for match in re.finditer(r'provsql\.([a-z0-9_]+)', html_file.read_text()):
            fname = match.group(1)
            if fname not in INTERNAL_FUNCTIONS:
                doxygen_funcs.add(fname)

    for func in sorted(doxygen_funcs):
        if func not in sql_map_entries:
            errors.append(f"Doxygen documents '{func}' but it is missing from _SQL_FUNC_MAP")

# ============================================================================
# :cfunc: checks
# ============================================================================

# 7. Extract all :cfunc: references from .rst files (dev + user docs)
c_refs = set()
for doc_dir in [DEV_DOCS, USER_DOCS]:
    if doc_dir.exists():
        for rst in doc_dir.glob("*.rst"):
            for match in re.finditer(r":cfunc:`([^`]+)`", rst.read_text()):
                c_refs.add(match.group(1).rstrip("()"))

# 8. Extract _C_FUNC_MAP keys and URLs from conf.py
c_map_block = conf_text.split("_C_FUNC_MAP")[1].split("}")[0]
c_map_entries = {}
for match in re.finditer(r"'([^']+)'\s*:\s*'([^']+)'", c_map_block):
    c_map_entries[match.group(1)] = match.group(2)

# 9. Check: every :cfunc: ref has an entry in the map
for ref in sorted(c_refs):
    if ref not in c_map_entries:
        errors.append(f":cfunc:`{ref}` used in docs but missing from _C_FUNC_MAP in conf.py")

# 10. Check: every map entry is referenced in docs
for func in sorted(c_map_entries):
    if func not in c_refs:
        errors.append(f"_C_FUNC_MAP['{func}'] is not referenced by any :cfunc: in the documentation")

# 11. Check: every map entry points to an existing Doxygen anchor
for func, url in sorted(c_map_entries.items()):
    parts = url.split("#")
    html_file = DOXYGEN_C_HTML / parts[0].split("/doxygen-c/html/")[-1]
    if not html_file.exists():
        errors.append(f"_C_FUNC_MAP['{func}']: HTML file not found: {html_file}")
        continue
    if len(parts) == 2:
        anchor = parts[1]
        html_text = html_file.read_text()
        if f'id="{anchor}"' not in html_text and f'name="{anchor}"' not in html_text:
            errors.append(f"_C_FUNC_MAP['{func}']: anchor #{anchor} not found in {html_file.name}")

# ============================================================================
# :cfile: checks
# ============================================================================

# 12. Extract all :cfile: references from .rst files
cfile_refs = set()
for doc_dir in [DEV_DOCS, USER_DOCS]:
    if doc_dir.exists():
        for rst in doc_dir.glob("*.rst"):
            for match in re.finditer(r":cfile:`([^`]+)`", rst.read_text()):
                cfile_refs.add(match.group(1))

# 13. Check: every :cfile: ref points to an existing Doxygen HTML page
#     (URL is computed from filename: _ -> __, . -> _8)
for ref in sorted(cfile_refs):
    base, ext = ref.rsplit('.', 1)
    doxy_name = base.replace('_', '__') + '_8' + ext + '.html'
    html_file = DOXYGEN_C_HTML / doxy_name
    if not html_file.exists():
        errors.append(f":cfile:`{ref}`: Doxygen page not found: {html_file}")

# ============================================================================
# :sqlfile: checks
# ============================================================================

# 14. Extract all :sqlfile: references from .rst files
sqlfile_refs = set()
for doc_dir in [DEV_DOCS, USER_DOCS]:
    if doc_dir.exists():
        for rst in doc_dir.glob("*.rst"):
            for match in re.finditer(r":sqlfile:`([^`]+)`", rst.read_text()):
                sqlfile_refs.add(match.group(1))

# 15. Check: every :sqlfile: ref points to an existing Doxygen HTML page
for ref in sorted(sqlfile_refs):
    base, ext = ref.rsplit('.', 1)
    doxy_name = base.replace('_', '__') + '_8' + ext + '.html'
    html_file = DOXYGEN_SQL_HTML / doxy_name
    if not html_file.exists():
        errors.append(f":sqlfile:`{ref}`: Doxygen page not found: {html_file}")

# ============================================================================
# Report
# ============================================================================

if errors:
    print("doc link check FAILED:", file=sys.stderr)
    for e in errors:
        print(f"  - {e}", file=sys.stderr)
    sys.exit(1)
else:
    print(f"doc link check OK: sqlfunc {len(sql_refs)} refs / {len(sql_map_entries)} map; "
          f"cfunc {len(c_refs)} refs / {len(c_map_entries)} map; "
          f"cfile {len(cfile_refs)} refs; "
          f"sqlfile {len(sqlfile_refs)} refs; all anchors valid.")
