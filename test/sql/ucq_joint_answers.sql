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

-- ---------------------------------------------------------------------
-- (8) Subquery form: the UCQ is wrapped in a FROM-subquery and the
--     per-answer aggregation is on the outer.  The recogniser recurses
--     into the subquery and maps the outer GROUP BY column through the
--     subquery target list, so the plain and the DISTINCT-in-subquery
--     spellings of H0 both fire and agree with the comma/WHERE form.
-- ---------------------------------------------------------------------
SET provsql.active = on;
SET provsql.provenance = 'boolean';
SET provsql.joint_width = on;
CREATE TEMP TABLE sub_on AS
  SELECT x, provenance() AS tok
    FROM (SELECT jr.x FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y) s
   GROUP BY x;
CREATE TEMP TABLE subd_on AS
  SELECT x, provenance() AS tok
    FROM (SELECT DISTINCT jr.x FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y) s
   GROUP BY x;
SET provsql.joint_width = off;
CREATE TEMP TABLE sub_off AS
  SELECT jr.x AS x, provenance() AS tok
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y GROUP BY jr.x;
SET provsql.active = off;
\echo '== subquery (plain) fired and value equals comma form =='
SELECT o.x, (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9) AS diff
  FROM sub_on o JOIN sub_off f ON o.x = f.x ORDER BY o.x;
\echo '== subquery (DISTINCT inside) fired and value equals comma form =='
SELECT o.x, (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9) AS diff
  FROM subd_on o JOIN sub_off f ON o.x = f.x ORDER BY o.x;

-- ---------------------------------------------------------------------
-- (9) Free JOIN-variable as head, and bag projection.  q(y) :- R(x),
--     S(x,y), T(y) GROUP BY y exposes the join variable y as the head
--     (joint-width fires and agrees with standard).  A bag projection
--     SELECT x, y (no DISTINCT / GROUP BY) keeps one row per witness, each
--     a single monomial -- joint-width must NOT fire (no aggregation), so
--     the per-row token equals the standard provenance.
-- ---------------------------------------------------------------------
SET provsql.active = on;
SET provsql.provenance = 'boolean';
SET provsql.joint_width = on;
CREATE TEMP TABLE jy_on AS
  SELECT js.y AS y, provenance() AS tok
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y GROUP BY js.y;
SET provsql.joint_width = off;
CREATE TEMP TABLE jy_off AS
  SELECT js.y AS y, provenance() AS tok
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y GROUP BY js.y;
SET provsql.joint_width = on;
CREATE TEMP TABLE jbag_on AS
  SELECT jr.x AS x, js.y AS y, provenance() AS tok
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y;
SET provsql.joint_width = off;
CREATE TEMP TABLE jbag_off AS
  SELECT jr.x AS x, js.y AS y, provenance() AS tok
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y;
SET provsql.active = off;
\echo '== free JOIN-variable head q(y): fired, diff vs standard 0 =='
SELECT o.y, (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9) AS diff
  FROM jy_on o JOIN jy_off f ON o.y = f.y ORDER BY o.y;
\echo '== bag projection (no dup-elim): joint-width does NOT fire (token = standard) =='
SELECT o.x, o.y, (o.tok = f.tok) AS same_as_standard
  FROM jbag_on o JOIN jbag_off f ON o.x = f.x AND o.y = f.y ORDER BY o.x, o.y;

-- ---------------------------------------------------------------------
-- (10) Boolean UNION of conjunctive queries: a genuine multi-disjunct UCQ
--      Q :- R(x).  Q :- S(x,y), T(y).  written as a SQL UNION inside an
--      aggregated subquery.  The planner recogniser builds a two-disjunct
--      descriptor (relations merged across the arms) and the transparent
--      substitution evaluates the union with the joint-width compiler --
--      the "U" in UCQ exercised end to end.  Cross-checked against the
--      standard ladder on the same instance (diff 0), token differs from
--      the joint_width=off run (proves it fired).
-- ---------------------------------------------------------------------
SET provsql.active = on;
SET provsql.joint_width = on;
CREATE TEMP TABLE ju_on AS SELECT provenance() AS tok
  FROM (SELECT 1 d FROM jr UNION SELECT 1 FROM js, jt WHERE js.y = jt.y) q;
SET provsql.joint_width = off;
CREATE TEMP TABLE ju_off AS SELECT provenance() AS tok
  FROM (SELECT 1 d FROM jr UNION SELECT 1 FROM js, jt WHERE js.y = jt.y) q;
SET provsql.active = off;
\echo '== Boolean UNION (two disjuncts): joint-width fired, value = ladder =='
SELECT (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9)
         AS diff_vs_ladder
  FROM ju_on o, ju_off f;

