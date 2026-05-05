\set ECHO none

-- =========================================================================
-- Intended provenance semantics for GROUPING SETS, CUBE, and ROLLUP.
-- Currently unsupported (see doc/source/user/querying.rst, Unsupported
-- SQL Features; today rejected by test/sql/unsupported_features.sql).
--
-- Each grouping subset desugars to a separate GROUP BY whose result
-- is UNION ALL'd with the others; columns absent from a subset get
-- NULL.  ProvSQL already handles GROUP BY and UNION ALL, so the
-- intended provenance is the existing δ-aggregation applied
-- per-subset:
--
--   * a non-empty grouping (city), (classification), ... -> one
--     output row per group, with provenance δ(⊕ contributors);
--   * the empty grouping () -> a single row aggregating every input
--     tuple, with provenance δ(⊕ all rows of the source).
--
-- CUBE (a, b) and ROLLUP (a, b) reduce to GROUPING SETS:
--   CUBE   (a, b) ≡ GROUPING SETS ((a,b), (a), (b), ())
--   ROLLUP (a, b) ≡ GROUPING SETS ((a,b), (a),       ())
-- so supporting GROUPING SETS automatically covers them.
-- =========================================================================

SET search_path TO provsql_test,provsql;

-- Three grouping subsets in one query: by city, by classification,
-- and grand total.  Each produces its own per-group δ-aggregation;
-- the grand-total row's δ argument ⊕-sums every input tuple.
SELECT city, classification, COUNT(*) AS n,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel
GROUP BY GROUPING SETS ((city), (classification), ())
ORDER BY city NULLS LAST, classification NULLS LAST;
