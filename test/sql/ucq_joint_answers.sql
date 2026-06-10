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

DROP TABLE jr, js, jt, jp, jq, jw_, jtt CASCADE;
