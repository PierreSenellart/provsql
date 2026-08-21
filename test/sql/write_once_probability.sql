\set ECHO none
\pset format unaligned

-- A probability is a fact appended to the circuit, like the gate it
-- belongs to: written once, re-writable with the identical value, and
-- otherwise refused.  A write a transaction rolls back is cleared, so the
-- circuit is left as the transaction found it.
--
-- The assertions run with provsql.active off, so that no token appears in
-- the output: the values are what matters, and the UUIDs differ run to
-- run.

CREATE TABLE wo_t (name text);
INSERT INTO wo_t VALUES ('alice'), ('bob');
SELECT add_provenance('wo_t');

-- Nobody has written a probability yet: the rows' gates are not even in
-- the circuit, since no query has needed them.
SET provsql.active = off;
SELECT bool_and(NOT probability_is_set(provsql)) AS unset_at_first,
       bool_and(get_prob(provsql) IS NULL) AS not_in_the_circuit_yet
  FROM wo_t;
SET provsql.active = on;

DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM wo_t; END $$;
-- Re-running the same assignment is a no-op, so setup scripts and
-- notebook cells stay re-runnable.
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM wo_t; END $$;
SET provsql.active = off;
SELECT bool_and(probability_is_set(provsql)) AS set_now,
       bool_and(get_prob(provsql) = 0.5) AS reads_back
  FROM wo_t;
SET provsql.active = on;

-- A different value is refused, and says what to do instead.
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.25) FROM wo_t;
  RAISE NOTICE 'rewriting a probability was accepted';
EXCEPTION WHEN others THEN
  RAISE NOTICE 'refused: %', regexp_replace(SQLERRM, '[0-9a-f-]{36}', '<token>');
END $$;

-- Out-of-range values and NaN are refused too.
DO $$ BEGIN
  PERFORM set_prob(public.uuid_generate_v4(), 1.5);
EXCEPTION WHEN others THEN RAISE NOTICE 'refused: %', SQLERRM;
END $$;
DO $$ BEGIN
  PERFORM set_prob(public.uuid_generate_v4(), 'NaN'::double precision);
EXCEPTION WHEN others THEN RAISE NOTICE 'refused: %', SQLERRM;
END $$;

-- A rolled-back write leaves the gate as it was.
CREATE TABLE wo_u (name text);
INSERT INTO wo_u VALUES ('carol');
SELECT add_provenance('wo_u');
BEGIN;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.25) FROM wo_u; END $$;
SET provsql.active = off;
SELECT bool_and(probability_is_set(provsql)) AS set_inside_txn FROM wo_u;
SET provsql.active = on;
ROLLBACK;
SET provsql.active = off;
SELECT bool_and(probability_is_set(provsql)) AS set_after_rollback FROM wo_u;
SET provsql.active = on;

-- ... and the gate is writable again, with a different value.
DO $$ BEGIN PERFORM set_prob(provenance(), 0.75) FROM wo_u; END $$;
SET provsql.active = off;
SELECT bool_and(get_prob(provsql) = 0.75) AS written_after_rollback FROM wo_u;
SET provsql.active = on;

-- Same at savepoint granularity, on a gate nobody has written yet.
CREATE TABLE wo_v (name text);
INSERT INTO wo_v VALUES ('dave');
SELECT add_provenance('wo_v');
BEGIN;
SAVEPOINT sp;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.9) FROM wo_v; END $$;
ROLLBACK TO SAVEPOINT sp;
SET provsql.active = off;
SELECT bool_and(NOT probability_is_set(provsql)) AS savepoint_rollback_cleared
  FROM wo_v;
SET provsql.active = on;
COMMIT;