-- ---------------------------------------------------------------------
-- (11) Per-answer UNION (free-variable head): Q(x) :- R(x).  Q(x) :-
--      S(x,y), T(y).  written as SELECT x, ... FROM (... UNION ...)
--      GROUP BY x.  Each arm exposes the head x; emit_cq_disjunct numbers
--      the head canonically (head -> variable 0 in every disjunct) so the
--      Sel-pin forces it across both disjuncts, and the substitution
--      evaluates the head-pinned UCQ per answer.  Cross-checked per x
--      against the standard ladder (diff 0), token differs (fired).
-- ---------------------------------------------------------------------
SET provsql.active = on;
SET provsql.joint_width = on;
CREATE TEMP TABLE jpu_on AS
  SELECT x, provenance() AS tok
    FROM (SELECT jr.x FROM jr UNION SELECT js.x FROM js, jt WHERE js.y = jt.y) q
   GROUP BY x;
SET provsql.joint_width = off;
CREATE TEMP TABLE jpu_off AS
  SELECT x, provenance() AS tok
    FROM (SELECT jr.x FROM jr UNION SELECT js.x FROM js, jt WHERE js.y = jt.y) q
   GROUP BY x;
SET provsql.active = off;
\echo '== per-answer UNION q(x): joint-width fired per answer, diff vs ladder 0 =='
SELECT o.x, (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9)
         AS diff_vs_ladder
  FROM jpu_on o JOIN jpu_off f ON o.x = f.x ORDER BY o.x;

-- ---------------------------------------------------------------------
-- (12) Constant selection: a Var = Const conjunct pins the column's
--      variable to the literal (the same Sel mechanism as a head, value
--      known at plan time), so the recogniser no longer declines on it.
--      H0 with the existential y pinned (js.y = 11): cross-check vs ladder.
-- ---------------------------------------------------------------------
SET provsql.active = on;
SET provsql.joint_width = on;
CREATE TEMP TABLE jc_on AS SELECT provenance() AS tok
  FROM (SELECT DISTINCT 1 FROM jr, js, jt
         WHERE jr.x = js.x AND js.y = jt.y AND js.y = 11) q;
SET provsql.joint_width = off;
CREATE TEMP TABLE jc_off AS SELECT provenance() AS tok
  FROM (SELECT DISTINCT 1 FROM jr, js, jt
         WHERE jr.x = js.x AND js.y = jt.y AND js.y = 11) q;
SET provsql.active = off;
\echo '== constant selection (js.y = 11): joint-width fired, diff vs ladder 0 =='
SELECT (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9)
         AS diff_vs_ladder
  FROM jc_on o, jc_off f;

-- ---------------------------------------------------------------------
-- (13) Arbitrary head expression: the per-answer partition is the GROUP BY
--      key, so what the SELECT list displays does not constrain
--      recognition.  Grouping by jr.x but outputting jr.x * 10 + 1 must
--      fire and give the SAME per-answer provenance as outputting jr.x.
-- ---------------------------------------------------------------------
SET provsql.active = on;
SET provsql.joint_width = on;
CREATE TEMP TABLE jhx_expr AS
  SELECT jr.x * 10 + 1 AS k, jr.x AS x, provenance() AS tok
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y GROUP BY jr.x;
CREATE TEMP TABLE jhx_bare AS
  SELECT jr.x AS x, provenance() AS tok
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y GROUP BY jr.x;
SET provsql.joint_width = off;
CREATE TEMP TABLE jhx_off AS
  SELECT jr.x AS x, provenance() AS tok
    FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y GROUP BY jr.x;
SET provsql.active = off;
\echo '== head expression r.x*10+1: fired, token == the bare-x head, diff vs ladder 0 =='
SELECT e.x, e.k, (e.tok <> o.tok) AS joint_width_fired,
       (e.tok = b.tok) AS same_token_as_bare_head,
       ROUND((probability_evaluate(e.tok) - probability_evaluate(o.tok))::numeric, 9)
         AS diff_vs_ladder
  FROM jhx_expr e JOIN jhx_bare b ON e.x = b.x JOIN jhx_off o ON e.x = o.x
 ORDER BY e.x;

-- ---------------------------------------------------------------------
-- (14) Single-relation selections as PRE-FILTERS: any predicate over one
--      relation (not just a Var=Const) is lifted by qc_split_quals,
--      deparsed, and pushed into that relation's gather scan -- the
--      relation/join/group structure is all the joint-width engine sees.
--      Inequality, IN, and a single-relation OR over the per-answer H0
--      (grouped by jr.x) all fire and agree with the ladder per answer.
--      (A self-join with disjoint constant filters yields two filtered
--      scans of the same relation; covered in the prefilter bench sweep.)
-- ---------------------------------------------------------------------
SET provsql.active = on;
CREATE OR REPLACE FUNCTION jw_prefilter_chk(label text, where_extra text)
  RETURNS TABLE(test text, all_fired bool, max_abs_diff numeric) AS $f$
