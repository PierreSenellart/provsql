\set ECHO none

-- =========================================================================
-- Intended provenance semantics for INTERSECT.  Currently
-- unsupported (see doc/source/user/querying.rst, Unsupported SQL
-- Features; today rejected by test/sql/unsupported_features.sql).
--
-- INTERSECT is a JOIN on all columns followed by deduplication, not
-- the m-semiring identity A ∩ B = A ⊖ (A ⊖ B).  The two formulations
-- are equivalent in the Boolean semiring but produce very different
-- circuits in general semirings: the join form is much smaller and
-- avoids the nested ⊖ that would force an m-semiring evaluation.
--
-- Algebraic form: for each value v that appears on both sides, the
-- output row's provenance is
--    ⊕(left_witnesses_of_v) ⊗ ⊕(right_witnesses_of_v),
-- expanded by ⊗-distribution into a ⊕-sum of (left, right) pair
-- products -- the same shape as the rewriter already builds for an
-- explicit equijoin on all selected columns.
-- =========================================================================

SET search_path TO provsql_test,provsql;

-- Cities that are home to both a high-classification (secret or above)
-- and a low-classification (restricted or below) agent.  In the
-- personnel data this is exactly Paris: Magdalen (top_secret) on the
-- left side, Nancy (restricted) on the right.
SELECT city,
       sr_formula(provenance(),'personnel_name') AS formula
FROM (
  SELECT city FROM personnel WHERE classification >= 'secret'
  INTERSECT
  SELECT city FROM personnel WHERE classification <= 'restricted'
) t
ORDER BY city;
