#!/usr/bin/env python3

import csv
import json
import sys
import re

if len(sys.argv) < 3:
    print("Usage: python "+sys.argv[0]+" data.json CC")
    sys.exit(1)

json_filename = sys.argv[1]
country = sys.argv[2]

# Load JSON data from a file
with open(json_filename, "r", encoding="utf-8") as file:
    data = json.load(file)

csv_person = country+"_person.csv"
csv_position = country+"_position.csv"
csv_party = country+"_party.csv"

headers_person = ["id", "name", "gender", "birth", "death"]
headers_position = ["id", "position", "country", "start", "until"]
headers_party = ["id", "party"]

with open(csv_person, 'w', encoding='utf-8') as pe:
    pe_writer = csv.writer(pe)
    pe_writer.writerow(headers_person)
    with open(csv_position, 'w', encoding='utf-8') as po:
        po_writer = csv.writer(po)
        po_writer.writerow(headers_position)
        with open(csv_party, 'w', encoding='utf-8') as po:
            pa_writer = csv.writer(po)
            pa_writer.writerow(headers_party)

            persons = dict()
            positions = dict()
            parties = dict()

            for row in data:
                id = row.get("person")
                id = re.sub(r'.*/Q', "", id)
                death = row.get("deathISO")
                if id not in persons:
                    persons[id] = 1
                    gender = row.get("genderLabel")
                    name = row.get("personLabel")
                    birth = row.get("birthISO")
                    pe_writer.writerow([id, name, gender, birth, death])

                position = row.get("ministerLabel")
                start = row.get("startISO")
                if (id, position, start) not in positions:
                    positions[(id, position, start)] = 1
                    start = row.get("startISO")
                    end = row.get("endISO")
                    if end is None and death is not None:
                        end = death
                    if start is not None and (
                            (end is None and start > '1950') or (
                                end is not None and end >= start)):
                        po_writer.writerow([id, position, country, start, end])

                party = row.get("partyLabel")
                if party is not None and (id, party) not in parties:
                    parties[(id, party)] = 1
                    pa_writer.writerow([id, party])
