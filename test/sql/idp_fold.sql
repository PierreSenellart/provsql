\set ECHO none
\pset format unaligned
SET client_min_messages = warning;

-- Folding and the cache of compiled d-DNNFs on circuits built gate by
-- gate.  Both are set to 'on' so that tree-decomposition, called by name,
-- goes through them: no chooser decides what runs.  The statistics count
-- the lookups of the cache, so they tell a compilation from a closed form.

CREATE FUNCTION fold_in(name text, p float8) RETURNS uuid
LANGUAGE plpgsql AS $$
DECLARE u uuid := md5('idp_fold:' || name)::uuid;
BEGIN
  PERFORM create_gate(u, 'input');
  PERFORM set_prob(u, p);
  RETURN u;
END $$;
CREATE FUNCTION fold_gate(name text, t provenance_gate, VARIADIC children uuid[])
RETURNS uuid LANGUAGE plpgsql AS $$
DECLARE u uuid := md5('idp_fold:' || name)::uuid;
BEGIN
  PERFORM create_gate(u, t, children);
  RETURN u;
END $$;
CREATE FUNCTION fold_token(name text) RETURNS uuid
LANGUAGE sql AS $$ SELECT md5('idp_fold:' || name)::uuid $$;
CREATE FUNCTION fold_not(name text, child uuid) RETURNS uuid
LANGUAGE sql AS $$ SELECT fold_gate(name, 'monus', gate_one(), child) $$;
CREATE FUNCTION fold_lookups() RETURNS text LANGUAGE sql AS $$
  SELECT format('%s hits, %s misses',
                prob_cache_stats()->>'hits', prob_cache_stats()->>'misses') $$;
CREATE FUNCTION fold_td(token uuid) RETURNS float8 LANGUAGE sql AS $$
  SELECT probability_evaluate(token, 'tree-decomposition') $$;

SET provsql.prob_cache = on;
SET provsql.idp_fold = on;

-- The cache alone, on (a AND b) OR (a AND c), of probability
-- P(a) (P(b) + P(c) - P(b) P(c)): compiled once, then served for the same
-- shape over other inputs and other probabilities.
SET provsql.idp_fold = off;
SELECT prob_cache_reset();
SELECT round(fold_td(fold_gate('t1', 'plus',
         fold_gate('t1_1', 'times', fold_in('t1_a', 0.5), fold_in('t1_b', 0.4)),
         fold_gate('t1_2', 'times', fold_in('t1_a', 0.5), fold_in('t1_c', 0.3))))
       ::numeric, 12) AS compiled,
       round((0.5 * (0.4 + 0.3 - 0.4 * 0.3))::numeric, 12) AS expected,
       fold_lookups();
SELECT round(fold_td(fold_gate('t2', 'plus',
         fold_gate('t2_1', 'times', fold_in('t2_a', 0.9), fold_in('t2_b', 0.2)),
         fold_gate('t2_2', 'times', fold_in('t2_a', 0.9), fold_in('t2_c', 0.7))))
       ::numeric, 12) AS served,
       round((0.9 * (0.2 + 0.7 - 0.2 * 0.7))::numeric, 12) AS expected,
       fold_lookups();

-- (u AND v) OR NOT w, the conjunction being an island: 1 - (1 - P(u) P(v)) P(w).
SELECT round(fold_td(fold_gate('t3', 'plus',
         fold_gate('t3_1', 'times', fold_in('t3_u', 0.25), fold_in('t3_v', 0.5)),
         fold_not('t3_n', fold_in('t3_w', 0.5))))
       ::numeric, 12) AS compiled,
       round((1 - (1 - 0.125) * 0.5)::numeric, 12) AS expected,
       fold_lookups();
SELECT round(fold_td(fold_gate('t4', 'plus',
         fold_gate('t4_1', 'times', fold_in('t4_u', 0.6), fold_in('t4_v', 0.8)),
         fold_not('t4_n', fold_in('t4_w', 0.1))))
       ::numeric, 12) AS served,
       round((1 - (1 - 0.48) * 0.1)::numeric, 12) AS expected,
       fold_lookups();

-- The residual of (x AND y) OR (z AND w) is the form of u OR v: once the
-- latter is compiled, the former compiles one island, the second island
-- and the residual being served.
SELECT prob_cache_reset();
SELECT round(fold_td(fold_gate('m1', 'plus',
         fold_in('m1_u', 0.3), fold_in('m1_v', 0.6)))::numeric, 12) AS disjunction,
       round((1 - 0.7 * 0.4)::numeric, 12) AS expected, fold_lookups();
SELECT round(fold_td(fold_gate('m2', 'plus',
         fold_gate('m2_1', 'times', fold_in('m2_x', 0.5), fold_in('m2_y', 0.4)),
         fold_gate('m2_2', 'times', fold_in('m2_z', 0.9), fold_in('m2_w', 0.2))))
       ::numeric, 12) AS of_conjunctions,
       round((1 - (1 - 0.2) * (1 - 0.18))::numeric, 12) AS expected,
       fold_lookups();
SET provsql.idp_fold = on;

