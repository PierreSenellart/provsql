\set ECHO none
\pset format unaligned

-- An AGG(DISTINCT) anywhere in a target-list expression is computed over the
-- distinct values, in the circuit as in the displayed value, and an expression
-- over other aggregates can sit beside it.  The rewrite used to see only a
-- DISTINCT aggregate that was a whole target entry: inside an expression the
-- DISTINCT was ignored in the circuit, and any other expression over an
-- aggregate was taken for a grouping key (internal planner error).
--
--   g=0: (v,w) = (5,1), (6,2)   g=1: (5,3), (7,4)   g=2: (1,5)
--   g=3: (9,6), (9,7)  -- two rows, one distinct v     g=NULL: (4,8), (4,9)
-- Each row has probability 0.5.

CREATE TABLE ade(g int, v int, w int);
INSERT INTO ade VALUES (0,5,1), (0,6,2), (1,5,3), (1,7,4), (2,1,5), (3,9,6), (3,9,7),
  (NULL,4,8), (NULL,4,9);
CREATE TABLE ade_u AS SELECT * FROM ade;
SELECT add_provenance('ade');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ade; END $$;

-- Values: the tracked query against the same query over the untracked copy.
CREATE FUNCTION ade_check(label text, query text) RETURNS TABLE(q text, differences bigint)
LANGUAGE plpgsql AS $$
BEGIN
  SET LOCAL client_min_messages = error;
  EXECUTE 'CREATE TEMP TABLE ade_t AS ' || replace(query, '@', 'ade');
  PERFORM provsql.remove_provenance('ade_t');
  EXECUTE 'CREATE TEMP TABLE ade_s AS ' || replace(query, '@', 'ade_u');
  CREATE TEMP TABLE ade_tt AS
    SELECT regexp_replace((t.*)::text, ' \(\*\)|"', '', 'g') AS r FROM ade_t t;
  RETURN QUERY EXECUTE
    'SELECT $1, (SELECT count(*) FROM
        ((SELECT r FROM ade_tt EXCEPT ALL SELECT (s.*)::text FROM ade_s s)
         UNION ALL
         (SELECT (s.*)::text FROM ade_s s EXCEPT ALL SELECT r FROM ade_tt)) x)'
    USING label;
  DROP TABLE ade_t, ade_s, ade_tt;
END $$;

SELECT * FROM ade_check('DISTINCT in an expression',
  'SELECT g, count(DISTINCT v) + 1 AS c1 FROM @ GROUP BY g');
SELECT * FROM ade_check('cast around DISTINCT',
  'SELECT g, count(DISTINCT v)::int AS ci FROM @ GROUP BY g');
SELECT * FROM ade_check('expression over another aggregate beside it',
  'SELECT g, count(DISTINCT v) AS cd, sum(w) * 2 AS s2, count(*) AS c FROM @ GROUP BY g');
SELECT * FROM ade_check('two DISTINCT aggregates in one expression',
  'SELECT g, count(DISTINCT v) + count(DISTINCT w) AS two FROM @ GROUP BY g');
SELECT * FROM ade_check('the same DISTINCT aggregate twice',
  'SELECT g, count(DISTINCT v) AS a, count(DISTINCT v) + 1 AS b FROM @ GROUP BY g');
SELECT * FROM ade_check('sum(DISTINCT) in an expression, WHERE, key expression',
  'SELECT g, g + 1 AS g1, sum(DISTINCT v) - 1 AS sd, max(w) + 1 AS mw FROM @ WHERE w <> 2 GROUP BY g');
SELECT * FROM ade_check('no GROUP BY',
  'SELECT count(DISTINCT v) + 1 AS c1, sum(w) * 2 AS s2 FROM @ WHERE w > 1');

-- Circuit: the count gate below "+ 1" ranges over distinct values, one child
-- per value (g=3 and g=NULL: one child for two rows), as the bare form does.
CREATE TABLE ade_c AS SELECT g, count(DISTINCT v) + 1 AS c1, count(DISTINCT v) AS c0
  FROM ade GROUP BY g;
SELECT remove_provenance('ade_c');
SELECT 'count gate children' AS q, g, c1,
       get_gate_type(agg_token_uuid(c1)) AS top,
       (SELECT array_length(get_children(c), 1)
          FROM unnest(get_children(agg_token_uuid(c1))) c
         WHERE get_gate_type(c) = 'agg') AS in_expression,
       array_length(get_children(agg_token_uuid(c0)), 1) AS bare
  FROM ade_c ORDER BY g;

-- The same gate is reached from both forms.
SELECT 'same count gate' AS q, bool_and(
         (SELECT c FROM unnest(get_children(agg_token_uuid(c1))) c
           WHERE get_gate_type(c) = 'agg') = agg_token_uuid(c0)) AS same
  FROM ade_c;

-- Evaluation through the expression: P(count(DISTINCT v) + 1 >= 3), that is two
-- distinct values present: 0.25 for g=0 and g=1, impossible elsewhere.
CREATE TABLE ade_h AS SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ade GROUP BY g HAVING count(DISTINCT v) + 1 >= 3;
SELECT remove_provenance('ade_h');
SELECT 'HAVING cd + 1 >= 3' AS q, g, p FROM ade_h ORDER BY g;

-- DISTINCT in the target list and in HAVING together.  count(DISTINCT v) >= 1
-- is group existence: 0.75 for the two-row groups, 0.5 for g=2.
CREATE TABLE ade_h2 AS SELECT g, count(DISTINCT v) + 1 AS c1,
    round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM ade GROUP BY g HAVING count(DISTINCT v) >= 1;
SELECT remove_provenance('ade_h2');
SELECT 'target and HAVING' AS q, g, c1, p FROM ade_h2 ORDER BY g;

DROP FUNCTION ade_check(text, text);
DROP TABLE ade, ade_u, ade_c, ade_h, ade_h2;
