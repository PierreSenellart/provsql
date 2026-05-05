\set ECHO none

-- =========================================================================
-- Intended provenance semantics for DISTINCT ON.  Currently
-- unsupported (see doc/source/user/querying.rst, Unsupported SQL
-- Features).
--
-- DISTINCT ON desugars to a GROUP BY on the key columns where each
-- non-key projected column is an implicit `choose` aggregate over the
-- group (see doc/source/user/aggregation.rst for the choose
-- aggregate).  The ORDER BY clause makes the choice deterministic at
-- the SQL level, but in possible-world semantics the value of every
-- non-key column is uncertain: if the chosen tuple is absent, the
-- next tuple in ORDER BY would supply the value.  Each non-key column
-- therefore returns an aggregate-typed value, displayed with the
-- "(*)" marker that ProvSQL uses for aggregate cells.
--
-- The expected formula uses the same choose(token*value, ...)
-- rendering as test/expected/choose.out, with the value being the
-- *name* column (chosen by virtue of the mapping being
-- personnel_name).  In a real implementation, which non-key column's
-- choose-token provenance() refers to is an implementation detail
-- that may shift this rendering.
-- =========================================================================

SET search_path TO provsql_test,provsql;

-- DISTINCT ON (city): pick the lowest-classification agent per city.
-- Ties on classification are broken by id (ascending).  The chosen
-- tuple supplies the displayed name and classification, both of which
-- become aggregate-typed values; the formula records the choose
-- structure over the city group.
SELECT DISTINCT ON (city)
       name, city, classification,
       sr_formula(provenance(),'personnel_name') AS formula
FROM personnel
ORDER BY city, classification, id;
