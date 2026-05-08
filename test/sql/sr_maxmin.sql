\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

/* The max-min m-semiring: ⊕ = enum-max, ⊗ = enum-min. Fuzzy / trust
   shape: alternative derivations combine to the most permissive label,
   joins combine to the strictest label. */
CREATE TYPE trust_level AS ENUM ('untrusted','low','medium','high','verified');

CREATE TABLE source_trust(
  src_id INT PRIMARY KEY,
  city varchar,
  trust trust_level
);

INSERT INTO source_trust VALUES
  (1,'New York','high'),
  (2,'New York','medium'),
  (3,'New York','low'),
  (4,'Berlin','verified'),
  (5,'Berlin','medium'),
  (6,'Paris','untrusted'),
  (7,'Paris','medium');

SELECT add_provenance('source_trust');
SELECT create_provenance_mapping('source_trust_map', 'source_trust', 'trust');

/* Self-join: trust of a city pair is min of the two joined sources (⊗),
   and across pairs we keep the max (⊕). */
CREATE TABLE result_maxmin AS SELECT
  s1.city,
  sr_maxmin(provenance(),'source_trust_map','untrusted'::trust_level) AS combined_trust
FROM source_trust s1, source_trust s2
WHERE s1.city = s2.city AND s1.src_id < s2.src_id
GROUP BY s1.city
ORDER BY s1.city;

SELECT remove_provenance('result_maxmin');
SELECT * FROM result_maxmin;

DROP TABLE result_maxmin;
SELECT remove_provenance('source_trust');
DROP TABLE source_trust_map;
DROP TABLE source_trust;
DROP TYPE trust_level;
