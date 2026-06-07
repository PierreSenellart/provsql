\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- The HAVING possible-worlds enumeration is a d-DNNF by construction
-- when the group's contributors are independent base tuples: the world
-- terms partition the worlds (deterministic OR) and each is an AND of
-- literals over distinct tuples (decomposable).  The Boolean-circuit
-- construction now persists that certificate, so the linear
-- certificate-aware 'independent' method evaluates such HAVING tokens
-- exactly -- covering shapes the closed-form passes do not (AVG,
-- arithmetic over several aggregates, choose() vs text).

CREATE TABLE hcert(grp int, val int, p float8, who text);
INSERT INTO hcert VALUES
  (1,1,0.5,'a'),(1,2,0.6,'b'),(1,5,0.7,'c'),(1,8,0.4,'d');
SELECT add_provenance('hcert');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM hcert; END $$;

-- AVG vs constant (no closed-form pass): 'independent' agrees with the
-- exhaustive method exactly.
CREATE TABLE hcert_avg AS
  SELECT grp FROM hcert GROUP BY grp HAVING avg(val) > 3;
CREATE TABLE hcert_avg_p AS
  SELECT grp,
         round(probability_evaluate(provenance(), 'independent')::numeric, 6)
           AS p_independent,
         round(probability_evaluate(provenance(), 'possible-worlds')::numeric, 6)
           AS p_worlds
  FROM hcert_avg GROUP BY grp, provenance();
SELECT remove_provenance('hcert_avg_p');
SELECT * FROM hcert_avg_p;
DROP TABLE hcert_avg_p;
SELECT remove_provenance('hcert_avg');
DROP TABLE hcert_avg;

-- Arithmetic over several aggregates (the joint-worlds path):
-- sum(val) > 3*count(*) is avg(val) > 3 in disguise, same probability.
CREATE TABLE hcert_gen AS
  SELECT grp FROM hcert GROUP BY grp HAVING sum(val) > 3 * count(*);
CREATE TABLE hcert_gen_p AS
  SELECT grp,
         round(probability_evaluate(provenance(), 'independent')::numeric, 6)
           AS p_independent
  FROM hcert_gen GROUP BY grp, provenance();
SELECT remove_provenance('hcert_gen_p');
SELECT * FROM hcert_gen_p;
DROP TABLE hcert_gen_p;
SELECT remove_provenance('hcert_gen');
DROP TABLE hcert_gen;

-- choose() vs a text constant (PICKFIRST): P = (1-p_a) * p_b = 0.3.
CREATE TABLE hcert_choose AS
  SELECT grp FROM hcert GROUP BY grp HAVING choose(who) = 'b';
CREATE TABLE hcert_choose_p AS
  SELECT grp,
         round(probability_evaluate(provenance(), 'independent')::numeric, 6)
           AS p_independent,
         round(probability_evaluate(provenance(), 'possible-worlds')::numeric, 6)
           AS p_worlds
  FROM hcert_choose GROUP BY grp, provenance();
SELECT remove_provenance('hcert_choose_p');
SELECT * FROM hcert_choose_p;
DROP TABLE hcert_choose_p;
SELECT remove_provenance('hcert_choose');
DROP TABLE hcert_choose;

-- Correlated contributors (join products sharing a base tuple) are NOT
-- certified: 'independent' refuses rather than risking a wrong product,
-- and the exhaustive method stays exact.
CREATE TABLE hcert_base(x int, p float8);
INSERT INTO hcert_base VALUES (1, 0.5), (2, 0.5);
SELECT add_provenance('hcert_base');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM hcert_base; END $$;
CREATE TABLE hcert_join AS
  SELECT 1 AS g FROM hcert_base b1, hcert_base b2
  GROUP BY 1 HAVING avg(b1.x + b2.x) > 2.4;
CREATE TABLE hcert_join_p AS
  SELECT g,
         round(probability_evaluate(provenance(), 'possible-worlds')::numeric, 6)
           AS p_worlds
  FROM hcert_join GROUP BY g, provenance();
SELECT remove_provenance('hcert_join_p');
SELECT * FROM hcert_join_p;
DROP TABLE hcert_join_p;
SELECT probability_evaluate(provenance(), 'independent') AS must_refuse
FROM hcert_join GROUP BY g, provenance();
SELECT remove_provenance('hcert_join');
DROP TABLE hcert_join;
DROP TABLE hcert_base;
DROP TABLE hcert;