-- A disjunction of 5000 independent unlikely inputs, and a conjunction of
-- 5000 independent likely ones (so that neither value is 0 or 1 to machine
-- precision): a closed form, with no compilation and no lookup.
DO $$
DECLARE rare uuid[] := '{}'; likely uuid[] := '{}';
BEGIN
  FOR i IN 0..4999 LOOP
    rare := rare || fold_in('rare' || i, 0.0001 * (1 + i % 7));
    likely := likely || fold_in('likely' || i, 1 - 0.0001 * (1 + i % 7));
  END LOOP;
  PERFORM fold_gate('flat_or', 'plus', VARIADIC rare);
  PERFORM fold_gate('flat_and', 'times', VARIADIC likely);
END $$;
SELECT prob_cache_reset();
SET provsql.last_eval_method = '';
SELECT round(fold_td(fold_token('flat_or'))::numeric, 9) AS flat_or,
       round((1 - exp(sum(ln(1 - 0.0001::float8 * (1 + i % 7)))))::numeric, 9)
         AS expected,
       round(fold_td(fold_token('flat_and'))::numeric, 9) AS flat_and,
       round(exp(sum(ln(1 - 0.0001::float8 * (1 + i % 7))))::numeric, 9)
         AS expected
FROM generate_series(0, 4999) i;
SELECT current_setting('provsql.last_eval_method') AS method, fold_lookups();

-- Gates without children, and a child given twice.
SELECT fold_td(fold_gate('empty_and', 'times', VARIADIC '{}'::uuid[]))
         AS empty_and,
       fold_td(fold_gate('empty_or', 'plus', VARIADIC '{}'::uuid[]))
         AS empty_or;
SELECT round(fold_td(fold_gate('dup', 'plus', fold_in('dup_x', 0.3),
         fold_in('dup_x', 0.3), fold_in('dup_y', 0.4)))::numeric, 12) AS twice,
       round((1 - 0.7 * 0.6)::numeric, 12) AS expected, fold_lookups();

-- Independent disjunctions under a conjunction: a closed form at each level.
SELECT round(fold_td(fold_gate('nest', 'times',
         fold_gate('nest_l', 'plus', fold_in('nest_a', 0.3), fold_in('nest_b', 0.4)),
         fold_gate('nest_r', 'plus', fold_in('nest_d', 0.5), fold_in('nest_e', 0.6))))
       ::numeric, 12) AS nested,
       round(((1 - 0.7 * 0.6) * (1 - 0.5 * 0.4))::numeric, 12) AS expected,
       fold_lookups();

-- Three orders with their lines, order o AND line l for each line: the
-- orders are independent groups.  The two orders of two lines have one
-- shape, compiled once and served once; the order of one line is a product.
CREATE TABLE fold_orders AS SELECT fold_gate('orders', 'plus',
  fold_gate('o0_1', 'times', fold_in('o0', 0.5), fold_in('o0_l1', 0.2)),
  fold_gate('o0_2', 'times', fold_in('o0', 0.5), fold_in('o0_l2', 0.3)),
  fold_gate('o1_1', 'times', fold_in('o1', 0.6), fold_in('o1_l1', 0.7)),
  fold_gate('o2_1', 'times', fold_in('o2', 0.8), fold_in('o2_l1', 0.9)),
  fold_gate('o2_2', 'times', fold_in('o2', 0.8), fold_in('o2_l2', 0.4))) AS token;
SELECT round(probability_evaluate(token, 'possible-worlds')::numeric, 12)
         AS orders_possible_worlds
FROM fold_orders;
SELECT prob_cache_reset();
SET provsql.last_eval_method = '';
SELECT round(fold_td(token)::numeric, 12) AS orders,
       round((1 - (1 - 0.5 * (1 - 0.8 * 0.7)) * (1 - 0.6 * 0.7)
                * (1 - 0.8 * (1 - 0.1 * 0.6)))::numeric, 12) AS expected,
       fold_lookups()
FROM fold_orders;
SELECT current_setting('provsql.last_eval_method') AS method;
-- Folding without the cache: each group is compiled, nothing is looked up.
SET provsql.prob_cache = off;
SELECT prob_cache_reset();
SELECT round(fold_td(token)::numeric, 12) AS orders_uncached, fold_lookups()
FROM fold_orders;
SET provsql.prob_cache = on;
DROP TABLE fold_orders;

-- A chain v0 v1 + v1 v2 + v2 v3 is one group: nothing to fold, the whole
-- circuit is compiled.
CREATE TABLE fold_chain AS SELECT fold_gate('chain', 'plus',
  fold_gate('chain_1', 'times', fold_in('chain0', 0.2), fold_in('chain1', 0.4)),
  fold_gate('chain_2', 'times', fold_in('chain1', 0.4), fold_in('chain2', 0.6)),
  fold_gate('chain_3', 'times', fold_in('chain2', 0.6), fold_in('chain3', 0.8)))
  AS token;
SELECT round(probability_evaluate(token, 'possible-worlds')::numeric, 12)
         AS chain_possible_worlds
