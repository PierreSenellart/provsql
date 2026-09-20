\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- Pin the runHavingAlwaysTrueRewriter probability-side pre-pass
-- (src/RangeCheck.cpp) against the Poisson-binomial path.
--
-- When a HAVING-style cmp is provably true on the agg's value-interval
-- (e.g. COUNT(*) <= N with N >= the largest group size), the rewriter
-- replaces the cmp with a gate_plus over the agg's per-row K-gates --
-- the "group is non-empty" indicator -- so the d-DNNF compiler never
-- sees the 2^N-clause DNF that provsql_having's enumerate_valid_worlds
-- would otherwise emit.
--
-- The pass runs unconditionally inside probability_evaluate, so it
-- catches the always-true case even when the Poisson-binomial GUC
-- (provsql.cmp_probability_evaluation) is OFF.  These tests verify
-- both: value equivalence vs the Poisson-binomial path, and (filtered
-- to a single group for determinism) a verbose_level >= 5 NOTICE
-- assertion that the rewriter actually fires.
-- ----------------------------------------------------------------------

DROP TABLE IF EXISTS att_t;
CREATE TABLE att_t (g INT, v INT);
INSERT INTO att_t VALUES
  (1,1),(1,2),(1,3),                  -- group 1: 3 rows
  (2,1),(2,2),                        -- group 2: 2 rows
  (3,1);                              -- group 3: 1 row
SELECT add_provenance('att_t');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM att_t; END $$;

-- (1) COUNT(*) <= 3 is always-true on every group (max group size is
--     3 = the constant).  Probabilities under shortcut on vs off
--     must coincide on every group.
DROP TABLE IF EXISTS att_off, att_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE att_off AS
  SELECT g, probability_evaluate(provenance()) AS p
  FROM att_t GROUP BY g HAVING count(*) <= 3;
