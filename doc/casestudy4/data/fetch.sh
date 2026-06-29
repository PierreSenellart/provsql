#!/usr/bin/env bash
# Fetch fresh minister data from Wikidata and produce data.json.
# Run from this directory: bash fetch.sh
# Requires: curl, python3

set -euo pipefail

ENDPOINT="https://query.wikidata.org/sparql"
AGENT="ProvSQL/1.0 (https://github.com/PierreSenellart/provsql)"
TEMPLATE="ministers.sparql"

# Add countries here as "QID CODE" pairs.
declare -a COUNTRIES=("wd:Q142 FR" "wd:Q334 SG")

fetch_country() {
    local qid="$1" code="$2" outfile="$3"
    local query
    # Substitute placeholders with the hardcoded country IRI and code.
    # Using literal IRIs (not SPARQL variables) lets the query planner
    # use index lookups and avoids timeout on the public endpoint.
    query=$(sed -e "s|COUNTRY_QID|$qid|g" -e "s|COUNTRY_CODE|$code|g" "$TEMPLATE")
    echo "Fetching $code ($qid)..."
    curl -s --max-time 120 \
        -X POST "$ENDPOINT" \
        -H "Accept: application/sparql-results+json" \
        -H "User-Agent: $AGENT" \
        --data-urlencode "query=$query" \
        -o "$outfile" \
        -w "  HTTP %{http_code}, %{size_download} bytes, %{time_total}s\n"
}

outfiles=()
for entry in "${COUNTRIES[@]}"; do
    qid="${entry% *}"
    code="${entry#* }"
    outfile="${code}_raw.json"
    fetch_country "$qid" "$code" "$outfile"
    outfiles+=("$outfile")
done

# Merge per-country SPARQL JSON files into one flat list.
python3 - "${outfiles[@]}" << 'PYEOF'
import json, sys

def flatten(path):
    with open(path) as f:
        raw = json.load(f)
    if isinstance(raw, dict) and "results" in raw:
        return [{k: v["value"] for k, v in b.items()}
                for b in raw["results"]["bindings"]]
    return raw

data = []
for path in sys.argv[1:]:
    rows = flatten(path)
    print(f"  {path}: {len(rows)} rows")
    data.extend(rows)

with open("data.json", "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False)
print(f"Merged {len(data)} rows -> data.json")
PYEOF
