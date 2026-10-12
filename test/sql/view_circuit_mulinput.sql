\set ECHO none
\if `perl -MGraph::Easy -e1 2>/dev/null && echo true || echo false`
\pset format unaligned

-- view_circuit on the values of a multivalued input (repair_key): each
-- value is a leaf labelled by the mapping, wired to the variable it is a
-- value of; a value the mapping does not name shows its index.
CREATE TABLE vcm(box int, label text);
INSERT INTO vcm VALUES (1, 'deer'), (1, 'fox'), (1, 'boar');
SELECT repair_key('vcm', 'box');
CREATE TABLE vcm_label AS
  SELECT label AS value, provsql AS provenance FROM vcm WHERE label <> 'boar';
SELECT remove_provenance('vcm_label');

CREATE TABLE vcm_result AS
SELECT view_circuit(provenance(), 'vcm_label') AS circuit
FROM vcm WHERE label IN ('deer', 'boar') GROUP BY box;
SELECT remove_provenance('vcm_result');
SELECT circuit FROM vcm_result;

DROP TABLE vcm_result;
DROP TABLE vcm_label;
DROP TABLE vcm;
\else
\echo 'SKIPPING: graph-easy not available'
\endif
