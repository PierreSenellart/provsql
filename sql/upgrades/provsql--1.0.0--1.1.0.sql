/**
 * @file
 * @brief ProvSQL upgrade script: 1.0.0 → 1.1.0
 *
 * Changes in this release that affect the SQL API:
 *
 * - Added five new C-level cast helper functions on @c agg_token
 *   (@c agg_token_to_numeric, @c agg_token_to_float8,
 *   @c agg_token_to_int4, @c agg_token_to_int8,
 *   @c agg_token_to_text) that extract the scalar value of an
 *   aggregate result, discarding the provenance token.
 *
 * - Registered matching PostgreSQL @c CREATE @c CAST rules so that
 *   @c agg_token values participate in arithmetic and string
 *   conversions: the numeric cast is @c IMPLICIT (to allow
 *   expressions like @c SUM(x)*2 without explicit casting),
 *   while @c double @c precision / @c integer / @c bigint / @c text
 *   are @c ASSIGNMENT casts.
 *
 * All additions are new — no pre-existing objects are altered.  The
 * upgrade is therefore a pure additive change that cannot be re-run
 * (PostgreSQL's extension mechanism only fires this script once for
 * a given 1.0.0 → 1.1.0 transition).
 */

SET search_path TO provsql;

/** @brief Cast an agg_token to numeric (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_numeric(agg_token)
  RETURNS numeric
  AS 'provsql','agg_token_to_numeric' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to double precision (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_float8(agg_token)
  RETURNS double precision
  AS 'provsql','agg_token_to_float8' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to integer (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_int4(agg_token)
  RETURNS integer
  AS 'provsql','agg_token_to_int4' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to bigint (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_int8(agg_token)
  RETURNS bigint
  AS 'provsql','agg_token_to_int8' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to text (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_text(agg_token)
  RETURNS text
  AS 'provsql','agg_token_to_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Implicit PostgreSQL cast from agg_token to numeric (enables arithmetic on aggregates) */
CREATE CAST (agg_token AS numeric) WITH FUNCTION agg_token_to_numeric(agg_token) AS IMPLICIT;
/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;
