\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Same 3-way times circuit as tseytin_cnf.sql: personnel 5/6/7 with
-- probabilities 0.5/0.6/0.7 (set by probability_setup). The variable
-- mapping is deterministic in (variable, probability); the gate UUID is
-- a random per-row token, so we only assert it is non-null.
CREATE TABLE tseytin_cnf_mapping_q AS
SELECT provenance() AS tok,
       tseytin_cnf(provenance())              AS cnf_default,
       tseytin_cnf(provenance(), true, false) AS cnf_nomap
FROM personnel p1, personnel p2, personnel p3
WHERE p1.id=5 AND p2.id=6 AND p3.id=7;

SELECT remove_provenance('tseytin_cnf_mapping_q');

-- The mapping: one row per input gate, ordered by variable.
SELECT variable, probability, gate IS NOT NULL AS has_uuid
FROM tseytin_cnf_mapping_q, tseytin_cnf_mapping(tok)
ORDER BY variable;

-- Default output carries the "c input" mapping comments; mapping=>false
-- suppresses them.
SELECT position('c input ' in cnf_default) > 0 AS default_has_mapping,
       position('c input ' in cnf_nomap)   > 0 AS nomap_has_mapping
FROM tseytin_cnf_mapping_q;

-- The mapping comments reference exactly the input variables (and no
-- auxiliary Tseytin variables), and the probabilities round-trip.
SELECT count(*) AS mapping_rows,
       min(probability) AS min_p, max(probability) AS max_p
FROM tseytin_cnf_mapping_q, tseytin_cnf_mapping(tok);

DROP TABLE tseytin_cnf_mapping_q;