FROM fold_chain;
SELECT prob_cache_reset();
SET provsql.last_eval_method = '';
SELECT round(fold_td(token)::numeric, 12) AS chain, fold_lookups()
FROM fold_chain;
SELECT current_setting('provsql.last_eval_method') AS method;
DROP TABLE fold_chain;

-- A conjunction nested 2000 deep, each level adding an independent input.
DO $$
DECLARE prev uuid := fold_in('deep0', 0.9);
BEGIN
  FOR i IN 1..1999 LOOP
    prev := fold_gate('deep_and' || i, 'times', fold_in('deep' || i, 0.999), prev);
  END LOOP;
END $$;
SELECT abs(fold_td(fold_token('deep_and1999'))
           - 0.9 * power(0.999::float8, 1999)) < 1e-9 AS deep_exact;

-- 300 circuits drawn from a fixed sequence, of at most 9 inputs, against
-- the enumeration of their possible worlds: folded with the cache, folded
-- without, and unfolded with the cache.
CREATE FUNCTION fold_rand(INOUT state bigint, bound int, OUT value int)
LANGUAGE sql AS $$
  SELECT (state * 1103515245 + 12345) % 2147483648,
         (((state * 1103515245 + 12345) % 2147483648) / 65536 % bound)::int
$$;
CREATE TABLE fold_random(round int, token uuid);
DO $$
DECLARE
  state bigint := 20261102; v int; kind int; fanin int;
  nodes uuid[]; kids uuid[]; g uuid;
BEGIN
  FOR round IN 1..300 LOOP
    nodes := '{}';
    SELECT * INTO state, v FROM fold_rand(state, 8);
    FOR i IN 1..2 + v LOOP
      SELECT * INTO state, v FROM fold_rand(state, 100);
      nodes := nodes || fold_in('r' || round || '_' || i, 0.1 + 0.8 * v / 100.);
    END LOOP;
    SELECT * INTO state, v FROM fold_rand(state, 8);
    FOR i IN 1..1 + v LOOP
      SELECT * INTO state, kind FROM fold_rand(state, 5);
      IF kind = 0 THEN fanin := 1;
      ELSE SELECT * INTO state, v FROM fold_rand(state, 4); fanin := 1 + v;
      END IF;
      kids := '{}';
      FOR k IN 1..fanin LOOP
        SELECT * INTO state, v FROM fold_rand(state, cardinality(nodes));
        kids := kids || nodes[1 + v];
      END LOOP;
      nodes := nodes || CASE
        WHEN kind = 0 THEN fold_not('r' || round || '_g' || i, kids[1])
        ELSE fold_gate('r' || round || '_g' || i,
               CASE WHEN kind % 2 = 1 THEN 'times' ELSE 'plus' END::provenance_gate,
               VARIADIC kids) END;
    END LOOP;
    kids := ARRAY[nodes[cardinality(nodes)]];
    FOR k IN 1..cardinality(nodes) - 1 LOOP
      SELECT * INTO state, v FROM fold_rand(state, 2);
      IF v = 1 THEN kids := kids || nodes[k]; END IF;
    END LOOP;
    SELECT * INTO state, v FROM fold_rand(state, 2);
    g := fold_gate('r' || round || '_root',
           CASE WHEN v = 1 THEN 'times' ELSE 'plus' END::provenance_gate,
           VARIADIC kids);
    INSERT INTO fold_random VALUES (round, g);
  END LOOP;
END $$;

CREATE TABLE fold_reference AS
  SELECT round, token, probability_evaluate(token, 'possible-worlds') AS p
  FROM fold_random;
SELECT prob_cache_reset();
SET provsql.last_eval_method = '';
SELECT count(*) AS circuits,
       count(*) FILTER (WHERE abs(fold_td(token) - p) > 1e-9) AS wrong_folded
FROM fold_reference;
SELECT current_setting('provsql.last_eval_method') LIKE '%fold%' AS folds,
       (prob_cache_stats()->>'hits')::int > 0 AS hits;
SET provsql.prob_cache = off;
SELECT count(*) FILTER (WHERE abs(fold_td(token) - p) > 1e-9)
         AS wrong_folded_uncached
FROM fold_reference;
SET provsql.prob_cache = on;
SET provsql.idp_fold = off;
SELECT prob_cache_reset();
SELECT count(*) FILTER (WHERE abs(fold_td(token) - p) > 1e-9)
         AS wrong_unfolded_cached
FROM fold_reference;
SELECT (prob_cache_stats()->>'hits')::int > 0 AS hits;

RESET provsql.prob_cache;
RESET provsql.idp_fold;
SELECT prob_cache_reset();
DROP TABLE fold_reference;
DROP TABLE fold_random;
DROP FUNCTION fold_rand(bigint, int);
DROP FUNCTION fold_td(uuid);
DROP FUNCTION fold_lookups();
DROP FUNCTION fold_not(text, uuid);
DROP FUNCTION fold_token(text);
DROP FUNCTION fold_gate(text, provenance_gate, uuid[]);
DROP FUNCTION fold_in(text, float8);
