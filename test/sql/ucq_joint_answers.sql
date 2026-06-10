\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql, public;

-- Per-answer (non-Boolean) joint-width evaluation: ucq_joint_answers()
-- pins each candidate head tuple with a Sel(h) constraint atom + a CERTAIN
-- fact and reuses the Boolean ucq_joint_evaluate(), one row per answer.
-- Cross-checked against ProvSQL's standard per-answer evaluation
-- (probability_evaluate over GROUP BY) where that terminates, and against
-- the closed form on the almost-safe shape whose per-answer lineage is the
-- QxW biclique the standard ladder cannot tree-decompose.

-- A helper: flatten a fact relation (rel id, element columns, token) of a
-- query into the columnar arrays, appended to running arrays.

-- ---------------------------------------------------------------------
-- (1) H0 per source: q(x) :- R(x), S(x,y), T(y).  Cross-check against the
--     standard per-x evaluation; the difference must be 0.
-- ---------------------------------------------------------------------
CREATE TABLE jr(x int);
CREATE TABLE js(x int, y int);
CREATE TABLE jt(y int);
INSERT INTO jr SELECT g FROM generate_series(1,4) g;
INSERT INTO js SELECT g, g*10+k FROM generate_series(1,4) g, generate_series(1,2) k;   -- private y per x
INSERT INTO jt SELECT g*10+k FROM generate_series(1,4) g, generate_series(1,2) k;
SELECT add_provenance('jr'); SELECT add_provenance('js'); SELECT add_provenance('jt');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.4) FROM jr;
  PERFORM set_prob(provsql, 0.4) FROM js;
  PERFORM set_prob(provsql, 0.4) FROM jt;
END $$;

SET provsql.provenance = 'boolean';
SET provsql.joint_width = off;
CREATE TEMP TABLE h0_std AS
  SELECT x, probability_evaluate(provenance()) AS p
    FROM (SELECT jr.x FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y) q
   GROUP BY x;

SET provsql.active = off;
CREATE TEMP TABLE h0_facts AS
  SELECT 0 rel, ARRAY[x] el, 1 ar, provsql t FROM jr
  UNION ALL SELECT 1, ARRAY[x,y], 2, provsql FROM js
  UNION ALL SELECT 2, ARRAY[y], 1, provsql FROM jt;
SET provsql.active = on;

