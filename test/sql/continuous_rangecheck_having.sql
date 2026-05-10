\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- RangeCheck on HAVING-style cmps over gate_agg children: trivially
-- decidable cases (count >= 0, count > n, sum bounds, min/max
-- bounds against the children's value range) get resolved to a
-- Bernoulli gate_input at getGenericCircuit time, before any sampler
-- runs.  The cmps that are NOT trivially decidable fall through to
-- the existing HAVING evaluation path (provsql_having's possible-
-- world enumeration), so the user-facing semantics are unchanged.
--
-- We use a deterministic table where every row's provenance is the
-- default gate_input with prob=1 -- so the only reason a HAVING cmp
-- could be undecided is if it depends on which rows are present.
-- Trivial cmps don't depend on that and resolve immediately.

CREATE TABLE rch(category text, value int);
INSERT INTO rch VALUES
  ('alpha', 1), ('alpha', 2), ('alpha', 3),
  ('beta',  -5), ('beta',   2);
SELECT add_provenance('rch');

-- (1) HAVING count(*) >= 0 -- always true (count is in [0, n] so
-- always >= 0).  Should resolve to gate_one and produce a row for
-- every group with probability 1.0.
CREATE TABLE rch_count_ge0 AS
  SELECT category, probability_evaluate(provenance(),
                                         'monte-carlo', '1') AS p
    FROM rch GROUP BY category HAVING count(*) >= 0;
SELECT remove_provenance('rch_count_ge0');
SELECT category, p = 1.0 AS exact_one
  FROM rch_count_ge0 ORDER BY category;
DROP TABLE rch_count_ge0;

-- (2) HAVING count(*) > 100 -- always false (alpha has 3 rows,
-- beta has 2; both well below 100).  Resolves to gate_zero and
-- yields p = 0.0 for every group.
CREATE TABLE rch_count_gt100 AS
  SELECT category, probability_evaluate(provenance(),
                                         'monte-carlo', '1') AS p
    FROM rch GROUP BY category HAVING count(*) > 100;
SELECT remove_provenance('rch_count_gt100');
SELECT category, p = 0.0 AS exact_zero
  FROM rch_count_gt100 ORDER BY category;
DROP TABLE rch_count_gt100;

-- (3) HAVING count(*) <= 0 -- count is in [0, n], so cmp is decided
-- only when n=0 (impossible here since groups are non-empty in the
-- table).  decideCmp returns NaN for diff=[-n, 0] (overlaps zero
-- on the LE side: diff.hi=0, diff.lo=-n; "diff <= 0" -> diff.hi=0
-- <=0 => TRUE).  So this resolves TRUE -- which would be wrong if
-- not for the COUNT case being NULL-free (count=0 IS a valid
-- value, not NULL).  Verify it indeed lands on TRUE.
CREATE TABLE rch_count_le0 AS
  SELECT category, probability_evaluate(provenance(),
                                         'monte-carlo', '1') AS p
    FROM rch GROUP BY category HAVING count(*) <= 5;
SELECT remove_provenance('rch_count_le0');
SELECT category, p = 1.0 AS exact_one
  FROM rch_count_le0 ORDER BY category;
DROP TABLE rch_count_le0;

-- (4) HAVING sum(value) > 100 -- alpha values [1,2,3], so sum is
-- bounded above by 1+2+3 = 6 (sum of positives).  100 > 6 so the
-- bound says "always false".  Beta values [-5, 2], sum bounded
-- above by 2.  100 > 2 so also "always false".  Decided to false
-- safely (FALSE-only decisions are sound for SUM).
CREATE TABLE rch_sum_gt100 AS
  SELECT category, probability_evaluate(provenance(),
                                         'monte-carlo', '1') AS p
    FROM rch GROUP BY category HAVING sum(value) > 100;
SELECT remove_provenance('rch_sum_gt100');
SELECT category, p = 0.0 AS exact_zero
  FROM rch_sum_gt100 ORDER BY category;
DROP TABLE rch_sum_gt100;

-- (5) HAVING sum(value) < -100 -- alpha sum >= 0 (all positive),
-- beta sum >= -5 (sum of negatives).  Both > -100 always.  False.
CREATE TABLE rch_sum_lt_neg100 AS
  SELECT category, probability_evaluate(provenance(),
                                         'monte-carlo', '1') AS p
    FROM rch GROUP BY category HAVING sum(value) < -100;
SELECT remove_provenance('rch_sum_lt_neg100');
SELECT category, p = 0.0 AS exact_zero
  FROM rch_sum_lt_neg100 ORDER BY category;
DROP TABLE rch_sum_lt_neg100;

-- (6) HAVING max(value) < -100 -- max can only be in [min(values),
-- max(values)] for nonempty subsets, NULL for empty.  alpha max in
-- [1,3], beta max in [-5,2].  -100 < min for both, so cmp is
-- always false (or NULL on empty -> false).  Decided.
CREATE TABLE rch_max_lt_neg AS
  SELECT category, probability_evaluate(provenance(),
                                         'monte-carlo', '1') AS p
    FROM rch GROUP BY category HAVING max(value) < -100;
SELECT remove_provenance('rch_max_lt_neg');
SELECT category, p = 0.0 AS exact_zero
  FROM rch_max_lt_neg ORDER BY category;
DROP TABLE rch_max_lt_neg;

-- (7) HAVING min(value) > 1000 -- min can only be in [min(values),
-- max(values)].  Both groups have max <= 3 < 1000, so cmp always
-- false.  Decided.
CREATE TABLE rch_min_gt_big AS
  SELECT category, probability_evaluate(provenance(),
                                         'monte-carlo', '1') AS p
    FROM rch GROUP BY category HAVING min(value) > 1000;
SELECT remove_provenance('rch_min_gt_big');
SELECT category, p = 0.0 AS exact_zero
  FROM rch_min_gt_big ORDER BY category;
DROP TABLE rch_min_gt_big;

-- (8) HAVING sum(value) >= -1000 -- both groups have sum well above
-- -1000 in every nonempty world.  RangeCheck would WRONGLY decide
-- this TRUE for SUM if not for the may_be_null guard: in empty
-- worlds (no row contributing) PG's SUM is NULL, the cmp is NULL,
-- the HAVING is FALSE, so a "TRUE in every world" rewrite would be
-- unsound.  decideAggVsConstCmp must fall through to provsql_having.
--
-- To detect a wrong TRUE resolution we set every row's input
-- probability to 0.5: the correct probability of each group's
-- output row becomes "at least one of n rows present" = 1 - 0.5^n.
-- For alpha (n=3) that is 0.875; for beta (n=2) it is 0.75.  A
-- wrongly-resolved gate_one would always give exactly 1.0.
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM rch;
END $$;
SET provsql.monte_carlo_seed = 42;
CREATE TABLE rch_sum_ge_neg AS
  SELECT category,
         probability_evaluate(provenance(), 'monte-carlo', '50000') AS p
    FROM rch GROUP BY category HAVING sum(value) >= -1000;
RESET provsql.monte_carlo_seed;
SELECT remove_provenance('rch_sum_ge_neg');
-- alpha should be ~0.875 (NOT 1.0); beta should be ~0.75 (NOT 1.0)
SELECT category,
       abs(p - CASE category WHEN 'alpha' THEN 0.875
                             WHEN 'beta'  THEN 0.75 END) < 0.02
       AS within_tolerance
  FROM rch_sum_ge_neg ORDER BY category;
DROP TABLE rch_sum_ge_neg;
-- Restore probs (defensive, table is dropped next anyway)
DO $$ BEGIN
  PERFORM set_prob(provenance(), 1.0) FROM rch;
END $$;

DROP TABLE rch;

SELECT 'ok'::text AS continuous_rangecheck_having_done;
