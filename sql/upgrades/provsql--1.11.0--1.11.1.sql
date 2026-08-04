-- ----------------------------------------------------------------------
-- provsql 1.11.0 -> 1.11.1
--
-- A bug-fix release.  Two areas of the aggregate-provenance surface change
-- behaviour; both are corrections, and both are visible to queries.
--
--   * Empty aggregate groups now report SQL NULL.  provsql.sum, min, max
--     and product over a group with no row used to return an identity
--     element -- as_random(0), -inf / +inf, as_random(1) -- while the
--     agg_token path and standard SQL both report NULL there.  The three
--     final functions below are replaced accordingly; avg already agreed.
--     The identity elements keep their real job, which is the value an
--     absent row contributes inside the fold, and are untouched there.
--
--   * A lifted aggregate comparison now supersedes only the compared
--     group's delta gate, not the whole row annotation it sits in.  The
--     planner emits the new provenance_cmp_times, which walks the row
--     tokens through the new C analyser cmp_surviving_factors and keeps
--     whatever the comparison does not subsume -- a join partner's
--     annotation, an earlier comparison on the same group.  Without these
--     two functions the extension still loads and falls back to the
--     previous behaviour, so the order of creation here does not matter.
--
-- No gate type, type, operator, cast or aggregate is added or changed:
-- every object below is a function whose signature is unchanged, so a
-- plain CREATE OR REPLACE suffices and the script is idempotent.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

-- ----------------------------------------------------------------------
-- 1. Aggregate comparison: supersede the compared group's delta only
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION cmp_surviving_factors(tokens uuid[], cmp uuid)
  RETURNS uuid[] AS
  'provsql', 'cmp_surviving_factors' LANGUAGE C PARALLEL SAFE STABLE;

CREATE OR REPLACE FUNCTION provenance_cmp_times(cmp uuid, tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  kept uuid[];
BEGIN
  kept := provsql.cmp_surviving_factors(tokens, cmp);
  IF kept IS NULL OR array_length(kept, 1) IS NULL THEN
    RETURN cmp;
  END IF;
  RETURN provsql.provenance_times(VARIADIC kept || cmp);
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 2. An empty aggregate group is SQL NULL, not an identity element
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION sum_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  arith_token uuid;
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  IF array_length(state, 1) = 1 THEN
    RETURN provsql.random_variable_make(state[1]);
  END IF;
  arith_token := provsql.provenance_arith(0, state);  -- 0 = PROVSQL_ARITH_PLUS
  RETURN provsql.random_variable_make(arith_token);
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION extremum_rv_ffunc(
  state uuid[], op integer, identity double precision)
  RETURNS random_variable AS
$$
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  IF array_length(state, 1) = 1 THEN
    RETURN provsql.random_variable_make(state[1]);
  END IF;
  RETURN provsql.random_variable_make(
    provsql.provenance_arith(op, state));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION product_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  IF array_length(state, 1) = 1 THEN
    RETURN provsql.random_variable_make(state[1]);
  END IF;
  RETURN provsql.random_variable_make(
    provsql.provenance_arith(1, state));  -- 1 = PROVSQL_ARITH_TIMES
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

-- The planner hook consults provenance_cmp_times through the per-session,
-- per-database OID cache that get_constants() fills lazily.  A backend
-- warmed under 1.11.0 holds InvalidOid for it and would silently keep the
-- old whole-attribute behaviour; force a fresh lookup.  (The usual reason
-- for this call is an appended provenance_gate value -- there is none in
-- this release -- but the cache covers function OIDs just the same.)
SELECT reset_constants_cache();