CREATE TEMP TABLE h0_jw AS
SELECT a.head[1] AS x, a.probability AS p FROM ucq_joint_answers(
  '{"disjuncts":[{"n_vars":2,"atoms":[
     {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
  ARRAY[0],
  (SELECT array_agg(DISTINCT x ORDER BY x) FROM jr),
  (SELECT array_agg(rel ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM h0_facts) z),
  (SELECT array_agg(e ORDER BY rn, idx) FROM (SELECT row_number() OVER () rn, u.e, u.idx
     FROM h0_facts f, LATERAL unnest(f.el) WITH ORDINALITY u(e,idx)) s),
  (SELECT array_agg(ar ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM h0_facts) z),
  (SELECT array_agg(t ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM h0_facts) z),
  (SELECT array_agg(0.4::float8 ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM h0_facts) z)) a;

SET provsql.active = off;   -- report rows must not carry a provsql column
\echo '== H0 per source: x, joint-width probability, diff vs standard =='
SELECT j.x, ROUND(j.p::numeric, 6) AS joint_width,
       ROUND((s.p - j.p)::numeric, 9) AS diff_vs_standard
  FROM h0_jw j JOIN h0_std s ON s.x = j.x ORDER BY j.x;

-- ---------------------------------------------------------------------
-- (2) Almost-safe per source: q(x) :- P(x,y), Q(x,z), W(x,u), T(y), with
--     a couple of P-key violations.  Per x the lineage is the QxW
--     biclique; standard tree-decomposition cannot, joint-width can.
--     Closed form per x (p=0.5, K=4 z's and u's, private):
--       branch = 1-(1-0.5)^4 = 0.9375
--       normal x   : 0.5*0.5 * branch^2          = 0.219727
--       violation x: (1-(1-0.25)^2) * branch^2   = 0.384521
-- ---------------------------------------------------------------------
CREATE TABLE jp(x int, y int);
CREATE TABLE jq(x int, z int);
CREATE TABLE jw_(x int, u int);
CREATE TABLE jtt(y int);
INSERT INTO jp SELECT x, x FROM generate_series(1,4) x;
INSERT INTO jp VALUES (1,101);                                  -- one key violation
INSERT INTO jtt SELECT y FROM generate_series(1,4) y;
INSERT INTO jtt VALUES (101);
INSERT INTO jq SELECT x, x*1000+j FROM generate_series(1,4) x, generate_series(1,4) j;
INSERT INTO jw_ SELECT x, x*1000+500+j FROM generate_series(1,4) x, generate_series(1,4) j;
SELECT add_provenance('jp'); SELECT add_provenance('jq');
SELECT add_provenance('jw_'); SELECT add_provenance('jtt');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM jp;
  PERFORM set_prob(provsql, 0.5) FROM jq;
  PERFORM set_prob(provsql, 0.5) FROM jw_;
  PERFORM set_prob(provsql, 0.5) FROM jtt;
END $$;

SET provsql.active = off;
CREATE TEMP TABLE as_facts AS
  SELECT 0 rel, ARRAY[x,y] el, 2 ar, provsql t FROM jp
  UNION ALL SELECT 1, ARRAY[x,z], 2, provsql FROM jq
  UNION ALL SELECT 2, ARRAY[x,u], 2, provsql FROM jw_
  UNION ALL SELECT 3, ARRAY[y], 1, provsql FROM jtt;
SET provsql.active = on;

\echo '== almost-safe per source: x, joint-width probability =='
SELECT a.head[1] AS x, ROUND(a.probability::numeric, 6) AS joint_width
FROM ucq_joint_answers(
  '{"disjuncts":[{"n_vars":4,"atoms":[{"rel":0,"vars":[0,1]},
     {"rel":1,"vars":[0,2]},{"rel":2,"vars":[0,3]},{"rel":3,"vars":[1]}]}]}'::jsonb,
  ARRAY[0],
  (SELECT array_agg(DISTINCT x ORDER BY x) FROM jp),
  (SELECT array_agg(rel ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM as_facts) z),
  (SELECT array_agg(e ORDER BY rn, idx) FROM (SELECT row_number() OVER () rn, u.e, u.idx
     FROM as_facts f, LATERAL unnest(f.el) WITH ORDINALITY u(e,idx)) s),
  (SELECT array_agg(ar ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM as_facts) z),
  (SELECT array_agg(t ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM as_facts) z),
  (SELECT array_agg(0.5::float8 ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM as_facts) z)) a
ORDER BY x;

-- A non-answer head value (no witness) is dropped: pin x=99, expect no row.
\echo '== non-answer (x=99) is dropped: count must be 0 =='
SELECT count(*) AS rows FROM ucq_joint_answers(
  '{"disjuncts":[{"n_vars":4,"atoms":[{"rel":0,"vars":[0,1]},
     {"rel":1,"vars":[0,2]},{"rel":2,"vars":[0,3]},{"rel":3,"vars":[1]}]}]}'::jsonb,
  ARRAY[0], ARRAY[99],
  (SELECT array_agg(rel ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM as_facts) z),
  (SELECT array_agg(e ORDER BY rn, idx) FROM (SELECT row_number() OVER () rn, u.e, u.idx
     FROM as_facts f, LATERAL unnest(f.el) WITH ORDINALITY u(e,idx)) s),
  (SELECT array_agg(ar ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM as_facts) z),
  (SELECT array_agg(t ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM as_facts) z),
  (SELECT array_agg(0.5::float8 ORDER BY rn) FROM (SELECT *, row_number() OVER () rn FROM as_facts) z));

-- ---------------------------------------------------------------------
-- (3) Per-answer transparent primitive ucq_joint_provenance_answer():
--     gather the real relations filtered to the bound head value, then
--     materialise the head-pinned d-D; probability_evaluate of the token
--     is the answer's marginal.  Must match the closed form above
--     (violation x=1 -> 0.384521, normal x=2 -> 0.219727).
-- ---------------------------------------------------------------------
SET provsql.provenance = 'boolean';
\echo '== ucq_joint_provenance_answer: materialise + evaluate per answer =='
SELECT pin, ROUND(probability_evaluate(ucq_joint_provenance_answer(
  '{"disjuncts":[{"n_vars":4,"atoms":[{"rel":0,"vars":[0,1]},
     {"rel":1,"vars":[0,2]},{"rel":2,"vars":[0,3]},{"rel":3,"vars":[1]}]}],
    "relations":["provsql_test.jp","provsql_test.jq","provsql_test.jw_","provsql_test.jtt"],
    "elem_cols":[["x","y"],["x","z"],["x","u"],["y"]]}'::jsonb,
  ARRAY[0], ARRAY[pin::text]))::numeric, 6) AS joint_width
