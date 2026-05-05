\set ECHO none

-- =========================================================================
-- Intended provenance semantics for scalar sublinks: subqueries that
-- return a single column and a single row, used as expressions in the
-- SELECT list or in WHERE predicates.  Currently unsupported; the
-- ProvSQL planner hook does not traverse SubLink/SubPlan nodes, so
-- these queries either error or silently drop the sublink's
-- provenance.  See doc/source/user/querying.rst (Unsupported SQL
-- Features) for context.
--
-- Each section captures the desired form once support lands.  The
-- rendered formulas in the expected output are best-guesses based on
-- existing renderer conventions (notably aggregation.out for δ); they
-- are likely to need adjustment when the implementation is in place.
-- =========================================================================

SET search_path TO provsql_test,provsql;

-- -------------------------------------------------------------------------
-- Case 1: Uncorrelated, returns a base tuple value.
-- Algebraic form: outer_token ⊗ inner_token.
-- The subquery yields the row for id = 1 (John); the outer row's
-- provenance becomes a ⊗-product of its own token with John's.
-- -------------------------------------------------------------------------
SELECT name,
       (SELECT name FROM personnel WHERE id = 1) AS first_name,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel
ORDER BY id;

-- -------------------------------------------------------------------------
-- Case 2: Uncorrelated, returns an aggregate.
-- Algebraic form: outer_token ⊗ δ(agg_token).
-- The subquery yields a single COUNT row whose provenance token is
-- δ(⊕ over all personnel); the outer row's provenance becomes the
-- ⊗-product of its own token with that δ-lifted aggregate.
-- -------------------------------------------------------------------------
SELECT name,
       (SELECT COUNT(*) FROM personnel) AS total,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel
ORDER BY id;

-- -------------------------------------------------------------------------
-- Case 3: Correlated (semantically equivalent to LATERAL).
-- Algebraic form: outer_token ⊗ δ(per-correlation agg_token).
-- Each outer row carries the ⊗-product of its own token with a δ-lifted
-- COUNT over personnel sharing its city.
-- -------------------------------------------------------------------------
SELECT p1.name,
       (SELECT COUNT(*) FROM personnel p2 WHERE p2.city = p1.city) AS city_count,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel p1
ORDER BY p1.id;

-- -------------------------------------------------------------------------
-- Case 4: Scalar subquery in a WHERE predicate (compares against an
-- aggregate value).
-- Algebraic form: outer_token ⊗ δ(agg_token), additionally constrained
-- by a cmp gate over the extracted aggregate value.  cmp does not render
-- in sr_formula today (it is observable only via probability_evaluate /
-- sr_boolexpr), so the visible formula is just outer ⊗ δ(...).
-- -------------------------------------------------------------------------
SELECT name,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel
WHERE id > (SELECT AVG(id) FROM personnel)
ORDER BY id;
