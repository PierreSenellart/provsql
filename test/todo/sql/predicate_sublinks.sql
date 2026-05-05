\set ECHO none

-- =========================================================================
-- Intended provenance semantics for predicate sublinks: subqueries
-- evaluated as a Boolean in a WHERE clause.  Covers EXISTS, NOT EXISTS,
-- IN, and NOT IN.  See scalar_sublinks.sql for value-returning
-- sublinks.
--
-- Currently unsupported; ProvSQL rejects all sublink shapes from a
-- single rewriter point with the error
--   "Subqueries (EXISTS, IN, scalar subquery) not supported".
--
-- Each section captures the desired form once support lands.  The
-- rendered formulas in the expected output are best-guesses based on
-- existing renderer conventions (notably having_on_aggregation.out
-- for the (𝟙 ⊖ ...) form); they are likely to need adjustment when
-- the implementation is in place.
-- =========================================================================

SET search_path TO provsql_test,provsql;

-- -------------------------------------------------------------------------
-- Case 1: EXISTS sublink.
-- Algebraic form: outer_token ⊗ (⊕ over inner tokens whose row matches
-- the correlation).  The outer row appears iff at least one inner
-- witness exists; its formula records the disjunction of all such
-- witnesses.
--
-- Correlation: an agent with strictly higher classification.
-- Magdalen (top_secret) has none; she is filtered out.
-- -------------------------------------------------------------------------
SELECT p1.name,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel p1
WHERE EXISTS (
  SELECT 1 FROM personnel p2 WHERE p2.classification > p1.classification
)
ORDER BY p1.id;

-- -------------------------------------------------------------------------
-- Case 2: NOT EXISTS sublink.
-- Algebraic form: outer_token ⊗ (𝟙 ⊖ ⊕(matching inner tokens)).
-- The outer row appears iff no inner witness exists; its formula
-- records the monus against the (potentially empty) ⊕-sum of
-- candidates.  When the candidate set is empty, the monus simplifies
-- (𝟙 ⊖ 𝟘) -- the explicit form is shown here to illustrate the
-- structure; the rewriter is free to elide trivial sub-circuits.
-- -------------------------------------------------------------------------
SELECT p1.name,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel p1
WHERE NOT EXISTS (
  SELECT 1 FROM personnel p2 WHERE p2.classification > p1.classification
)
ORDER BY p1.id;

-- -------------------------------------------------------------------------
-- Case 3: IN sublink.
-- Algebraic form: outer_token ⊗ (⊕ over inner tokens whose row matches
-- the equality on the compared expression).  Equivalent in semantics
-- to EXISTS with an explicit equijoin condition.
--
-- The inner returns Berlin classifications (only "secret", witnessed
-- by both Ellen and Susan).  For each outer row whose classification
-- is "secret", both witnesses contribute.
-- -------------------------------------------------------------------------
SELECT name,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel
WHERE classification IN (
  SELECT classification FROM personnel WHERE city = 'Berlin'
)
ORDER BY id;

-- -------------------------------------------------------------------------
-- Case 4: NOT IN sublink.
-- Algebraic form: outer_token ⊗ (𝟙 ⊖ ⊕(matching inner tokens)).
-- Symmetric to NOT EXISTS, with the outer-vs-inner correlation
-- expressed by equality on the compared expression rather than by an
-- explicit subquery WHERE clause.
--
-- In the personnel data the (city, classification) pairs are unique,
-- so the inner-matching set is empty for every passing outer row and
-- the monus reduces to (𝟙 ⊖ 𝟘); the cmp gates that the rewriter
-- would emit to enforce the equality are not visible in sr_formula.
-- -------------------------------------------------------------------------
SELECT name,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel
WHERE classification NOT IN (
  SELECT classification FROM personnel WHERE city = 'Berlin'
)
ORDER BY id;