-- Changing a probability: replace the row's input gate.  The table stays
-- TID -- a replacement leaf is an independent fresh leaf -- and a mapping
-- built from the table follows the token to its replacement.
SELECT create_provenance_mapping('wo_map', 'wo_t', 'name');
UPDATE wo_t SET provsql = provsql.replace_input(provsql, 0.25);
SELECT (get_table_info('wo_t'::regclass::oid)).kind AS kind_after_replace;
SET provsql.active = off;
SELECT bool_and(get_prob(provsql) = 0.25) AS replaced FROM wo_t;
SELECT name, sr_formula(provsql, 'wo_map') AS formula FROM wo_t ORDER BY name;
SET provsql.active = on;

-- The procedural form does the UPDATE itself, which is what a client
-- holding only the token can use.
SELECT provsql.replace_input('wo_u'::regclass,
                             (SELECT provsql FROM wo_u), 0.6) IS NOT NULL
       AS replaced_by_relation;
SET provsql.active = off;
SELECT bool_and(get_prob(provsql) = 0.6) AS replaced_in_place FROM wo_u;
SET provsql.active = on;

-- A repair_key block's values share a key gate and their masses are
-- meaningful together, so they are replaced together.
CREATE TABLE wo_bid (k text, v text);
INSERT INTO wo_bid VALUES ('a', 'x'), ('a', 'y'), ('b', 'z');
SELECT repair_key('wo_bid', 'k');
-- Untouched, a repaired row evaluates at the uniform weight of its block.
SET provsql.active = off;
SELECT k, count(*) AS n, round(max(get_prob(provsql))::numeric, 4) AS p
  FROM wo_bid GROUP BY k ORDER BY k;
SELECT provsql AS wo_key FROM wo_bid WHERE k = 'a' LIMIT 1 \gset
SELECT (get_children(:'wo_key'::uuid))[1] AS wo_block \gset
SET provsql.active = on;
SELECT replace_block('wo_bid', :'wo_block'::uuid, ARRAY[0.3, 0.6]);
SET provsql.active = off;
SELECT k, round(sum(get_prob(provsql))::numeric, 4) AS mass
  FROM wo_bid GROUP BY k ORDER BY k;
SELECT (get_table_info('wo_bid'::regclass::oid)).kind AS kind_after_replace_block;
SET provsql.active = on;

-- A gate's annotations are written once too: writing what the gate holds
-- is a no-op, writing something else is refused.  The two info fields go
-- once each, 0 meaning "nothing recorded", because a certified gate is
-- marked as certified when it is built and tagged with the route that
-- made it a query root afterwards.
DO $$
DECLARE t uuid := public.uuid_generate_v4();
BEGIN
  PERFORM create_gate(t, 'input');
  PERFORM set_infos(t, 1, 0);
  PERFORM set_infos(t, 0, 2);   -- the other field, later: allowed
  PERFORM set_infos(t, 1, 2);   -- exactly what it holds: a no-op
  RAISE NOTICE 'infos now (%, %)', (get_infos(t)).info1, (get_infos(t)).info2;
  BEGIN
    PERFORM set_infos(t, 1, 3);
    RAISE NOTICE 're-annotating was accepted';
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'refused: %', regexp_replace(SQLERRM, '[0-9a-f-]{36}', '<token>');
  END;
  PERFORM set_extra(t, 'first');
  PERFORM set_extra(t, 'first');
  BEGIN
    PERFORM set_extra(t, 'second');
    RAISE NOTICE 're-annotating was accepted';
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'refused: %', regexp_replace(SQLERRM, '[0-9a-f-]{36}', '<token>');
  END;
END $$;

-- replace_input refuses what it is not for, and points at what is.
DO $$ BEGIN
  PERFORM provsql.replace_input(provsql.gate_one(), 0.5);
EXCEPTION WHEN others THEN
  RAISE NOTICE 'refused: %', regexp_replace(SQLERRM, '[0-9a-f-]{36}', '<token>');
END $$;

DROP TABLE wo_map;
DROP TABLE wo_t;
DROP TABLE wo_u;
DROP TABLE wo_v;
DROP TABLE wo_bid;
