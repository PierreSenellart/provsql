-- ----------------------------------------------------------------------
-- provsql 1.13.0 -> 1.14.0
--
-- sr_formula_rows: the formula of a token as rows of bounded size, the
-- large sub-formulas named and given rows of their own, produced one at
-- a time from the parts of the circuit each row reads.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

CREATE FUNCTION sr_formula_rows(
  token ANYELEMENT, token2value regclass = NULL, max_size INT = 50)
  RETURNS TABLE(name TEXT, formula TEXT) AS
  'provsql', 'sr_formula_rows' LANGUAGE C PARALLEL SAFE STABLE;
