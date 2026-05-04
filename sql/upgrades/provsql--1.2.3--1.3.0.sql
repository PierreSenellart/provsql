/**
 * @file
 * @brief ProvSQL upgrade script: 1.2.3 → 1.3.0
 *
 * 1.3.0 changes the circuit storage layout from a single set of flat
 * files ($PGDATA/provsql_*.mmap, shared by all databases) to
 * per-database files ($PGDATA/base/<db_oid>/provsql_*.mmap).
 *
 * If old flat files are still present in $PGDATA, the circuit data
 * they contain has not been migrated and will be inaccessible.
 * In that case, this script raises a WARNING with recovery instructions.
 *
 * The correct upgrade procedure is:
 *   1. While the old PostgreSQL is still running, run:
 *        provsql_migrate_mmap -D $PGDATA -c <connstr>
 *      The tool migrates each database's circuit data into the new
 *      per-database files and deletes the old flat files on success.
 *   2. Install the new ProvSQL binaries (make install).
 *   3. Restart PostgreSQL.
 *   4. In each database: ALTER EXTENSION provsql UPDATE;
 *
 * If step 1 was skipped, follow the recovery instructions in the
 * WARNING message below.
 */
SET search_path TO provsql;

DO $$
BEGIN
  IF EXISTS (
    SELECT 1 FROM pg_ls_dir('.') WHERE pg_ls_dir = 'provsql_gates.mmap'
  ) THEN
    RAISE WARNING
      E'ProvSQL 1.3.0: old flat circuit files detected in the data directory.\n'
      'Circuit data stored in those files has NOT been migrated to the new\n'
      'per-database layout and is currently inaccessible.\n'
      '\n'
      'To recover:\n'
      '  1. Run: provsql_migrate_mmap -D %s -c <connstr>\n'
      '     The tool migrates all databases and deletes the old flat files.\n'
      '     Run it before any provenance query executes; if queries have already\n'
      '     run, first delete any empty per-database files:\n'
      '       rm -f %s/base/*/provsql_*.mmap\n'
      '     then restart PostgreSQL and run the tool immediately.\n'
      '  2. Restart PostgreSQL so the background worker picks up the migrated files.',
      current_setting('data_directory'),
      current_setting('data_directory');
  END IF;
END
$$;