SELECT remove_provenance('att_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE att_on AS
  SELECT g, probability_evaluate(provenance()) AS p
  FROM att_t GROUP BY g HAVING count(*) <= 3;
SELECT remove_provenance('att_on');
SELECT 'count <= 3' AS shape, o.g,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM att_off o JOIN att_on n USING (g) ORDER BY o.g;

-- (2) COUNT(*) >= 1: always true on every non-empty group (the
--     GE arm is clamped to >= 1 by count_enum, so any non-empty
--     subset satisfies).
DROP TABLE att_off, att_on;
SET provsql.cmp_probability_evaluation = off;
CREATE TEMP TABLE att_off AS
  SELECT g, probability_evaluate(provenance()) AS p
  FROM att_t GROUP BY g HAVING count(*) >= 1;
SELECT remove_provenance('att_off');
SET provsql.cmp_probability_evaluation = on;
CREATE TEMP TABLE att_on AS
  SELECT g, probability_evaluate(provenance()) AS p
  FROM att_t GROUP BY g HAVING count(*) >= 1;
SELECT remove_provenance('att_on');
SELECT 'count >= 1' AS shape, o.g,
       ROUND(o.p::numeric, 4) AS p_off,
       ROUND(n.p::numeric, 4) AS p_on,
       ROUND(ABS(o.p - n.p)::numeric, 6) AS diff
FROM att_off o JOIN att_on n USING (g) ORDER BY o.g;

-- (3) Fire assertion: with the Poisson-binomial GUC OFF and
--     verbose_level >= 5, an always-true HAVING must emit a single
--     "always-true" NOTICE.  Restricted to one group (g = 1) so the
--     NOTICE count is deterministic.
DROP TABLE att_off, att_on;
SET provsql.cmp_probability_evaluation = off;
SET provsql.verbose_level = 5;
CREATE TEMP TABLE att_fires AS
  SELECT g, ROUND(probability_evaluate(provenance())::numeric, 4) AS p_fires
  FROM att_t WHERE g = 1 GROUP BY g HAVING count(*) <= 3;
SET provsql.verbose_level = 0;
SELECT remove_provenance('att_fires');
SELECT * FROM att_fires;
DROP TABLE att_fires;

-- (4) Mirror : with the GUC ON, the same always-true HAVING is
--     resolved by the Poisson-binomial path instead.  Lock that
--     ordering in so a future refactor cannot silently re-route the
--     always-true case.
SET provsql.cmp_probability_evaluation = on;
SET provsql.verbose_level = 5;
CREATE TEMP TABLE att_fires_on AS
  SELECT g, ROUND(probability_evaluate(provenance())::numeric, 4) AS p_fires
  FROM att_t WHERE g = 1 GROUP BY g HAVING count(*) <= 3;
SET provsql.verbose_level = 0;
SELECT remove_provenance('att_fires_on');
SELECT * FROM att_fires_on;
DROP TABLE att_fires_on;

-- (5) Negative control : a HAVING that is not always-true on g=1's
--     value-interval [0, 3] -- count(*) >= 2 is true at values 2,3
--     and false at 0,1 -- must NOT trigger the always-true rewriter
--     and (with the GUC off) must not emit any shortcut NOTICE.
--     Filtered to g = 1 to make the assertion deterministic.
SET provsql.cmp_probability_evaluation = off;
SET provsql.verbose_level = 5;
CREATE TEMP TABLE att_neg AS
  SELECT g, ROUND(probability_evaluate(provenance())::numeric, 4) AS p_neg
  FROM att_t WHERE g = 1 GROUP BY g HAVING count(*) >= 2;
SET provsql.verbose_level = 0;
SELECT remove_provenance('att_neg');
SELECT * FROM att_neg;
DROP TABLE att_neg;

-- (6) A tautology over a scalar aggregation (no GROUP BY) is gate_one only
--     where the aggregate has a value over no row: count(*) is 0 there and
--     count(*) >= 0 holds, so p = 1; min, max, sum and avg are NULL, the
--     comparison is unknown and the row is filtered out, so the empty world
--     is excluded exactly as it is for a group.  att_t has six rows at
--     p = 0.5, so P(non-empty) = 1 - 1/64 = 0.984375.
SET provsql.cmp_probability_evaluation = on;
DO $$
DECLARE r record; p numeric;
BEGIN
  FOR r IN SELECT * FROM (VALUES
      ('count(*) >= 0', 1.000000),
      ('max(v) >= 0',   0.984375),
      ('min(v) >= 0',   0.984375),
      ('sum(v) >= 0',   0.984375),
      ('max(v) > -1',   0.984375)) v(pred, truth)
  LOOP
    EXECUTE format(
      'SELECT round(provsql.probability_evaluate(provsql.provenance())::numeric, 6)'
      ' FROM att_t HAVING %s', r.pred) INTO p;
    RAISE NOTICE 'scalar tautology %  p=%  (truth %)', rpad(r.pred, 14), p, r.truth;
  END LOOP;
END $$;

-- (7) The same, reading the aggregate results of a subquery: a max of a max is
--     the max over the rows of the groups (see reaggregation), so the rewriter
--     sees the flattened contributions, whose tokens are products rather than
--     inputs.  That is the shape where the empty world was credited: the three
--     groups of att_t hold all six rows, so P(some group exists) is again
--     0.984375, and max(m) >= 2 is P(a row with v >= 2 is present) = 1 - 1/8.
DO $$
DECLARE r record; p numeric;
BEGIN
  FOR r IN SELECT * FROM (VALUES
      ('max(m) >= 0', 0.984375),
      ('max(m) >= 2', 0.875000),
      ('min(m) >= 0', 0.984375)) v(pred, truth)
  LOOP
    EXECUTE format(
      'SELECT round(provsql.probability_evaluate(provsql.provenance())::numeric, 6)'
      ' FROM (SELECT g, max(v) AS m FROM att_t GROUP BY g) t HAVING %s', r.pred)
      INTO p;
    RAISE NOTICE 'reaggregated tautology %  p=%  (truth %)',
                 rpad(r.pred, 14), p, r.truth;
  END LOOP;
END $$;

DROP TABLE att_t;
