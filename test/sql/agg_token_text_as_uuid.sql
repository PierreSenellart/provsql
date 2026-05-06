\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Cover the provsql.aggtoken_text_as_uuid GUC and its companion
-- agg_token_value_text helper. The GUC controls how agg_token's
-- type-output function renders cells (default: "value (*)"; on:
-- the underlying UUID). We don't print the cells directly because
-- the UUIDs are non-deterministic across runs; the test checks
-- the GUC contract via SHOW and validates the helper through
-- (col::uuid).

CREATE TABLE aggtok_demo AS
SELECT city, COUNT(*) AS c
  FROM personnel GROUP BY city ORDER BY city;
SELECT remove_provenance('aggtok_demo');

SELECT pg_typeof(c)::text FROM aggtok_demo LIMIT 1;

-- The GUC is registered and PGC_USERSET (any session can SET it).
SHOW provsql.aggtoken_text_as_uuid;
BEGIN;
SET LOCAL provsql.aggtoken_text_as_uuid = on;
SHOW provsql.aggtoken_text_as_uuid;
ROLLBACK;
SHOW provsql.aggtoken_text_as_uuid;

-- agg_token_value_text recovers the friendly "value (*)" form from any
-- agg_token's UUID, regardless of the GUC. Sorted by city for
-- determinism.
SELECT city, agg_token_value_text(c::uuid) AS display
  FROM aggtok_demo
  ORDER BY city;

-- Returns NULL for non-agg gates: the per-row provenance column on
-- personnel is an input gate, not an agg. Wrap in a CTE + subquery
-- so the agg_token_value_text result is the only printed column
-- (personnel.provsql is a non-deterministic UUID).
WITH t(p) AS (SELECT provsql FROM personnel WHERE name = 'John')
SELECT (SELECT agg_token_value_text(p) FROM t) IS NULL AS not_an_agg;

DROP TABLE aggtok_demo;
