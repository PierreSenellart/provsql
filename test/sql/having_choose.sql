\set ECHO none
\pset format unaligned

-- choose() in HAVING over non-numeric value types (here boolean and date) is
-- evaluated in the value-as-text domain via choose's PICKFIRST telescoping:
-- choose() takes the first present row's value. Atoms independent at p = 0.5,
-- rows kept in insertion order. A = (true/2020-01-01, false/2021-02-02),
-- B = (true/2020-01-01).

CREATE TABLE hc(g text, flag boolean, d date);
INSERT INTO hc VALUES
  ('A', true,  DATE '2020-01-01'),
  ('A', false, DATE '2021-02-02'),
  ('B', true,  DATE '2020-01-01');
SELECT add_provenance('hc');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hc; END $$;

-- choose(flag) = true: first present row is the true one, so P(row1 present).
-- A 0.5  B 0.5
CREATE TABLE r1 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hc GROUP BY g HAVING choose(flag) = true;
SELECT remove_provenance('r1'); SELECT 'choose bool' AS q, g, p FROM r1 ORDER BY g;

-- choose(d) = '2020-01-01' over a date column (first present row's date).
-- A 0.5  B 0.5
CREATE TABLE r2 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hc GROUP BY g HAVING choose(d) = DATE '2020-01-01';
SELECT remove_provenance('r2'); SELECT 'choose date' AS q, g, p FROM r2 ORDER BY g;

DROP TABLE r1; DROP TABLE r2; DROP TABLE hc;

-- choose() against another aggregate, which goes through the general
-- possible-worlds route: it read no value of choose() in any world, so the
-- comparison never held and every group came out at probability 0.  It reads
-- choose() as PICKFIRST now.  A = (10, 20) in that order, B = (30), at 0.5.
-- choose(v) = max(v): A holds in {10} and {20}, not in {10, 20}, where the
-- first present row is 10 and the maximum 20: 0.5 (the last row would give
-- 0.75); B whenever its row is there, 0.5.  choose(v) < max(v): A only in
-- {10, 20}, 0.25; B never.
CREATE TABLE hcn(g text, v int);
INSERT INTO hcn VALUES ('A', 10), ('A', 20), ('B', 30);
SELECT add_provenance('hcn');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM hcn; END $$;
CREATE TABLE r3 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hcn GROUP BY g HAVING choose(v) = max(v);
SELECT remove_provenance('r3'); SELECT 'choose = max' AS q, g, p FROM r3 ORDER BY g;
CREATE TABLE r4 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM hcn GROUP BY g HAVING choose(v) < max(v);
SELECT remove_provenance('r4'); SELECT 'choose < max' AS q, g, p FROM r4 ORDER BY g;
DROP TABLE r3; DROP TABLE r4; DROP TABLE hcn;
