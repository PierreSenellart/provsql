\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

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

DROP TABLE att_t;
