\set ECHO none
\pset format unaligned

-- Comma-join (FROM a, b WHERE ...) and ANSI JOIN syntax (a JOIN b ON ...)
-- produce different Query trees for the same semantics; rewrite passes that
-- pattern-match one shape have repeatedly missed the other.  This test runs
-- one representative query per feature area in BOTH syntaxes: each pair of
-- result blocks must be identical, so a feature accepting one form and
-- erroring (or mis-tracking) on the other shows up as a test diff.

CREATE TABLE jr(a int, k int, lab text);
CREATE TABLE js(k int, b int, lab text);
CREATE TABLE jd(k int, tag text);  -- untracked dimension
INSERT INTO jr VALUES (1,1,'r1'),(2,2,'r2'),(3,3,'r3');
INSERT INTO js VALUES (1,10,'s10'),(2,20,'s20'),(2,21,'s21');
INSERT INTO jd VALUES (1,'x'),(2,'y'),(3,'x');
SELECT add_provenance('jr');
SELECT add_provenance('js');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.9) FROM jr;
  PERFORM set_prob(provsql, 0.5) FROM js;
END $$;
SELECT create_provenance_mapping('jrmap', 'jr', 'lab');
SELECT create_provenance_mapping('jsmap', 'js', 'lab');
CREATE TABLE jmap AS
  SELECT * FROM jrmap UNION ALL SELECT * FROM jsmap;

-- 1. Basic equijoin: formula readout and probability.
CREATE TABLE je AS
  SELECT a, b, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr, js WHERE jr.k = js.k;
SELECT remove_provenance('je');
SELECT a, b, f, p FROM je ORDER BY a, b;
DROP TABLE je;
CREATE TABLE je AS
  SELECT a, b, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr JOIN js ON jr.k = js.k;
SELECT remove_provenance('je');
SELECT a, b, f, p FROM je ORDER BY a, b;
DROP TABLE je;

-- 2. Three-way join through the untracked dimension.
CREATE TABLE je AS
  SELECT a, b, tag, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr, js, jd WHERE jr.k = js.k AND js.k = jd.k;
SELECT remove_provenance('je');
SELECT a, b, tag, f, p FROM je ORDER BY a, b;
DROP TABLE je;
CREATE TABLE je AS
  SELECT a, b, tag, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr JOIN js ON jr.k = js.k JOIN jd ON js.k = jd.k;
SELECT remove_provenance('je');
SELECT a, b, tag, f, p FROM je ORDER BY a, b;
DROP TABLE je;

-- 3. GROUP BY aggregation over a join (agg_token value + group probability).
CREATE TABLE je AS
  SELECT a, sum(b) AS s,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr, js WHERE jr.k = js.k GROUP BY a;
SELECT remove_provenance('je');
SELECT a, s, p FROM je ORDER BY a;
DROP TABLE je;
CREATE TABLE je AS
  SELECT a, sum(b) AS s,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr JOIN js ON jr.k = js.k GROUP BY a;
SELECT remove_provenance('je');
SELECT a, s, p FROM je ORDER BY a;
DROP TABLE je;

-- 4. HAVING over the joined group.
CREATE TABLE je AS
  SELECT a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr, js WHERE jr.k = js.k GROUP BY a HAVING sum(b) > 15;
SELECT remove_provenance('je');
SELECT a, p FROM je ORDER BY a;
DROP TABLE je;
CREATE TABLE je AS
  SELECT a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr JOIN js ON jr.k = js.k GROUP BY a HAVING sum(b) > 15;
SELECT remove_provenance('je');
SELECT a, p FROM je ORDER BY a;
DROP TABLE je;

-- 5. SELECT DISTINCT over a join: k = 2 has two js contributors, so its
-- annotation must be their disjunction -- s20 + s21, probability
-- 0.9 * (1 - 0.5^2) = 0.675 -- under either syntax.
CREATE TABLE je AS
  SELECT k, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT DISTINCT jr.k FROM jr, js WHERE jr.k = js.k) t;
SELECT remove_provenance('je');
SELECT k, f, p FROM je ORDER BY k;
DROP TABLE je;
CREATE TABLE je AS
  SELECT k, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT DISTINCT jr.k FROM jr JOIN js ON jr.k = js.k) t;
SELECT remove_provenance('je');
SELECT k, f, p FROM je ORDER BY k;
DROP TABLE je;

-- 6. EXCEPT whose arms are joins.
CREATE TABLE je AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT jr.k FROM jr, js WHERE jr.k = js.k
        EXCEPT
        SELECT jr.k FROM jr, jd WHERE jr.k = jd.k AND jd.tag = 'y') e;
SELECT remove_provenance('je');
SELECT k, p FROM je ORDER BY k;
DROP TABLE je;
CREATE TABLE je AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT jr.k FROM jr JOIN js ON jr.k = js.k
        EXCEPT
        SELECT jr.k FROM jr JOIN jd ON jr.k = jd.k WHERE jd.tag = 'y') e;
SELECT remove_provenance('je');
SELECT k, p FROM je ORDER BY k;
DROP TABLE je;

-- 7. UNION whose arms are joins.
CREATE TABLE je AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT jr.k FROM jr, js WHERE jr.k = js.k
        UNION
        SELECT jr.k FROM jr, jd WHERE jr.k = jd.k AND jd.tag = 'x') u;
SELECT remove_provenance('je');
SELECT k, p FROM je ORDER BY k;
DROP TABLE je;
CREATE TABLE je AS
  SELECT k, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT jr.k FROM jr JOIN js ON jr.k = js.k
        UNION
        SELECT jr.k FROM jr JOIN jd ON jr.k = jd.k WHERE jd.tag = 'x') u;
