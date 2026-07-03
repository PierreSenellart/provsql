\set ECHO none
\pset format unaligned

-- =========================================================================
-- NULL semantics battery: SQL-conformant behavior of the rewriter on
-- instances containing NULLs, and possible-worlds-correct Boolean
-- provenance for the negative fragment (the [GL17] Q1/Q2/Q3 example and
-- its relatives).  The normative semantics: predicates over NULLs follow
-- SQL's 3VL on the actual data and a condition evaluating to unknown
-- annotates the tuple with the semiring zero; set operations match
-- tuples syntactically (NULL-identical); for Boolean provenance, a tuple
-- must satisfy "t ∈ Q(W) under SQL semantics iff the valuation of W
-- satisfies the tuple's provenance" in every world W.
--
-- Zero-annotated rows may remain visible (a row with annotation 0 is
-- equivalent to an absent row); the probability readouts below make the
-- semantics observable either way.
-- =========================================================================

-- Determinism for the Monte Carlo calls in the random_variable section.
SET provsql.monte_carlo_seed = 42;

-- Fixtures (all probabilities 0.5):
--   ns_r(a,b)  = {(1,10),(2,NULL),(3,30),(3,30),(NULL,40),(NULL,NULL)}  r1..r6
--   ns_gr(a)   = {1, NULL}                                              gr1,gr2
--   ns_gs(a)   = {NULL}                                                 gs1
--   ns_s(a)    = {1, NULL}                                              s1,s2
CREATE TABLE ns_r(a int, b int, name text);
INSERT INTO ns_r VALUES
  (1,10,'r1'),(2,NULL,'r2'),(3,30,'r3'),(3,30,'r4'),
  (NULL,40,'r5'),(NULL,NULL,'r6');
CREATE TABLE ns_gr(a int, name text);
INSERT INTO ns_gr VALUES (1,'gr1'),(NULL,'gr2');
CREATE TABLE ns_gs(a int, name text);
INSERT INTO ns_gs VALUES (NULL,'gs1');
CREATE TABLE ns_s(a int, name text);
INSERT INTO ns_s VALUES (1,'s1'),(NULL,'s2');

SELECT add_provenance('ns_r');
SELECT add_provenance('ns_gr');
SELECT add_provenance('ns_gs');
SELECT add_provenance('ns_s');
SELECT create_provenance_mapping('ns_r_name','ns_r','name');
SELECT create_provenance_mapping('ns_gr_name','ns_gr','name');
SELECT create_provenance_mapping('ns_gs_name','ns_gs','name');
SELECT create_provenance_mapping('ns_s_name','ns_s','name');
-- Merged mapping: circuits below mix leaves from several fixtures.
CREATE TABLE ns_map AS
  SELECT * FROM ns_r_name UNION ALL SELECT * FROM ns_gr_name
  UNION ALL SELECT * FROM ns_gs_name UNION ALL SELECT * FROM ns_s_name;
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ns_r;
  PERFORM set_prob(provsql, 0.5) FROM ns_gr;
  PERFORM set_prob(provsql, 0.5) FROM ns_gs;
  PERFORM set_prob(provsql, 0.5) FROM ns_s;
END $$;

-- -------------------------------------------------------------------------
-- 1. WHERE 3VL delegation: PostgreSQL filters NULL comparisons; only the
--    surviving rows' tokens appear.
-- -------------------------------------------------------------------------
CREATE TABLE ns_t1 AS
  SELECT a, b, sr_formula(provenance(),'ns_map') AS f
  FROM ns_r WHERE b > 5;
SELECT remove_provenance('ns_t1');
SELECT * FROM ns_t1 ORDER BY a NULLS LAST, b;
DROP TABLE ns_t1;

CREATE TABLE ns_t2 AS
  SELECT a, sr_formula(provenance(),'ns_map') AS f
  FROM ns_r WHERE b IS NULL;
SELECT remove_provenance('ns_t2');
SELECT * FROM ns_t2 ORDER BY a NULLS LAST;
DROP TABLE ns_t2;

-- -------------------------------------------------------------------------
-- 2. The [GL17] difference trio over gr = {1, NULL}, gs = {NULL}.
--    Vanilla SQL: Q1 = ∅, Q2 = {1, NULL}, Q3 = {1}.  The three queries
--    must NOT get the same provenance.
-- -------------------------------------------------------------------------

