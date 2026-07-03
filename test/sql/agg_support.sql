\set ECHO none
\pset format unaligned

-- support() over aggregate tokens: the per-aggregate support-interval
-- computation (aggSupportOf) over possibly-present rows.
--   sum:   per-row contribution is the value if the row fires, else 0,
--          so [Σ min(0, vᵢ), Σ max(0, vᵢ)];
--   count: [0, n];
--   min /
--   max:   the extremum is one of the firing rows' values, so the union
--          of the value intervals.

CREATE TABLE sup(g int, v int);
INSERT INTO sup VALUES (1,10),(1,-3),(1,7);
SELECT add_provenance('sup');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM sup; END $$;

CREATE TABLE supr AS
  SELECT g,
         (support(sum(v))).lo   AS sum_lo, (support(sum(v))).hi   AS sum_hi,
         (support(count(*))).lo AS cnt_lo, (support(count(*))).hi AS cnt_hi,
         (support(min(v))).lo   AS min_lo, (support(min(v))).hi   AS min_hi,
         (support(max(v))).lo   AS max_lo, (support(max(v))).hi   AS max_hi
  FROM sup GROUP BY g;
SELECT remove_provenance('supr');
SELECT * FROM supr;
DROP TABLE supr;

-- avg has no world-independent support (the ratio depends on the row
-- count): explicit refusal.
CREATE TABLE supa AS SELECT g, avg(v) AS a FROM sup GROUP BY g;
SELECT remove_provenance('supa');
SELECT (support(a)).lo FROM supa;

-- The same intervals through the C fallback (rv_support -> compute_support
-- -> aggSupportOf): support(uuid) on the aggregation gate itself
-- (agg_token casts implicitly to the gate's uuid).  avg has no closed
-- form there either, but the C side answers with the conservative
-- all-real interval instead of raising.
-- count(*) lowers to a SUM of per-row 0/1 indicators; count(v) keeps the
-- COUNT operator at the gate level, so both arms are exercised.
CREATE TABLE supg AS
  SELECT g, sum(v) AS s, count(*) AS c, count(v) AS cv, min(v) AS mn,
         max(v) AS mx, avg(v) AS a
  FROM sup GROUP BY g;
SELECT remove_provenance('supg');
SELECT (support(s::uuid)).*  FROM supg;
SELECT (support(c::uuid)).*  FROM supg;
SELECT (support(cv::uuid)).* FROM supg;
SELECT (support(mn::uuid)).* FROM supg;
SELECT (support(mx::uuid)).* FROM supg;
SELECT (support(a::uuid)).*  FROM supg;

-- A semimod gate (per-row contribution value . 1_{k fires}) as the support
-- root: union of {0} and the value's range.
SELECT get_gate_type(ch) AS t, (support(ch)).lo, (support(ch)).hi
FROM (SELECT unnest(get_children(s::uuid)) AS ch FROM supg) sub
ORDER BY lo, hi;
DROP TABLE supg;

DROP TABLE supa;
DROP TABLE sup;