DECLARE q text;
BEGIN
  q := 'SELECT jr.x AS x, provenance() AS t FROM jr, js, jt '
    || 'WHERE jr.x = js.x AND js.y = jt.y AND ' || where_extra || ' GROUP BY jr.x';
  SET provsql.active = on;
  SET provsql.joint_width = on;  EXECUTE 'CREATE TEMP TABLE pf_on AS '||q;
  SET provsql.joint_width = off; EXECUTE 'CREATE TEMP TABLE pf_off AS '||q;
  SET provsql.active = off;      -- the comparison below only reads tokens
  test := label;
  SELECT bool_and(o.t <> f.t),
         max(abs(round((probability_evaluate(o.t)-probability_evaluate(f.t))::numeric,9)))
    INTO all_fired, max_abs_diff
    FROM pf_on o JOIN pf_off f USING (x);
  DROP TABLE pf_on; DROP TABLE pf_off;
  RETURN NEXT;
END $f$ LANGUAGE plpgsql;
\echo '== single-relation pre-filters (inequality / IN / OR): every answer fired, diff 0 =='
SELECT * FROM jw_prefilter_chk('inequality jr.x > 2', 'jr.x > 2');
SELECT * FROM jw_prefilter_chk('IN jr.x IN (1,3)',    'jr.x IN (1,3)');
SELECT * FROM jw_prefilter_chk('OR jr.x=1 OR jr.x=4', '(jr.x = 1 OR jr.x = 4)');
DROP FUNCTION jw_prefilter_chk(text, text);
SET provsql.active = off;

-- ---------------------------------------------------------------------
-- (15) Boolean EXISTENCE of a multi-atom UCQ via DISTINCT: the #P-hard H0
--      = R(x), S(x,y), T(y) asked as "does any witness exist?".  DISTINCT
--      lowers to GROUP BY <const>, which is one group (the existence), not
--      a per-answer head -- the recogniser must read it as Boolean and
--      fire build_joint_width_provenance_expr.  Cross-check vs the ladder.
-- ---------------------------------------------------------------------
SET provsql.active = on;
SET provsql.joint_width = on;
CREATE TEMP TABLE he_on AS SELECT provenance() AS tok
  FROM (SELECT DISTINCT 1 FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y) q;
SET provsql.joint_width = off;
CREATE TEMP TABLE he_off AS SELECT provenance() AS tok
  FROM (SELECT DISTINCT 1 FROM jr, js, jt WHERE jr.x = js.x AND js.y = jt.y) q;
SET provsql.active = off;
\echo '== Boolean existence of H0 (3-atom, DISTINCT): fired, diff vs ladder 0 =='
SELECT (o.tok <> f.tok) AS joint_width_fired,
       ROUND((probability_evaluate(o.tok) - probability_evaluate(f.tok))::numeric, 9)
         AS diff_vs_ladder
  FROM he_on o, he_off f;

-- ---------------------------------------------------------------------
-- (16) The width screen (the cheap tw prepass): the joint compiler runs a
--      degeneracy lower bound on the joint graph before the expensive
--      decomposition + DP, and throws (-> the SQL layer falls back to the
--      ladder) when it exceeds provsql.joint_max_treewidth.  A 6-cycle has
--      joint treewidth 2: at the default bound it fires; with the bound set
--      to 1 the screen declines and the query is answered by the ladder --
--      same probability either way (the screen is a sound dispatch gate,
--      not an approximation).  [The provsql.joint_max_states cap is the
--      second net, during the DP; not toggled here.]
-- ---------------------------------------------------------------------
CREATE TABLE jcyc(x int, y int);
INSERT INTO jcyc VALUES (1,2),(2,3),(3,4),(4,5),(5,6),(6,1);
SELECT add_provenance('jcyc');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM jcyc; END $$;
SET provsql.active = on;
SET provsql.joint_width = on;
SET provsql.joint_max_treewidth = 10;
CREATE TEMP TABLE sc_hi AS SELECT provenance() AS tok FROM (SELECT DISTINCT 1 FROM jcyc) q;
SET provsql.joint_max_treewidth = 1;     -- below the joint treewidth (2): screen declines
CREATE TEMP TABLE sc_lo AS SELECT provenance() AS tok FROM (SELECT DISTINCT 1 FROM jcyc) q;
SET provsql.joint_max_treewidth = 10;
SET provsql.joint_width = off;
CREATE TEMP TABLE sc_off AS SELECT provenance() AS tok FROM (SELECT DISTINCT 1 FROM jcyc) q;
SET provsql.active = off;
\echo '== width screen: fires under the bound, declines (falls back) below it, same value =='
SELECT (hi.tok <> off.tok) AS fired_at_tw10,
       (lo.tok =  off.tok) AS declined_to_ladder_at_tw1,
       ROUND(probability_evaluate(hi.tok)::numeric, 6) AS p_joint,
       ROUND((probability_evaluate(lo.tok) - probability_evaluate(off.tok))::numeric, 9)
         AS diff_fallback_vs_ladder
  FROM sc_hi hi, sc_lo lo, sc_off off;

DROP TABLE jr, js, jt, jp, jq, jw_, jtt, na_, ne_, nb_, tr_, ts_, tt2_, jcyc CASCADE;
