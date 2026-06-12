\set ECHO none
\pset format unaligned

-- HAVING on array_agg() against a constant array: the general possible-worlds
-- pipeline (no aggregate-specific optimization), with elements compared as text
-- so any element type works. Exact probabilities, all atoms independent at
-- p = 0.5:  A = {1='a', 2='b'}, B = {3='c'}.

CREATE TABLE haa(g text, v int, nm text);
INSERT INTO haa VALUES ('A',1,'a'), ('A',2,'b'), ('B',3,'c');
SELECT add_provenance('haa');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM haa; END $$;

-- array_agg(v ORDER BY v) = ARRAY[1,2]: both A-rows present.    A 0.25  B 0
CREATE TABLE r1 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM haa GROUP BY g HAVING array_agg(v ORDER BY v) = ARRAY[1,2];
SELECT remove_provenance('r1'); SELECT 'int =[1,2]' AS q, g, p FROM r1 ORDER BY g;

-- = ARRAY[1]: row v=1 present, v=2 absent.                      A 0.25  B 0
CREATE TABLE r2 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM haa GROUP BY g HAVING array_agg(v ORDER BY v) = ARRAY[1];
SELECT remove_provenance('r2'); SELECT 'int =[1]' AS q, g, p FROM r2 ORDER BY g;

-- <> ARRAY[1,2]: any non-empty world that is not exactly [1,2]. A 0.5   B 0.5
CREATE TABLE r3 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM haa GROUP BY g HAVING array_agg(v ORDER BY v) <> ARRAY[1,2];
SELECT remove_provenance('r3'); SELECT 'int <>[1,2]' AS q, g, p FROM r3 ORDER BY g;

-- text elements compared the same way.                          A 0.25  B 0
CREATE TABLE r4 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM haa GROUP BY g HAVING array_agg(nm ORDER BY nm) = ARRAY['a','b'];
SELECT remove_provenance('r4'); SELECT 'text =[a,b]' AS q, g, p FROM r4 ORDER BY g;

DROP TABLE r1; DROP TABLE r2; DROP TABLE r3; DROP TABLE r4;
DROP TABLE haa;
