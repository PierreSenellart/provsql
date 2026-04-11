/**
 * @file
 * @brief ProvSQL upgrade script: 1.2.1 → 1.2.2
 *
 * No SQL-surface changes in this release; the upgrade script is a
 * no-op.  PostgreSQL still requires its presence to offer an
 * ALTER EXTENSION provsql UPDATE path between these versions.
 */
SET search_path TO provsql;
