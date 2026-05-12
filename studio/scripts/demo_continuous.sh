#!/usr/bin/env bash
# Build the City Air-Quality Sensor Network fixture and (optionally)
# launch ProvSQL Studio against it.  Backs the worked example in
# doc/source/user/casestudy6.rst.
#
# Usage:
#   bash studio/scripts/demo_continuous.sh                # default db
#   bash studio/scripts/demo_continuous.sh --db <name>     # custom db name
#   bash studio/scripts/demo_continuous.sh --no-launch     # skip Studio
#
# Requires:
#   - PostgreSQL with the ProvSQL extension installed
#     (shared_preload_libraries = 'provsql' in postgresql.conf).
#   - provsql-studio installed (or runnable as `python3 -m provsql_studio`
#     from the source tree).

set -euo pipefail

DB_NAME="air_quality_demo"
LAUNCH=1
PORT=8000

while [[ $# -gt 0 ]]; do
  case "$1" in
    --db)        DB_NAME="$2"; shift 2 ;;
    --port)      PORT="$2";    shift 2 ;;
    --no-launch) LAUNCH=0;     shift   ;;
    -h|--help)
      sed -n '2,15p' "$0"
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      exit 1
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SQL_FIXTURE="$SCRIPT_DIR/demo_continuous.sql"

if [[ ! -f "$SQL_FIXTURE" ]]; then
  echo "missing fixture file: $SQL_FIXTURE" >&2
  exit 1
fi

# Drop and recreate the database so each run starts fresh.
if psql -lqt | cut -d \| -f 1 | grep -qw "$DB_NAME"; then
  echo "Dropping existing database $DB_NAME..."
  dropdb "$DB_NAME"
fi
echo "Creating database $DB_NAME..."
createdb "$DB_NAME"

echo "Loading fixture..."
psql -d "$DB_NAME" -f "$SQL_FIXTURE"

if [[ "$LAUNCH" -eq 0 ]]; then
  echo ""
  echo "Database $DB_NAME is ready.  Launch Studio yourself with:"
  echo "  provsql-studio --dsn postgresql:///$DB_NAME --port $PORT"
  exit 0
fi

DSN="postgresql:///$DB_NAME"
echo ""
echo "Launching Studio at http://127.0.0.1:$PORT/ against $DSN ..."

if command -v provsql-studio > /dev/null 2>&1; then
  exec provsql-studio --dsn "$DSN" --port "$PORT"
else
  # Fall back to running the package as a module from the source tree.
  exec python3 -m provsql_studio --dsn "$DSN" --port "$PORT"
fi