FROM (VALUES (1), (2)) AS v(pin) ORDER BY pin;

-- ---------------------------------------------------------------------
-- (4) Transparent per-answer rewrite: the NATURAL flat query
--     "SELECT head, probability_evaluate(provenance()) ... GROUP BY head"
--     over tracked base relations, under provsql.joint_width = on, must
--     substitute the head-pinned joint-width provenance per group.  On H0
--     it must agree with the standard per-answer evaluation (diff 0); on
--     the almost-safe shape it must reproduce the closed form (the values
--     of case (2)) -- there the per-answer lineage is the QxW biclique no
--     other method tree-decomposes.
-- ---------------------------------------------------------------------
SET provsql.provenance = 'boolean';
SET provsql.joint_width = on;
CREATE TEMP TABLE h0_tr AS
  SELECT jr.x AS x, probability_evaluate(provenance()) AS p
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y GROUP BY jr.x;
-- Capture the joint-width token (on) and the standard token (off) per
-- answer; the joint-width d-D is a different gate, so on <> off proves the
-- per-answer substitution fired (a regression that silently fell back to
-- the standard provenance would make them equal -- and pass a value-only
-- check, since the values agree on a small instance).
CREATE TEMP TABLE as_on AS
  SELECT jp.x AS x, provenance() AS tok FROM jp, jq, jw_, jtt
   WHERE jp.x = jq.x AND jp.x = jw_.x AND jp.y = jtt.y GROUP BY jp.x;
SET provsql.joint_width = off;
CREATE TEMP TABLE as_off AS
  SELECT jp.x AS x, provenance() AS tok FROM jp, jq, jw_, jtt
   WHERE jp.x = jq.x AND jp.x = jw_.x AND jp.y = jtt.y GROUP BY jp.x;
SET provsql.active = off;

\echo '== transparent H0 per source: diff vs standard (must be 0) =='
SELECT t.x, ROUND((t.p - s.p)::numeric, 9) AS diff_transparent_vs_standard
  FROM h0_tr t JOIN h0_std s ON t.x = s.x ORDER BY t.x;

\echo '== almost-safe: joint-width fired (token differs) and value = closed form =='
SELECT o.x, (o.tok <> f.tok) AS joint_width_fired,
       ROUND(probability_evaluate(o.tok)::numeric, 6) AS joint_width
  FROM as_on o JOIN as_off f ON o.x = f.x ORDER BY o.x;

-- ---------------------------------------------------------------------
-- (5) Multiple heads over a SELF-JOIN: the star 2-path q(x,z) :- A(x),
--     E(x,y), E(y,z), B(z) (E twice), GROUP BY x, z.  The head must be
--     forced by a Sel constraint on the variable, not by filtering the
--     E relation (which both atoms share); transparent must match the
--     standard per-(x,z) evaluation (diff 0 for every pair).
-- ---------------------------------------------------------------------
SET provsql.active = on;   -- re-enable tracking (section 4 left it off)
CREATE TABLE na_(x int); INSERT INTO na_ SELECT g FROM generate_series(1,3) g;
CREATE TABLE ne_(x int, y int);
INSERT INTO ne_ SELECT g, 0 FROM generate_series(1,3) g
  UNION ALL SELECT 0, g FROM generate_series(1,3) g;
CREATE TABLE nb_(z int); INSERT INTO nb_ SELECT g FROM generate_series(1,3) g;
SELECT add_provenance('na_'); SELECT add_provenance('ne_'); SELECT add_provenance('nb_');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM na_;
  PERFORM set_prob(provsql, 0.5) FROM ne_;
  PERFORM set_prob(provsql, 0.5) FROM nb_;
END $$;

