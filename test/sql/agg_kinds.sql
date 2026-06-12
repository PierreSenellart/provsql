\set ECHO none
\pset format unaligned

-- Provenance of the boolean and array aggregates (bool_and / its every alias,
-- bool_or, array_agg) through the agg_token path. getAggregationOperator must
-- map the PostgreSQL aggregate names to the right operator, and the formula
-- semiring renders the aggregation. ORDER BY inside each aggregate keeps the
-- rendered provenance deterministic.

CREATE TABLE agg_kinds_src(g text, flag boolean, val int, name text);
INSERT INTO agg_kinds_src VALUES
  ('A', true,  1, 'a1'),
  ('A', false, 2, 'a2'),
  ('B', true,  3, 'b1');
SELECT add_provenance('agg_kinds_src');
SELECT create_provenance_mapping('agg_kinds_map', 'agg_kinds_src', 'name');

CREATE TABLE agg_kinds_res AS
  SELECT g,
         bool_or(flag  ORDER BY name)  AS bo,
         bool_and(flag ORDER BY name)  AS ba,
         every(flag    ORDER BY name)  AS ev,
         array_agg(val ORDER BY name)  AS arr
  FROM agg_kinds_src GROUP BY g;
SELECT remove_provenance('agg_kinds_res');

SELECT g,
       sr_formula(bo::uuid,  'agg_kinds_map') AS bool_or,
       sr_formula(ba::uuid,  'agg_kinds_map') AS bool_and,
       sr_formula(ev::uuid,  'agg_kinds_map') AS every,
       sr_formula(arr::uuid, 'agg_kinds_map') AS array_agg
FROM agg_kinds_res ORDER BY g;

DROP TABLE agg_kinds_res;
DROP TABLE agg_kinds_map;
DROP TABLE agg_kinds_src;
