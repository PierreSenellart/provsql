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

-- Boolean elements: PostgreSQL renders a scalar bool as 'true'/'false' but a
-- bool array element as 't'/'f'; the array_agg comparison reconciles the two so
-- the value-as-text match works.  hb: A = {true, false}, B = {true}, p = 0.5.
CREATE TABLE hb(g text, flag boolean);
INSERT INTO hb VALUES ('A', true), ('A', false), ('B', true);
SELECT add_provenance('hb');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hb; END $$;

-- = ARRAY[true]: only the true row present.                      A 0.25  B 0.5
CREATE TABLE r5 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hb GROUP BY g HAVING array_agg(flag ORDER BY flag) = ARRAY[true];
SELECT remove_provenance('r5'); SELECT 'bool =[t]' AS q, g, p FROM r5 ORDER BY g;

-- = ARRAY[false,true]: both A-rows present (ordered).            A 0.25  B 0
CREATE TABLE r6 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hb GROUP BY g HAVING array_agg(flag ORDER BY flag) = ARRAY[false,true];
SELECT remove_provenance('r6'); SELECT 'bool =[f,t]' AS q, g, p FROM r6 ORDER BY g;

-- <> ARRAY[true]: non-empty and not exactly [true].              A 0.5   B 0
CREATE TABLE r7 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hb GROUP BY g HAVING array_agg(flag ORDER BY flag) <> ARRAY[true];
SELECT remove_provenance('r7'); SELECT 'bool <>[t]' AS q, g, p FROM r7 ORDER BY g;

DROP TABLE r1; DROP TABLE r2; DROP TABLE r3; DROP TABLE r4;
DROP TABLE r5; DROP TABLE r6; DROP TABLE r7;
DROP TABLE haa; DROP TABLE hb;