SET provsql.provenance = 'boolean';
SET provsql.joint_width = on;
CREATE TEMP TABLE star_on AS
  SELECT na_.x AS x, nb_.z AS z, provenance() AS tok
    FROM na_, ne_ e1, ne_ e2, nb_
   WHERE na_.x = e1.x AND e1.y = e2.x AND e2.y = nb_.z GROUP BY na_.x, nb_.z;
SET provsql.joint_width = off;
CREATE TEMP TABLE star_off AS
  SELECT na_.x AS x, nb_.z AS z, provenance() AS tok
    FROM na_, ne_ e1, ne_ e2, nb_
   WHERE na_.x = e1.x AND e1.y = e2.x AND e2.y = nb_.z GROUP BY na_.x, nb_.z;
SET provsql.active = off;
\echo '== star 2 heads over a self-join: joint-width fired, diff vs standard 0 =='
SELECT o.x, o.z, (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9) AS diff
  FROM star_on o JOIN star_off f ON o.x = f.x AND o.z = f.z ORDER BY o.x, o.z;

-- ---------------------------------------------------------------------
-- (6) JOIN ... ON spelling: the recogniser flattens inner JoinExprs and
--     collects their ON conditions, so the JOIN-ON form of H0 is
--     recognised exactly like the comma/WHERE form -- joint-width fires
--     (token differs from the comma-form standard) and the value agrees.
-- ---------------------------------------------------------------------
SET provsql.active = on;
SET provsql.provenance = 'boolean';
SET provsql.joint_width = on;
CREATE TEMP TABLE h0_join_on AS
  SELECT jr.x AS x, provenance() AS tok
    FROM jr JOIN js ON jr.x = js.x JOIN jt ON js.y = jt.y GROUP BY jr.x;
SET provsql.joint_width = off;
CREATE TEMP TABLE h0_comma_off AS
  SELECT jr.x AS x, provenance() AS tok
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y GROUP BY jr.x;
SET provsql.active = off;
\echo '== JOIN..ON recognised: fired (token != comma standard), value equal =='
SELECT o.x, (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9) AS diff
  FROM h0_join_on o JOIN h0_comma_off f ON o.x = f.x ORDER BY o.x;

-- ---------------------------------------------------------------------
-- (7) Non-integer head column: a TEXT key.  The head value is bound by
--     casting the GROUP BY column to text (matching the gather's
--     (col)::text), so a text head works -- joint-width fires and agrees
--     with the standard per-answer evaluation.
-- ---------------------------------------------------------------------
SET provsql.active = on;
SET provsql.provenance = 'boolean';
CREATE TABLE tr_(nm text); INSERT INTO tr_ VALUES ('a'), ('b'), ('c');
CREATE TABLE ts_(nm text, y int);
INSERT INTO ts_ SELECT nm, ascii(nm)*10 + k
  FROM (VALUES ('a'), ('b'), ('c')) g(nm), generate_series(1,2) k;
CREATE TABLE tt2_(y int);
INSERT INTO tt2_ SELECT ascii(nm)*10 + k
  FROM (VALUES ('a'), ('b'), ('c')) g(nm), generate_series(1,2) k;
SELECT add_provenance('tr_'); SELECT add_provenance('ts_'); SELECT add_provenance('tt2_');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM tr_;
  PERFORM set_prob(provsql, 0.5) FROM ts_;
  PERFORM set_prob(provsql, 0.5) FROM tt2_;
END $$;
SET provsql.joint_width = on;
CREATE TEMP TABLE txt_on AS
  SELECT tr_.nm AS nm, provenance() AS tok FROM tr_, ts_, tt2_
   WHERE tr_.nm = ts_.nm AND ts_.y = tt2_.y GROUP BY tr_.nm;
SET provsql.joint_width = off;
CREATE TEMP TABLE txt_off AS
  SELECT tr_.nm AS nm, provenance() AS tok FROM tr_, ts_, tt2_
   WHERE tr_.nm = ts_.nm AND ts_.y = tt2_.y GROUP BY tr_.nm;
SET provsql.active = off;
\echo '== text head column: joint-width fired (token differs), value agrees =='
SELECT o.nm, (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9) AS diff
  FROM txt_on o JOIN txt_off f ON o.nm = f.nm ORDER BY o.nm;

DROP TABLE jr, js, jt, jp, jq, jw_, jtt, na_, ne_, nb_, tr_, ts_, tt2_ CASCADE;
