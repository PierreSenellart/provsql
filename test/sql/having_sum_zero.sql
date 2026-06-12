\set ECHO none
\pset format unaligned

-- Regression for HAVING sum(val) over a mutually-exclusive (repair_key / BID)
-- block that contains a value-0 outcome.  Value 0 is SUM's additive identity,
-- so a *present* value-0 tuple must not be conflated with the *absent* (empty)
-- group: the world in which the chosen value is 0 is a legitimate non-empty
-- world.  The world-enumeration (sum_dp) used to skip the whole dp[0] bucket
-- for < and <= (to drop the empty world), which also dropped those value-0
-- worlds, so e.g. P(sum<5) came out 0 instead of 0.5.  The fix keeps dp[0] and
-- removes only the empty world.
--
-- Controls:
--   * choose(val) -- the faithful categorical analog (no additive identity):
--     it must agree with sum(val) on every predicate below;
--   * count(*)    -- every tuple counts 1, so there is no value-0 contributor:
--     it must stay correct (it always was).
--
-- Each probability is read in two steps: a top-level GROUP BY/HAVING query
-- captures the result group's provenance token (\gset), then a second,
-- untracked query evaluates it -- so the output is just (pred, p), without the
-- auto-appended provsql column.

-- A single mutually-exclusive block X with P(X=0)=0.5, P(X=10)=0.5.
CREATE TABLE bz(d text, val int, p float);
INSERT INTO bz VALUES ('d', 0, 0.5), ('d', 10, 0.5);
SELECT repair_key('bz', 'd');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM bz; END $$;

SELECT provenance() AS t FROM bz GROUP BY d HAVING sum(val)    < 5  \gset
SELECT 'sum < 5'     AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bz GROUP BY d HAVING sum(val)    <= 0 \gset
SELECT 'sum <= 0'    AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bz GROUP BY d HAVING sum(val)    = 0  \gset
SELECT 'sum = 0'     AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bz GROUP BY d HAVING sum(val)    >= 0 \gset
SELECT 'sum >= 0'    AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bz GROUP BY d HAVING sum(val)    > 0  \gset
SELECT 'sum > 0'     AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bz GROUP BY d HAVING sum(val)    >= 5 \gset
SELECT 'sum >= 5'    AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;

-- choose() must match sum() on every predicate above (the proper analog).
SELECT provenance() AS t FROM bz GROUP BY d HAVING choose(val) < 5  \gset
SELECT 'choose < 5'  AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bz GROUP BY d HAVING choose(val) <= 0 \gset
SELECT 'choose <= 0' AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bz GROUP BY d HAVING choose(val) = 0  \gset
SELECT 'choose = 0'  AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bz GROUP BY d HAVING choose(val) >= 5 \gset
SELECT 'choose >= 5' AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;

-- Two independent blocks summed (here sum != choose): X in {0,10}, Y in {0,1}.
-- X+Y is {0,1,10,11} each 0.25; the sum=0 world (both chose 0) is a present,
-- non-empty world.  P(sum<11)=0.75, P(sum<=1)=0.5.
CREATE TABLE bxy(blk text, c int, val int, p float);
INSERT INTO bxy VALUES ('X',1,0,0.5),('X',1,10,0.5),('Y',1,0,0.5),('Y',1,1,0.5);
SELECT repair_key('bxy', 'blk');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM bxy; END $$;

SELECT provenance() AS t FROM bxy GROUP BY c HAVING sum(val) < 11 \gset
SELECT 'X+Y < 11'   AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bxy GROUP BY c HAVING sum(val) <= 1 \gset
SELECT 'X+Y <= 1'   AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;

-- COUNT control: no value-0 contributor exists, so COUNT was always correct.
SELECT provenance() AS t FROM bz GROUP BY d HAVING count(*) < 2  \gset
SELECT 'count < 2'  AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;
SELECT provenance() AS t FROM bz GROUP BY d HAVING count(*) >= 1 \gset
SELECT 'count >= 1' AS pred, round(probability_evaluate(:'t'::uuid)::numeric, 3) AS p;

SELECT remove_provenance('bz');
SELECT remove_provenance('bxy');
DROP TABLE bz;
DROP TABLE bxy;
