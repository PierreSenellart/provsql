\set ECHO none
\pset format unaligned

-- A row that no provenance-tracked relation contributes to is in every
-- possible world: provenance() gives it the semiring one, gate_one(), whether
-- the query reads untracked data only or also tracked relations.

-- No relation at all
SELECT 1 AS x, provenance() = gate_one() AS one;

-- A VALUES list, duplicates included
SELECT x, provenance() = gate_one() AS one
FROM (VALUES (1), (2), (2)) v(x)
ORDER BY x;

-- An untracked table, with an aggregation over it
CREATE TABLE provenance_certain_t(x int);
INSERT INTO provenance_certain_t VALUES (1), (NULL), (3);
SELECT x, provenance() = gate_one() AS one
FROM provenance_certain_t
ORDER BY x;
SELECT count(*), provenance() = gate_one() AS one FROM provenance_certain_t;

-- The token evaluates as the semiring one
SELECT sr_formula(provenance()), sr_boolean(provenance()),
       probability_evaluate(provenance())
FROM (VALUES (1)) v(x);

-- The same rows next to a tracked relation: the VALUES row gets the same token
CREATE TABLE provenance_certain_result AS
SELECT name AS label, provenance() = gate_one() AS one
FROM personnel WHERE id = 1
UNION ALL
SELECT 'values', provenance() = gate_one() FROM (VALUES (1)) v(x);
SELECT remove_provenance('provenance_certain_result');
SELECT * FROM provenance_certain_result ORDER BY label;

DROP TABLE provenance_certain_result;
DROP TABLE provenance_certain_t;