-- Q1: NOT IN.  1 NOT IN {NULL} is unknown, so in a world where gs1 is
-- present no gr row is an answer: correct Boolean provenance gri ∧ ¬gs1,
-- probability 0.25 (rows absent or 0-annotated on the actual instance).
CREATE TABLE ns_q1 AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_gr WHERE a NOT IN (SELECT a FROM ns_gs);
SELECT remove_provenance('ns_q1');
SELECT * FROM ns_q1 ORDER BY a NULLS LAST;
DROP TABLE ns_q1;

-- Q1 with a constant left operand: 1 NOT IN {NULL} is unknown whenever
-- the subquery row is present, and the constant side needs no NULL
-- guard: each row's probability is P(row ∧ ¬gs1) = 0.25.
CREATE TABLE ns_q1c AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_gr WHERE 1 NOT IN (SELECT a FROM ns_gs);
SELECT remove_provenance('ns_q1c');
SELECT * FROM ns_q1c ORDER BY a NULLS LAST;
DROP TABLE ns_q1c;

-- Columns declared NOT NULL on both sides: the planner proves the
-- guards unnecessary and the lift keeps its unguarded form; classic
-- antijoin probabilities (row 1 never matches: 0.5; row 2 is removed
-- by ns1: 0.25).
CREATE TABLE ns_nnr(a int NOT NULL, name text);
INSERT INTO ns_nnr VALUES (1,'nr1'),(2,'nr2');
CREATE TABLE ns_nns(a int NOT NULL, name text);
INSERT INTO ns_nns VALUES (2,'ns1');
SELECT add_provenance('ns_nnr');
SELECT add_provenance('ns_nns');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ns_nnr;
  PERFORM set_prob(provsql, 0.5) FROM ns_nns;
END $$;
CREATE TABLE ns_nn AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_nnr WHERE a NOT IN (SELECT a FROM ns_nns);
SELECT remove_provenance('ns_nn');
SELECT * FROM ns_nn ORDER BY a;
DROP TABLE ns_nn;
DROP TABLE ns_nnr;
DROP TABLE ns_nns;

-- Q2: NOT EXISTS.  gs.a = x is never true for gs1's NULL, so both rows
-- are answers in every world containing them: probability 0.5.
CREATE TABLE ns_q2 AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_gr WHERE NOT EXISTS (SELECT * FROM ns_gs WHERE ns_gs.a = ns_gr.a);
SELECT remove_provenance('ns_q2');
SELECT * FROM ns_q2 ORDER BY a NULLS LAST;
DROP TABLE ns_q2;

-- Q3: EXCEPT.  Set difference matches syntactically: the NULL row is
-- removed by gs1 (probability gr2 ∧ ¬gs1 = 0.25), the 1 row never
-- matches gs1 (probability 0.5).
CREATE TABLE ns_q3 AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM (SELECT a FROM ns_gr EXCEPT SELECT a FROM ns_gs) q;
SELECT remove_provenance('ns_q3');
SELECT * FROM ns_q3 ORDER BY a NULLS LAST;
DROP TABLE ns_q3;

-- -------------------------------------------------------------------------
-- 3. EXCEPT ALL, against ProvSQL's documented bag difference (every
--    matching left copy removed; matching is syntactic).  On the actual
--    instance the surviving multiset is {2, 3, 3}: the 1 row is removed
--    by gr1, both NULL rows by gr2.
-- -------------------------------------------------------------------------
CREATE TABLE ns_ea AS
  SELECT a, sr_formula(provenance(),'ns_map') AS f,
         round(probability_evaluate(provenance())::numeric,4) AS p
  FROM (SELECT a FROM ns_r EXCEPT ALL SELECT a FROM ns_gr) q;
SELECT remove_provenance('ns_ea');
SELECT * FROM ns_ea ORDER BY a NULLS LAST, f;
DROP TABLE ns_ea;

-- -------------------------------------------------------------------------
-- 4. IN with an unmatched / NULL subquery: rows are not answers (unknown
--    is filtered like false); 0-annotated rows may remain visible.
-- -------------------------------------------------------------------------
CREATE TABLE ns_in AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_gr WHERE a IN (SELECT a FROM ns_gs);
SELECT remove_provenance('ns_in');
SELECT * FROM ns_in ORDER BY a NULLS LAST;
DROP TABLE ns_in;

