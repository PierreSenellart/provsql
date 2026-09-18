\set ECHO none
\pset format unaligned

-- Each data-modification statement mints an update gate of its own, and
-- every statement of one transaction also names the transaction's gate:
-- the effect on a row is times(transaction, statement).  So a reader of
-- update_provenance can tell which statements were one transaction, and
-- undo() reverses either one statement or the whole transaction.

SET provsql.update_provenance = on;

CREATE TABLE tt_e (id int, v text);
INSERT INTO tt_e VALUES (1, 'a'), (2, 'b'), (3, 'c');
SELECT add_provenance('tt_e');
SELECT create_provenance_mapping('tt_map', 'tt_e', 'v');
DELETE FROM update_provenance;

BEGIN;
DELETE FROM tt_e WHERE id = 1;
UPDATE tt_e SET v = 'B' WHERE id = 2;
COMMIT;

-- One transaction row, and both statements point at it.  The assertions
-- run with provsql.active off so no token reaches the output: the values
-- are what matters, and the UUIDs differ run to run.
SET provsql.active = off;
SELECT count(*) FILTER (WHERE query_type = 'TRANSACTION') AS transactions,
       count(*) FILTER (WHERE tx_token IS NOT NULL) AS statements_with_a_transaction,
       count(DISTINCT tx_token) AS distinct_transactions,
       count(*) FILTER (WHERE xid IS NULL) AS rows_without_an_xid
  FROM update_provenance;

-- A transaction has no query text of its own, so looking a statement up
-- by its text finds the statement and not the transaction.
SELECT query_type FROM update_provenance
 WHERE query LIKE 'DELETE FROM tt_e%';

SELECT id, v, sr_formula(provsql, 'tt_map') AS formula FROM tt_e ORDER BY id, v;

SELECT provsql AS tx FROM update_provenance WHERE query_type = 'TRANSACTION' \gset
SET provsql.active = on;

-- Undoing the transaction reverses both of its statements at once.
SELECT undo(:'tx'::uuid) IS NOT NULL AS undone;

SET provsql.active = off;
SELECT id, v, sr_formula(provsql, 'tt_map') AS formula FROM tt_e ORDER BY id, v;

-- The transaction's own validity is the universal range, the identity of
-- the temporal m-semiring: it multiplies into every effect of the
-- transaction, so anything else would intersect itself into all of them.
SELECT valid_time = '{(,)}'::tstzmultirange AS transaction_validity_is_neutral
  FROM update_provenance WHERE query_type = 'TRANSACTION' ORDER BY ts LIMIT 1;
SET provsql.active = on;

-- A statement's timestamp and the start of its validity are stamped at
-- commit, not when the statement ran: CURRENT_TIMESTAMP is the
-- transaction's start, so two overlapping transactions could otherwise
-- commit in the opposite order of the validity they recorded.  The sleep
-- makes the difference measurable rather than a matter of microseconds.
BEGIN;
DELETE FROM tt_e WHERE id = 3;
CREATE TEMP TABLE tt_pre AS
  SELECT ts AS pre FROM update_provenance
   WHERE query LIKE 'DELETE FROM tt_e WHERE id = 3%';
SELECT pg_sleep(0.05);
COMMIT;
SET provsql.active = off;
SELECT (SELECT ts FROM update_provenance
         WHERE query LIKE 'DELETE FROM tt_e WHERE id = 3%')
       > (SELECT pre FROM tt_pre) AS stamped_at_commit;
DROP TABLE tt_pre;
SET provsql.active = on;

-- Giving a recorded modification a different probability mints a new
-- update gate, logs it beside the one it replaces, and rewrites every
-- tracked row whose provenance mentions the old one.
SELECT provsql AS del3 FROM update_provenance
 WHERE query LIKE 'DELETE FROM tt_e WHERE id = 3%' \gset
SELECT set_prob(:'del3'::uuid, 0.5);
SELECT replace_update(:'del3'::uuid, 0.25) AS new_update \gset
SET provsql.active = off;
SELECT get_prob(:'new_update'::uuid)  AS new_update_probability,
       get_prob(:'del3'::uuid)        AS old_update_probability;
SELECT query_type FROM update_provenance WHERE provsql = :'new_update'::uuid;
-- Row 3 still reads as deleted, now through the replacement gate.
SELECT id, v, sr_formula(provsql, 'tt_map') AS formula
  FROM tt_e WHERE id = 3 ORDER BY v;

DELETE FROM update_provenance;
SET provsql.update_provenance = off;
DROP TABLE tt_map;
DROP TABLE tt_e;
