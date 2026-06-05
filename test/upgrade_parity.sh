#!/usr/bin/env bash
#
# Upgrade-chain parity check: a database brought to the current version
# through CREATE EXTENSION VERSION '1.0.0' + ALTER EXTENSION UPDATE must
# be catalog-identical to a fresh CREATE EXTENSION -- same functions
# (signature, return type, body hash, volatility, security), aggregates,
# operators (with commutators), casts (with their context), types, enum
# values, and relations.
#
# This is the strong form of the extension_upgrade pg_regress canary:
# the canary smoke-tests a handful of features, while this check proves
# the upgrade scripts replicate the whole SQL surface.  It has caught,
# in one sweep: missing objects, function-body drift, casts left at the
# wrong context, operators left as shells (COMMUTATOR/NEGATOR forward
# references treated as "exists" by idempotence guards), and functions
# created in the wrong schema by a script lacking SET search_path.
#
# Requirements: the current extension installed (sudo make install),
# a PostgreSQL superuser connection (same defaults as make installcheck),
# and createdb rights.  Usage:
#   test/upgrade_parity.sh [psql-options]
# e.g.
#   test/upgrade_parity.sh --port=5434
set -euo pipefail

PSQL=(psql -X -v ON_ERROR_STOP=1 "$@")
DB_UPGRADED=provsql_parity_upgraded
DB_FRESH=provsql_parity_fresh
TMP=$(mktemp -d /tmp/provsql-parity.XXXXXX)
cleanup() {
  "${PSQL[@]}" -d postgres -c "DROP DATABASE IF EXISTS $DB_UPGRADED" \
               -c "DROP DATABASE IF EXISTS $DB_FRESH" > /dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT

"${PSQL[@]}" -d postgres -c "DROP DATABASE IF EXISTS $DB_UPGRADED" \
             -c "DROP DATABASE IF EXISTS $DB_FRESH" \
             -c "CREATE DATABASE $DB_UPGRADED" \
             -c "CREATE DATABASE $DB_FRESH" > /dev/null

echo "upgrade_parity: building $DB_UPGRADED (1.0.0 + upgrade chain)..."
"${PSQL[@]}" -q -d "$DB_UPGRADED" \
  -c "CREATE EXTENSION provsql VERSION '1.0.0' CASCADE" \
  -c "ALTER EXTENSION provsql UPDATE" > /dev/null
echo "upgrade_parity: building $DB_FRESH (direct install)..."
"${PSQL[@]}" -q -d "$DB_FRESH" \
  -c "CREATE EXTENSION provsql CASCADE" > /dev/null

V1=$("${PSQL[@]}" -d "$DB_UPGRADED" -tAc \
  "SELECT extversion FROM pg_extension WHERE extname='provsql'")
V2=$("${PSQL[@]}" -d "$DB_FRESH" -tAc \
  "SELECT extversion FROM pg_extension WHERE extname='provsql'")
if [ "$V1" != "$V2" ]; then
  echo "upgrade_parity: FAIL: upgraded at $V1, fresh at $V2" >&2
  exit 1
fi

cat > "$TMP/dump.sql" <<'EOF'
\pset format unaligned
\pset tuples_only on
SELECT 'FUNC|' || p.proname || '(' || pg_get_function_identity_arguments(p.oid) || ')|'
       || p.prorettype::regtype || '|' || md5(p.prosrc) || '|'
       || p.provolatile::text || p.prosecdef::int
FROM pg_proc p WHERE p.pronamespace = 'provsql'::regnamespace AND p.prokind <> 'a'
ORDER BY 1;
SELECT 'AGG|' || p.proname || '(' || pg_get_function_identity_arguments(p.oid) || ')|'
       || a.aggtransfn::regproc || '|' || a.aggfinalfn::regproc || '|' || coalesce(a.agginitval,'-')
FROM pg_proc p JOIN pg_aggregate a ON a.aggfnoid = p.oid
WHERE p.pronamespace = 'provsql'::regnamespace ORDER BY 1;
SELECT 'OP|' || o.oprname || '|' || coalesce(o.oprleft::regtype::text,'-') || '|'
       || coalesce(o.oprright::regtype::text,'-') || '|' || o.oprcode::regproc
       || '|' || coalesce(o.oprcom::regoperator::text,'-')
FROM pg_operator o WHERE o.oprnamespace = 'provsql'::regnamespace ORDER BY 1;
SELECT 'CAST|' || c.castsource::regtype || '->' || c.casttarget::regtype || '|'
       || coalesce(c.castfunc::regproc::text,'binary') || '|' || c.castcontext::text
FROM pg_cast c
WHERE c.castsource IN (SELECT oid FROM pg_type WHERE typnamespace='provsql'::regnamespace)
   OR c.casttarget IN (SELECT oid FROM pg_type WHERE typnamespace='provsql'::regnamespace)
ORDER BY 1;
SELECT 'TYPE|' || t.typname || '|' || t.typtype::text FROM pg_type t
WHERE t.typnamespace = 'provsql'::regnamespace AND t.typname NOT LIKE '\_%' ORDER BY 1;
SELECT 'ENUM|' || t.typname || '|' || e.enumsortorder || '|' || e.enumlabel
FROM pg_enum e JOIN pg_type t ON t.oid = e.enumtypid
WHERE t.typnamespace = 'provsql'::regnamespace ORDER BY t.typname, e.enumsortorder;
SELECT 'REL|' || c.relname || '|' || c.relkind::text FROM pg_class c
WHERE c.relnamespace = 'provsql'::regnamespace AND c.relkind IN ('r','v','S') ORDER BY 1;
-- Extension members that an upgrade script created OUTSIDE the provsql
-- schema (a missing SET search_path) would be invisible to the queries
-- above on both sides; list every member's schema to catch them.
SELECT 'MEMBER|' || pg_describe_object(d.classid, d.objid, 0)
FROM pg_depend d JOIN pg_extension e ON d.refobjid = e.oid
WHERE e.extname = 'provsql' AND d.refclassid = 'pg_extension'::regclass
  AND d.deptype = 'e'
ORDER BY 1;
EOF

"${PSQL[@]}" -d "$DB_UPGRADED" -f "$TMP/dump.sql" > "$TMP/upgraded.txt"
"${PSQL[@]}" -d "$DB_FRESH"    -f "$TMP/dump.sql" > "$TMP/fresh.txt"

if diff -u "$TMP/upgraded.txt" "$TMP/fresh.txt"; then
  echo "upgrade_parity: OK ($V2; $(wc -l < "$TMP/fresh.txt") catalog entries identical)"
else
  echo "upgrade_parity: FAIL: an upgraded database differs from a fresh install (diff above:" >&2
  echo "  '<' = upgraded-only / stale, '>' = fresh-only / missing from the upgrade chain)." >&2
  echo "  Complete sql/upgrades/provsql--<prev>--<new>.sql accordingly." >&2
  exit 1
fi
