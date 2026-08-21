\set ECHO none
\pset format unaligned

-- The circuit store only grows: a rolled-back transaction leaves its
-- gates behind as orphans, and so does every dropped table and every
-- exploratory query.  circuit_cleanup() is the one operation allowed to
-- remove them, and it keeps everything the tokens stored in the database
-- reach -- whatever column they sit in.

CREATE TABLE cc_base (name text);
INSERT INTO cc_base VALUES ('alice'), ('bob'), ('carol');
SELECT add_provenance('cc_base');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM cc_base; END $$;

-- A derived table: its token is a plus gate over the three inputs, and it
-- lives in a column that is not called provsql.
CREATE TABLE cc_derived AS
  SELECT provenance() AS tok FROM (SELECT DISTINCT 1 FROM cc_base) x;
SELECT remove_provenance('cc_derived');
SELECT round(probability_evaluate(tok)::numeric, 4) AS derived_before
  FROM cc_derived;

-- Orphans: a rolled-back join creates gates that no committed row can
-- ever reference.
BEGIN;
CREATE TABLE cc_rolled (name text);
INSERT INTO cc_rolled VALUES ('alice'), ('bob');
SELECT add_provenance('cc_rolled');
DO $$ BEGIN PERFORM provenance() FROM cc_base, cc_rolled
             WHERE cc_base.name = cc_rolled.name; END $$;
ROLLBACK;

-- A dry run measures without writing.
SELECT gates_after < gates_before AS dry_run_finds_orphans,
       wires_after IS NULL AS dry_run_skips_the_rewrite
  FROM circuit_cleanup(true);
SELECT get_nb_gates() > 0 AS dry_run_wrote_nothing;

-- The real run.
SELECT gates_after < gates_before AS shrank FROM circuit_cleanup();

-- Everything reachable survived, with its probabilities.
SELECT round(probability_evaluate(tok)::numeric, 4) AS derived_after
  FROM cc_derived;
SET provsql.active = off;
SELECT bool_and(get_prob(provsql) = 0.5) AS probabilities_kept FROM cc_base;
SET provsql.active = on;

-- A rebuilt store is consistent, and a second run has nothing left to do.
SELECT unclean_shutdown, dangling_indices, unreferenced, bad_wires, bad_extra
  FROM check_store();
SELECT gates_before = gates_after AS second_run_is_a_no_op FROM circuit_cleanup();

DROP TABLE cc_derived;
DROP TABLE cc_base;
