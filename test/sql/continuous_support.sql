\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Support intervals for base distributions: matches the closed-form
-- supports propagated by RangeCheck.intervalOf.

SELECT lo, hi FROM support(provsql.as_random(7.25));   -- (7.25, 7.25)
SELECT lo, hi FROM support(provsql.normal(0, 1));      -- (-Infinity, +Infinity)
SELECT lo, hi FROM support(provsql.uniform(1, 3));     -- (1, 3)
SELECT lo, hi FROM support(provsql.exponential(2));    -- (0, +Infinity)
SELECT lo, hi FROM support(provsql.erlang(3, 1));      -- (0, +Infinity)

-- Interval-arithmetic propagation through gate_arith.  The lateral
-- subselect is needed so support() sees the column from the enclosing
-- row.

SELECT (support(provsql.uniform(1, 3) + provsql.uniform(2, 5))).*; -- (3, 8) PLUS
SELECT (support(provsql.uniform(1, 3) - provsql.uniform(0, 2))).*; -- (-1, 3) MINUS
SELECT (support(-provsql.uniform(2, 5))).*;                        -- (-5, -2) NEG
SELECT (support(provsql.uniform(2, 4) * provsql.uniform(1, 3))).*; -- (2, 12) TIMES
SELECT (support(provsql.uniform(1, 3) / 2)).*;                     -- (0.5, 1.5) DIV-by-const

-- Normal-tainted: any sub-circuit reaching a normal RV has the
-- conservative all-real support.
SELECT (support(provsql.normal(0, 1) + provsql.uniform(1, 3))).*;

-- Conditioning on prov: not supported for random_variable.  Use a
-- precomputed gate UUID (gate_zero) as the prov so the call doesn't
-- accidentally fail earlier on a non-existent gate.
DO $$
DECLARE msg text; matched bool;
BEGIN
  PERFORM lo FROM support(provsql.normal(0, 1), gate_zero());
  RAISE NOTICE 'rv_support_with_prov_did_not_raise';
EXCEPTION WHEN OTHERS THEN
  msg := SQLERRM;
  matched := position('not yet supported for circuit-token' in msg) > 0;
  RAISE NOTICE 'rv_support_prov_raises_specific=%', matched;
END
$$;

-- agg_token path.

CREATE TABLE support_agg_t(p text, v int);
INSERT INTO support_agg_t VALUES ('a', 10), ('b', -20), ('c', 30);
SELECT add_provenance('support_agg_t');

-- SUM support: lo = sum of negative v_i = -20; hi = sum of positive
-- v_i = 10 + 30 = 40.
DO $$
DECLARE s agg_token; lo_v float8; hi_v float8;
BEGIN
  SELECT sum(v) INTO s FROM support_agg_t;
  SELECT lo, hi INTO lo_v, hi_v FROM support(s);
  RAISE NOTICE 'agg_sum_support_lo=%', lo_v;
  RAISE NOTICE 'agg_sum_support_hi=%', hi_v;
END
$$;

-- MIN support: with default p=1.0 for every row, empty world is
-- impossible (P(no row) = 0).  Support = [min(v), max(v)] = [-20, 30].
DO $$
DECLARE mn agg_token; lo_v float8; hi_v float8;
BEGIN
  SELECT min(v) INTO mn FROM support_agg_t;
  SELECT lo, hi INTO lo_v, hi_v FROM support(mn);
  RAISE NOTICE 'agg_min_support_lo=%', lo_v;
  RAISE NOTICE 'agg_min_support_hi=%', hi_v;
END
$$;

-- Drop probability of every row to introduce a non-zero empty-world
-- probability, so MIN's hi flips to +Infinity (empty MIN convention).
DO $$
BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM support_agg_t;
END
$$;

DO $$
DECLARE mn agg_token; mx agg_token; pr uuid;
        mn_lo float8; mn_hi float8;
        mx_lo float8; mx_hi float8;
