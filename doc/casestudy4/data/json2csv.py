#!/usr/bin/env python3
# Convert a Wikidata JSON export (produced by ministers.sparql) to per-country
# CSV files suitable for loading with setup.sql.
#
# Usage:  python json2csv.py data.json
# Output: CC_person.csv, CC_position.csv, CC_party.csv for each country code
#         found in the data (e.g. FR_person.csv, SG_person.csv, …).

import csv
import json
import sys
import re

if len(sys.argv) < 2:
    print("Usage: python " + sys.argv[0] + " data.json")
    sys.exit(1)

with open(sys.argv[1], "r", encoding="utf-8") as f:
    raw = json.load(f)

# Accept both flat list format and standard SPARQL JSON format.
if isinstance(raw, dict) and "results" in raw:
    data = [{k: v["value"] for k, v in binding.items()}
            for binding in raw["results"]["bindings"]]
else:
    data = raw

# --- label normalisation helpers ---

_SMALL = {'a', 'an', 'and', 'at', 'by', 'de', 'du', 'for', 'in', 'of', 'on', 'the', 'to', 'with'}

def title_case(s):
    """Capitalize each significant word; keep small words lowercase except at start."""
    words = s.split()
    return ' '.join(
        w if (i > 0 and w.lower() in _SMALL) else w[0].upper() + w[1:]
        for i, w in enumerate(words)
    )

_FRENCH_RE = re.compile(
    r'[àâéèêëîïôùûüœæç]|\bministre\b|\bministère\b|\bdélégué\b',
    re.IGNORECASE)

def is_english(label):
    """Return False if the label contains French-specific characters or words."""
    return bool(label) and not _FRENCH_RE.search(label)

# --- open one set of output files per country code ---

country_codes = sorted({row["countryCode"] for row in data if row.get("countryCode")})

writers = {}      # cc -> (person_writer, position_writer, party_writer)
file_handles = []

for cc in country_codes:
    pe = open(cc + "_person.csv",   'w', encoding='utf-8')
    po = open(cc + "_position.csv", 'w', encoding='utf-8')
    pa = open(cc + "_party.csv",    'w', encoding='utf-8')
    file_handles += [pe, po, pa]
    pe_w, po_w, pa_w = csv.writer(pe), csv.writer(po), csv.writer(pa)
    pe_w.writerow(["id", "name", "gender", "birth", "death"])
    po_w.writerow(["id", "position", "country", "start", "until"])
    pa_w.writerow(["id", "party"])
    writers[cc] = (pe_w, po_w, pa_w)

# --- process rows ---

persons   = {}   # id -> True  (globally unique Wikidata QIDs)
positions = {}   # (cc, id, position, start) -> True
parties   = {}   # (cc, id, party) -> True

for row in data:
    cc = row.get("countryCode")
    if not cc or cc not in writers:
        continue
    pe_w, po_w, pa_w = writers[cc]

    pid = re.sub(r'.*/Q', "", row["person"])
    death = row.get("deathISO")

    if pid not in persons:
        persons[pid] = True
        pe_w.writerow([pid, row.get("personLabel"), row.get("genderLabel"),
                       row.get("birthISO"), death])

    position = row.get("ministerLabel")
    if not is_english(position):
        continue
    position = title_case(position)

    start = row.get("startISO")
    if (cc, pid, position, start) not in positions:
        positions[(cc, pid, position, start)] = True
        end = row.get("endISO")
        if end is None and death is not None:
            end = death
        if start is not None and (
                (end is None and start > '1950') or
                (end is not None and end >= start)):
            po_w.writerow([pid, position, cc, start, end])

    party = row.get("partyLabel")
    if party is not None and (cc, pid, party) not in parties:
        parties[(cc, pid, party)] = True
        pa_w.writerow([pid, party])

for fh in file_handles:
    fh.close()