-- -------------------------------------------------------------------------
-- 5. Grouping and duplicate elimination treat NULLs as identical
--    (SQL's syntactic equality): NULL keys collapse into one group whose
--    annotation is the ⊕ of all contributing tokens.
-- -------------------------------------------------------------------------
CREATE TABLE ns_d AS
  SELECT a, sr_formula(provenance(),'ns_map') AS f
  FROM (SELECT DISTINCT a FROM ns_r) q;
SELECT remove_provenance('ns_d');
SELECT a, replace(replace(f,'r4 ⊕ r3','r3 ⊕ r4'),'r6 ⊕ r5','r5 ⊕ r6') AS f
FROM ns_d ORDER BY a NULLS LAST;
DROP TABLE ns_d;

CREATE TABLE ns_u AS
  SELECT a, sr_formula(provenance(),'ns_map') AS f
  FROM (SELECT a FROM ns_gr UNION SELECT a FROM ns_gs) q;
SELECT remove_provenance('ns_u');
SELECT a, replace(f,'gs1 ⊕ gr2','gr2 ⊕ gs1') AS f FROM ns_u ORDER BY a NULLS LAST;
DROP TABLE ns_u;

-- -------------------------------------------------------------------------
-- 6. Aggregates skip NULL inputs; count(*) does not; sum/avg over an
--    all-NULL group is SQL NULL (and so is its formula readout).
-- -------------------------------------------------------------------------
CREATE TABLE ns_ag AS
  SELECT a, sr_formula(s,'ns_map') AS sum_f,
         sr_formula(cb,'ns_map') AS count_b_f,
         sr_formula(cs,'ns_map') AS count_star_f,
         sr_formula(av,'ns_map') IS NULL AS avg_f_is_null
  FROM (SELECT a, sum(b) AS s, count(b) AS cb, count(*) AS cs, avg(b) AS av
        FROM ns_r GROUP BY a) q;
SELECT remove_provenance('ns_ag');
SELECT a, replace(sum_f,'r4*30+r3*30','r3*30+r4*30') AS sum_f,
       replace(replace(count_b_f,'r4*1+r3*1','r3*1+r4*1'),'r6*0+r5*1','r5*1+r6*0') AS count_b_f,
       replace(replace(count_star_f,'r4*1+r3*1','r3*1+r4*1'),'r6*1+r5*1','r5*1+r6*1') AS count_star_f,
       avg_f_is_null
FROM ns_ag ORDER BY a NULLS LAST;
DROP TABLE ns_ag;

-- -------------------------------------------------------------------------
-- 7. HAVING <agg> IS [NOT] NULL across possible worlds (the Kn/Kz split).
--    For the a IS NULL group (rows r5 with value 40, r6 with value NULL):
--    sum(b) IS NULL exactly when r6 is present and r5 is not: p = 0.25.
-- -------------------------------------------------------------------------
CREATE TABLE ns_hn AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_r GROUP BY a HAVING sum(b) IS NULL;
SELECT remove_provenance('ns_hn');
SELECT * FROM ns_hn ORDER BY a NULLS LAST;
DROP TABLE ns_hn;

CREATE TABLE ns_hnn AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_r GROUP BY a HAVING sum(b) IS NOT NULL;
SELECT remove_provenance('ns_hnn');
SELECT * FROM ns_hnn ORDER BY a NULLS LAST;
DROP TABLE ns_hnn;

-- -------------------------------------------------------------------------
-- 8. HAVING <agg> <op> <const> on groups whose aggregate can be NULL.
--    A NULL aggregate (all-NULL group, or a world with no contributing
--    row) never passes the comparison.
--      sum(b) > 5: a=1 iff r1 (0.5); a=3 iff r3 or r4 (0.75);
--                  a=NULL iff r5 (0.5); a=2 never (0).
--      sum(b) < 5: no group in any world (sum is 10, 30, 60, or NULL;
--                  a world's empty group does not exist and a NULL sum
--                  does not pass).
-- -------------------------------------------------------------------------
CREATE TABLE ns_hc AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_r GROUP BY a HAVING sum(b) > 5;
SELECT remove_provenance('ns_hc');
SELECT * FROM ns_hc ORDER BY a NULLS LAST;
DROP TABLE ns_hc;

CREATE TABLE ns_hc2 AS
  SELECT a, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_r GROUP BY a HAVING sum(b) < 5;
SELECT remove_provenance('ns_hc2');
SELECT * FROM ns_hc2 ORDER BY a NULLS LAST;
DROP TABLE ns_hc2;

-- Scalar (no GROUP BY) aggregate over all-NULL input: sum(b) is NULL in
-- every world, so the HAVING comparison never passes.
CREATE TABLE ns_hs AS
  SELECT round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_r WHERE a = 2 HAVING sum(b) > 5;
SELECT remove_provenance('ns_hs');
SELECT * FROM ns_hs;
DROP TABLE ns_hs;

-- -------------------------------------------------------------------------
-- 9. LEFT JOIN: NULL join keys never match (correct in every world);
--    matched and padded arms partition the worlds.
--      (1, matched):  gr1 ⊗ s1        p = 0.25
--      (1, padded):   gr1 ⊖ (gr1⊗s1)  p = 0.25
--      (NULL, padded): gr2            p = 0.5
-- -------------------------------------------------------------------------
CREATE TABLE ns_lj AS
  SELECT ns_gr.a AS ga, ns_s.a AS sa,
         round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_gr LEFT JOIN ns_s ON ns_gr.a = ns_s.a;
SELECT remove_provenance('ns_lj');
SELECT * FROM ns_lj ORDER BY ga NULLS LAST, sa NULLS LAST;
DROP TABLE ns_lj;

-- Outer joins outside the lowered shape: allowed when the null-padded side
-- is untracked (its padding is deterministic and rows keep the tracked
-- arm's tokens), refused with an explicit error when a tracked relation
-- sits on the null-padded side (it would be silently treated as an inner
-- join).
CREATE TABLE ns_u1(a int, b int);
INSERT INTO ns_u1 VALUES (1, 2);
CREATE TABLE ns_lu AS
  SELECT ns_gr.a AS ga, ns_u1.b,
         round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_gr LEFT JOIN ns_u1 ON ns_gr.a = ns_u1.a;
SELECT remove_provenance('ns_lu');
SELECT * FROM ns_lu ORDER BY ga NULLS LAST;
DROP TABLE ns_lu;
SELECT ns_u1.b FROM ns_u1 LEFT JOIN ns_gr ON ns_u1.a = ns_gr.a;

-- RIGHT and FULL variants of the refusal: the null-padded side is the
-- left arm (RIGHT) or both arms (FULL), and ns_gr is tracked.
SELECT ns_gr.a FROM ns_gr RIGHT JOIN ns_u1 ON ns_gr.a = ns_u1.a;
SELECT ns_gr.a FROM ns_gr FULL JOIN ns_u1 ON ns_gr.a = ns_u1.a;

-- Allowed padded-side shapes: a VALUES list, and a nested join of
-- untracked relations; rows keep the tracked arm's tokens.
CREATE TABLE ns_lv AS
  SELECT ns_gr.a AS ga, v.k,
         round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_gr LEFT JOIN (VALUES (1),(7)) v(k) ON ns_gr.a = v.k;
SELECT remove_provenance('ns_lv');
SELECT * FROM ns_lv ORDER BY ga NULLS LAST;
DROP TABLE ns_lv;

CREATE TABLE ns_u2(a int, c int);
INSERT INTO ns_u2 VALUES (1, 9);
CREATE TABLE ns_ln AS
  SELECT ns_gr.a AS ga,
         round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_gr LEFT JOIN (ns_u1 JOIN ns_u2 ON ns_u1.a = ns_u2.a)
       ON ns_gr.a = ns_u1.a;
SELECT remove_provenance('ns_ln');
SELECT * FROM ns_ln ORDER BY ga NULLS LAST;
DROP TABLE ns_ln;

-- A tracked relation inside a nested join on the null-padded side is
-- refused like a direct one.
SELECT ns_u1.b FROM ns_u1
  LEFT JOIN (ns_u2 JOIN ns_gr ON ns_u2.a = ns_gr.a) ON ns_u1.a = ns_u2.a;

-- A CTE over a tracked relation exposes its provsql column, so it counts
-- as tracked on the padded side and is refused; an untracked function RTE
-- there is allowed.
WITH w AS (SELECT * FROM ns_gr)
SELECT ns_u1.b FROM ns_u1 LEFT JOIN w ON ns_u1.a = w.a;
CREATE TABLE ns_lf AS
  SELECT ns_gr.a AS ga, g.k,
         round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_gr LEFT JOIN generate_series(1,2) g(k) ON ns_gr.a = g.k;
SELECT remove_provenance('ns_lf');
SELECT * FROM ns_lf ORDER BY ga NULLS LAST;
DROP TABLE ns_lf;
DROP TABLE ns_u2;
DROP TABLE ns_u1;

-- -------------------------------------------------------------------------
-- 10. Comparisons involving a NULL random_variable are unknown: the row
--     is not an answer (annotation 0), never "certainly true".
-- -------------------------------------------------------------------------
CREATE TABLE ns_m(id int, v provsql.random_variable);
INSERT INTO ns_m VALUES (1, provsql.normal(0,1)), (2, NULL);
SELECT add_provenance('ns_m');

-- v > NULL constant: unknown for every row.
CREATE TABLE ns_rv1 AS
  SELECT id, round(probability_evaluate(provenance(), 'monte-carlo', '10000')::numeric,4) AS p
  FROM ns_m WHERE v > NULL::provsql.random_variable;
SELECT remove_provenance('ns_rv1');
SELECT * FROM ns_rv1 ORDER BY id;
DROP TABLE ns_rv1;

-- v > as_random(0): defined for id=1 (P(N(0,1) > 0) = 0.5), unknown for
-- the NULL cell of id=2 (p = 0).
CREATE TABLE ns_rv2 AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE id WHEN 1 THEN 0.5 WHEN 2 THEN 0.0 END) < 0.02 AS ok
  FROM ns_m WHERE v > provsql.as_random(0);
SELECT remove_provenance('ns_rv2');
SELECT * FROM ns_rv2 ORDER BY id;
DROP TABLE ns_rv2;

-- v IS NULL is a deterministic NullTest, delegated to PostgreSQL.
CREATE TABLE ns_rv3 AS
  SELECT id, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM ns_m WHERE v IS NULL;
SELECT remove_provenance('ns_rv3');
SELECT * FROM ns_rv3 ORDER BY id;
DROP TABLE ns_rv3;

DROP TABLE ns_m;

-- Aggregates over random_variable columns skip NULL cells (SQL
-- semantics): a NULL reading contributes to neither the sum nor avg's
-- count (the value-aware presence indicator), and the statistic
-- aggregates likewise ignore the row.
CREATE TABLE ns_rvagg(g int, v provsql.random_variable);
INSERT INTO ns_rvagg VALUES
  (1, provsql.as_random(10)), (1, provsql.as_random(20)), (1, NULL);
SELECT add_provenance('ns_rvagg');
CREATE TABLE ns_rva AS
  SELECT round(expected(avg(v))::numeric,4) AS avg_e,
         round(expected(sum(v))::numeric,4) AS sum_e,
         round(expected(stddev_samp(v))::numeric,4) AS sd_e
  FROM ns_rvagg GROUP BY g;
SELECT remove_provenance('ns_rva');
SELECT * FROM ns_rva;
DROP TABLE ns_rva;
DROP TABLE ns_rvagg;

-- -------------------------------------------------------------------------
-- 11. Robustness: NULL arguments to C entry points yield NULL or a clean
--     error, never a crash.
-- -------------------------------------------------------------------------
SELECT provsql.provenance_evaluate_compiled(NULL, NULL, 'formula', NULL::text) IS NULL
  AS pec_null_token;
SELECT provsql.where_provenance(NULL::uuid) IS NULL AS wp_null_token;
SELECT provsql.create_gate(public.uuid_generate_v4(), 'plus',
                           ARRAY[NULL::uuid]);

DROP TABLE ns_map;
DROP TABLE ns_r_name;
DROP TABLE ns_gr_name;
DROP TABLE ns_gs_name;
DROP TABLE ns_s_name;
DROP TABLE ns_r;
DROP TABLE ns_gr;
DROP TABLE ns_gs;
DROP TABLE ns_s;
