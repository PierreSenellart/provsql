\set ECHO none
\pset format unaligned

-- ==========================================================================
-- Comparison-event surface + the `probability` short alias.
--
-- An RV comparison (x > y) is an uncertain event whose natural value is a
-- provenance token.  The planner hook lifts it into that token (a gate_cmp
-- uuid) when it is projected in the SELECT list, and the
-- probability(<predicate>) / probability_evaluate(<predicate>) Boolean
-- overloads evaluate the event's probability with the natural infix grammar.
-- `probability` is also a thin alias of probability_evaluate on a uuid.
-- MC-backed here; the closed forms (Uniform-Uniform, ...) are asserted in
-- continuous_analytic.
-- ==========================================================================

SET provsql.active = on;
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples = 20000;
SET search_path TO public, provsql;

CREATE TABLE d AS SELECT uniform(0,1) AS x, uniform(0,1) AS y, uniform(0,1) AS z;

-- `probability` is a thin alias of `probability_evaluate` (same C symbol).
-- P(U(0,1) > 0.5) = 0.5 is resolved analytically (closed-form CDF), so the
-- alias and the long name agree exactly.
SELECT probability(t) = probability_evaluate(t) AS alias_agrees,
       probability(t) = 0.5 AS alias_half
  FROM (SELECT rv_cmp_gt(uniform(0,1), as_random(0.5)) AS t) s;

-- Projected comparison: x > y surfaces its event uuid instead of raising in
-- random_variable_cmp_placeholder.  Checked through a subquery so the outer
-- IS NOT NULL / pg_typeof do not themselves sit in a lifted position.
SELECT t IS NOT NULL AS projected_present,
       pg_typeof(t) = 'uuid'::regtype AS projected_is_uuid
  FROM (SELECT x > y AS t FROM d) s;

-- probability(<predicate>) over RV comparisons, MC-backed (seed pinned).
-- P(X > Y) = 1/2 for i.i.d. uniforms.
SELECT abs(probability(x > y) - 0.5) < 0.05 AS p_gt_half FROM d;

-- P(X > Y AND X < Z) = 1/6 (the ordering Y < X < Z, one of 3! = 6 orders).
SELECT abs(probability(x > y AND x < z) - 1.0/6) < 0.05 AS p_middle FROM d;

-- The predicate surface lives only on `probability`; probability_evaluate
-- keeps just its uuid overload (a boolean overload of the long name would make
-- probability_evaluate('<uuid-literal>') ambiguous).
SELECT probability_evaluate(rv_cmp_gt(uniform(0,1), as_random(0.5))) = 0.5
       AS pe_uuid_unchanged;

-- A predicate mixing an RV comparison with an always-true regular one keeps
-- the RV probability (the regular conjunct is a certain indicator).
SELECT abs(probability(x > y AND 1 > 0) - 0.5) < 0.05 AS p_mixed FROM d;

-- Totality over deterministic Booleans: the probability of a definite event
-- is 1 when it holds, 0 when it does not, NULL for an unknown.  Handled by
-- the SQL body alone, so it holds even with no probabilistic comparison.
SELECT probability(1 > 0)          AS p_true,
       probability(1 > 2)          AS p_false,
       probability(TRUE)           AS p_t,
       probability(FALSE)          AS p_f,
       probability(NULL::boolean) IS NULL AS p_null;

DROP TABLE d;

SELECT 'ok'::text AS continuous_comparison_surface_done;
