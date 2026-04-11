/**
 * @file
 * @brief ProvSQL upgrade script: 1.2.0 → 1.2.1
 *
 * Changes in this release that affect the SQL API:
 *
 * - @c create_provenance_mapping_view was moved out of the PostgreSQL
 *   14+ specific file (@c provsql.14.sql) into the version-independent
 *   file (@c provsql.common.sql), making it available on all
 *   supported PostgreSQL versions.
 *
 * For users on PostgreSQL 14+ this is a no-op: the function already
 * exists with the same body and the @c CREATE OR REPLACE below simply
 * rewrites it in place.  For users on PostgreSQL 12 or 13, the function
 * is newly added.
 */

SET search_path TO provsql;

CREATE OR REPLACE FUNCTION create_provenance_mapping_view(
  newview text,
  oldtbl regclass,
  att text,
  preserve_case bool DEFAULT false
)
RETURNS void
LANGUAGE plpgsql
AS
$$
BEGIN
  IF preserve_case THEN
    EXECUTE format(
      'CREATE OR REPLACE VIEW %I AS SELECT %s AS value, provsql AS provenance FROM %s',
      newview,
      att,
      oldtbl
    );
  ELSE
    EXECUTE format(
      'CREATE OR REPLACE VIEW %s AS SELECT %s AS value, provsql AS provenance FROM %s',
      newview,
      att,
      oldtbl
    );
  END IF;
END;
$$;
