\set ECHO none
\pset format unaligned
-- Regression: compiled semirings (sr_*) must resolve the mapping
-- relation by its full schema-qualified name. Earlier versions called
-- `get_rel_name` and embedded the bare name in the SPI lookup, which
-- failed with "relation does not exist" whenever the mapping lived in
-- a schema not on search_path (e.g. when called from a session whose
-- search_path is the default "public"). Studio's eval strip is the
-- canonical reproducer.
SET search_path TO provsql_test,provsql;
SELECT create_provenance_mapping('personnel_count_qm', 'personnel', '1');

-- Drop provsql_test from search_path: both the mapping and the source
-- relation are still reachable by qualified name. The compiled
-- semiring's SPI lookup must cope with the schema being absent.
SET search_path TO provsql;

CREATE TABLE provsql_test.result_qm AS SELECT
  p1.city,
  provsql.sr_counting(provsql.provenance(),'provsql_test.personnel_count_qm'::regclass)
    AS counting
FROM provsql_test.personnel p1, provsql_test.personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;

SELECT provsql.remove_provenance('provsql_test.result_qm'::regclass);
SELECT * FROM provsql_test.result_qm ORDER BY city;

DROP TABLE provsql_test.result_qm;
DROP TABLE provsql_test.personnel_count_qm;
