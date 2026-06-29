\set ECHO none
\pset format unaligned

SET TIME ZONE 'UTC';
SET datestyle = 'iso';

-- A maintained validity mapping must stay correct across data modification.
-- Deleting a row rewrites its provsql into a monus gate, but with a maintained
-- mapping the validity stays keyed to the *original input token* (the monus
-- gate's child), so sr_temporal still reports the row's term with its original
-- lower bound, bounded at the deletion instant -- not the unbounded interval a
-- view-based mapping produced (it lost the lower bound once provsql was
-- rewritten).

CREATE TABLE office(id int, holder text, validity tstzmultirange);
-- validity set before add_provenance so the update trigger does not fire
INSERT INTO office VALUES
  (1, 'Alice', tstzmultirange(tstzrange('2000-01-01+00','2010-01-01+00')));
SELECT add_provenance('office');
SELECT create_provenance_mapping('office_validity', 'office', 'validity', maintained => true);

-- Extend time_validity_view so sr_temporal resolves both the row inputs and
-- the data-modification (delete) tokens.
ALTER VIEW provsql.time_validity_view RENAME TO time_validity_view_save;
CREATE VIEW provsql.time_validity_view AS
    SELECT * FROM provsql.time_validity_view_save
  UNION ALL SELECT * FROM office_validity;

-- A row inserted AFTER the mapping was created is picked up automatically
-- (the guard appends it, keyed to its fresh input token).
INSERT INTO office VALUES
  (2, 'Bob', tstzmultirange(tstzrange('2020-01-01+00', NULL)));   -- ongoing

-- Full validity of each holder before any modification.  Materialise then
-- remove_provenance so the deterministic multirange is compared, not the
-- random input-token UUID the rewriter appends.
CREATE TABLE before_mod AS
SELECT holder, sr_temporal(provenance(), 'provsql.time_validity_view') AS valid
FROM office;
SELECT remove_provenance('before_mod');
SELECT holder, valid FROM before_mod ORDER BY holder;
DROP TABLE before_mod;

-- Dismiss the ongoing holder at a fixed instant.
SET provsql.update_provenance = on;
DELETE FROM update_provenance;
DELETE FROM office WHERE holder = 'Bob';
UPDATE update_provenance
   SET valid_time = tstzmultirange(tstzrange('2026-01-01+00', NULL));
SET provsql.update_provenance = off;

-- Bob's tenure is now [2020-01-01, 2026-01-01): original lower bound kept,
-- upper bound at the deletion instant.
CREATE TABLE after_mod AS
SELECT holder, sr_temporal(provenance(), 'provsql.time_validity_view') AS valid
FROM office WHERE holder = 'Bob';
SELECT remove_provenance('after_mod');
SELECT holder, valid FROM after_mod ORDER BY holder;
DROP TABLE after_mod;

-- Clean up (restore the global view).
DROP VIEW provsql.time_validity_view;
DROP TABLE office_validity;
ALTER VIEW provsql.time_validity_view_save RENAME TO time_validity_view;
DROP TABLE office;
DELETE FROM update_provenance;