BEGIN
  -- Unconditional: empty world has positive probability.
  SELECT min(v), max(v) INTO mn, mx FROM support_agg_t;
  SELECT lo, hi INTO mn_lo, mn_hi FROM support(mn);
  SELECT lo, hi INTO mx_lo, mx_hi FROM support(mx);
  RAISE NOTICE 'agg_min_uncond_lo=%, hi=%', mn_lo, mn_hi;
  RAISE NOTICE 'agg_max_uncond_lo=%, hi=%', mx_lo, mx_hi;

  -- Conditioning on provenance() restricts to non-empty worlds; no
  -- ±Infinity bound flip.
  SELECT min(v), max(v), provenance() INTO mn, mx, pr FROM support_agg_t;
  SELECT lo, hi INTO mn_lo, mn_hi FROM support(mn, pr);
  SELECT lo, hi INTO mx_lo, mx_hi FROM support(mx, pr);
  RAISE NOTICE 'agg_min_cond_lo=%, hi=%', mn_lo, mn_hi;
  RAISE NOTICE 'agg_max_cond_lo=%, hi=%', mx_lo, mx_hi;
END
$$;

-- Unsupported aggregation function raises clearly.  AVG is rejected
-- with a "Cannot compute support for aggregation function avg"
-- message; we only assert the raise here.
DO $$
DECLARE
  agg_avg agg_token;
  matched bool;
BEGIN
  SELECT avg(v) INTO agg_avg FROM support_agg_t;
  PERFORM lo FROM support(agg_avg);
  RAISE NOTICE 'agg_avg_support_did_not_raise';
EXCEPTION WHEN OTHERS THEN
  matched := position('Cannot compute support' in SQLERRM) > 0;
  RAISE NOTICE 'agg_avg_support_raises_specific=%', matched;
END
$$;

-- Plain numeric inputs: degenerate point support, no need to wrap
-- in as_random.  Each numeric type goes through the same branch.
SELECT (support(7::int)).*;             -- (7, 7)
SELECT (support(2.5::numeric)).*;       -- (2.5, 2.5)
SELECT (support((-3.14)::float8)).*;    -- (-3.14, -3.14)

-- Bare UUID: route to rv_support directly.  A gate_value UUID gives a
-- point support; a gate_rv UUID gives the distribution's support.
SELECT (support(provsql.random_variable_uuid(provsql.as_random(9.5)))).*;
SELECT (support(provsql.random_variable_uuid(provsql.uniform(2, 5)))).*;
-- A non-scalar gate falls back to the conservative all-real interval
-- without raising (gate_one is the identity, not a scalar).
SELECT (support(gate_one())).*;

-- text input: still raises the polymorphic rejection.
DO $$
DECLARE matched bool;
BEGIN
  PERFORM lo FROM support('toto'::text);
  RAISE NOTICE 'text_support_did_not_raise';
EXCEPTION WHEN OTHERS THEN
  matched := position('not yet supported for input type' in SQLERRM) > 0;
  RAISE NOTICE 'text_support_raises_specific=%', matched;
END
$$;

-- agg_token over non-numeric values (only MIN/MAX, since SUM(text)
-- doesn't typecheck at PG level).  The CAST(get_extra(...) AS float8)
-- inside the MIN/MAX branch raises -- same shape as existing
-- expected() / variance() over the same agg_token.
CREATE TABLE support_text_t(p text, name text);
INSERT INTO support_text_t VALUES ('a', 'alice'), ('b', 'bob');
SELECT add_provenance('support_text_t');

DO $$
DECLARE
  m agg_token;
  matched bool;
BEGIN
  SELECT min(name) INTO m FROM support_text_t;
  PERFORM lo FROM support(m);
  RAISE NOTICE 'text_min_support_did_not_raise';
EXCEPTION WHEN OTHERS THEN
  matched := position('invalid input syntax for type double precision'
                      in SQLERRM) > 0;
  RAISE NOTICE 'text_min_support_raises_cast_error=%', matched;
END
$$;

DROP TABLE support_text_t;
DROP TABLE support_agg_t;
