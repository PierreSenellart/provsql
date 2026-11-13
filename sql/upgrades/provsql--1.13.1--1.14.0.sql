-- ----------------------------------------------------------------------
-- provsql 1.13.1 -> 1.14.0
--
-- Cache of compiled d-DNNFs keyed by the canonical form of a circuit:
-- the functions that show a circuit's canonical form and its island key,
-- and those that read and empty the cache of the session.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

CREATE FUNCTION circuit_canonical_form(token UUID)
  RETURNS TEXT AS
  'provsql','circuit_canonical_form' LANGUAGE C STRICT;

CREATE FUNCTION circuit_island_key(token UUID)
  RETURNS TEXT AS
  'provsql','circuit_island_key' LANGUAGE C STRICT;

CREATE FUNCTION prob_cache_stats()
  RETURNS jsonb AS
  'provsql','prob_cache_stats' LANGUAGE C;

CREATE FUNCTION prob_cache_reset()
  RETURNS void AS
  'provsql','prob_cache_reset' LANGUAGE C;