SELECT remove_provenance('je');
SELECT k, p FROM je ORDER BY k;
DROP TABLE je;

-- 8. NOT IN whose body is a join (through the untracked dimension).
CREATE TABLE je AS
  SELECT a, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr WHERE jr.k NOT IN (SELECT js.k FROM js, jd
                             WHERE js.k = jd.k AND jd.tag = 'y');
SELECT remove_provenance('je');
SELECT a, f, p FROM je ORDER BY a;
DROP TABLE je;
CREATE TABLE je AS
  SELECT a, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr WHERE jr.k NOT IN (SELECT js.k FROM js JOIN jd ON js.k = jd.k
                             WHERE jd.tag = 'y');
SELECT remove_provenance('je');
SELECT a, f, p FROM je ORDER BY a;
DROP TABLE je;

-- 8b. The same NOT IN with the body's join written as JOIN ... USING: the
-- merged column in the body's target list resolves through the dissolved
-- join's alias.  Must match case 8 exactly.
CREATE TABLE je AS
  SELECT a, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr WHERE jr.k NOT IN (SELECT k FROM js JOIN jd USING (k)
                             WHERE jd.tag = 'y');
SELECT remove_provenance('je');
SELECT a, f, p FROM je ORDER BY a;
DROP TABLE je;

-- 9. Correlated scalar sublink: joined outer FROM, joined body.
CREATE TABLE je AS
  SELECT a, (SELECT max(js.b) FROM js, jd
             WHERE js.k = jd.k AND js.k = jr.k) AS m
  FROM jr, jd WHERE jr.k = jd.k;
SELECT remove_provenance('je');
SELECT a, m FROM je ORDER BY a;
DROP TABLE je;
CREATE TABLE je AS
  SELECT a, (SELECT max(js.b) FROM js JOIN jd ON js.k = jd.k
             WHERE js.k = jr.k) AS m
  FROM jr JOIN jd ON jr.k = jd.k;
SELECT remove_provenance('je');
SELECT a, m FROM je ORDER BY a;
DROP TABLE je;

-- 10. Nested subquery FROM whose inner is a join.
CREATE TABLE je AS
  SELECT t.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT jr.a AS a FROM jr, js WHERE jr.k = js.k) t;
SELECT remove_provenance('je');
SELECT a, p FROM je ORDER BY a;
DROP TABLE je;
CREATE TABLE je AS
  SELECT t.a AS a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT jr.a AS a FROM jr JOIN js ON jr.k = js.k) t;
SELECT remove_provenance('je');
SELECT a, p FROM je ORDER BY a;
DROP TABLE je;

-- 11. Where-provenance through the join's equijoin condition (UUIDs
-- stripped for stability).
SET provsql.provenance TO 'where';
CREATE TABLE je AS
  SELECT a, b,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g') AS w
  FROM jr, js WHERE jr.k = js.k;
SELECT remove_provenance('je');
SELECT a, b, w FROM je ORDER BY a, b;
DROP TABLE je;
CREATE TABLE je AS
  SELECT a, b,
    regexp_replace(where_provenance(provenance()),':[0-9a-f-]*:','::','g') AS w
  FROM jr JOIN js ON jr.k = js.k;
SELECT remove_provenance('je');
SELECT a, b, w FROM je ORDER BY a, b;
DROP TABLE je;
RESET provsql.provenance;

-- 12. Safe-query rewriter path (Boolean provenance, DISTINCT projection).
SET provsql.provenance TO 'boolean';
CREATE TABLE je AS
  SELECT a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT DISTINCT jr.a FROM jr, js WHERE jr.k = js.k) t;
SELECT remove_provenance('je');
SELECT a, p FROM je ORDER BY a;
DROP TABLE je;
CREATE TABLE je AS
  SELECT a, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT DISTINCT jr.a FROM jr JOIN js ON jr.k = js.k) t;
SELECT remove_provenance('je');
SELECT a, p FROM je ORDER BY a;
DROP TABLE je;
RESET provsql.provenance;

-- 13. Random-variable comparison lifted from WHERE, over a join.
CREATE TABLE jrv(k int, x random_variable);
INSERT INTO jrv
  SELECT g, provsql.normal(g, 1) FROM generate_series(1, 3) g;
CREATE TABLE je AS
  SELECT jrv.k AS k, round(expected(x)::numeric, 4) AS e,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jrv, jr WHERE jrv.k = jr.k AND x > 2;
SELECT remove_provenance('je');
SELECT k, e, p FROM je ORDER BY k;
DROP TABLE je;
CREATE TABLE je AS
  SELECT jrv.k AS k, round(expected(x)::numeric, 4) AS e,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jrv JOIN jr ON jrv.k = jr.k WHERE x > 2;
SELECT remove_provenance('je');
SELECT k, e, p FROM je ORDER BY k;
DROP TABLE je;
DROP TABLE jrv;

-- 14. JOIN ... USING: the merged column is a join-alias reference, resolved
-- through the dissolved join RTE's joinaliasvars by the canonicalisation.
CREATE TABLE je AS
  SELECT jr.k AS k, b, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr, js WHERE jr.k = js.k;
SELECT remove_provenance('je');
SELECT k, b, f, p FROM je ORDER BY k, b;
DROP TABLE je;
CREATE TABLE je AS
  SELECT k, b, sr_formula(provenance(), 'jmap') AS f,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM jr JOIN js USING (k);
SELECT remove_provenance('je');
SELECT k, b, f, p FROM je ORDER BY k, b;
DROP TABLE je;

DROP TABLE jmap;
DROP TABLE jrmap;
DROP TABLE jsmap;
DROP TABLE jd;
DROP TABLE js;
DROP TABLE jr;
