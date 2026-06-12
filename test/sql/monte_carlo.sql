\set ECHO none
\pset format unaligned

CREATE TABLE mc_result AS
SELECT city, probability_evaluate(provenance(),'monte-carlo','10000') AS prob
FROM (
  SELECT DISTINCT city
  FROM personnel
EXCEPT 
  SELECT p1.city
  FROM personnel p1,personnel p2
  WHERE p1.id<p2.id AND p1.city=p2.city
  GROUP BY p1.city
) t;

SELECT remove_provenance('mc_result');

SELECT city, ROUND(prob::numeric,1) FROM mc_result WHERE city = 'Paris';
DROP TABLE mc_result;

-- Argument grammar: a bare integer and samples=N are equivalent, and an
-- *additive* (eps,delta) target is accepted -- |estimate - p| <= eps with
-- probability >= 1-delta after N=ceil(ln(2/delta)/(2*eps^2)) samples
-- (Hoeffding, so the count is independent of p).  Pin the seed and cross-check
-- the three forms against the exact probability of x1 OR x2 (independent),
-- 1-(1-.5)(1-.4) = 0.7.  (For the *relative* guarantee that stays useful on
-- rare events, see the 'karp-luby' method / karp_luby test.)
SET provsql.monte_carlo_seed = 42;
CREATE TABLE mc_arg_in(id int);
INSERT INTO mc_arg_in VALUES (1),(2);
SELECT add_provenance('mc_arg_in');
DO $$ BEGIN
  PERFORM set_prob(provsql,0.5) FROM mc_arg_in WHERE id=1;
  PERFORM set_prob(provsql,0.4) FROM mc_arg_in WHERE id=2;
  PERFORM set_config('mc.root', provenance_plus(ARRAY[
            (SELECT provsql FROM mc_arg_in WHERE id=1),
            (SELECT provsql FROM mc_arg_in WHERE id=2)])::text, false);
END $$;
\set root '(current_setting(''mc.root'')::uuid)'
SELECT round(probability_evaluate(:root,'independent')::numeric,2)                  AS exact,
       round(probability_evaluate(:root,'monte-carlo','300000')::numeric,2)         AS bare,
       round(probability_evaluate(:root,'monte-carlo','samples=300000')::numeric,2) AS samples_kw,
       round(probability_evaluate(:root,'monte-carlo','eps=0.005,delta=0.01')::numeric,2) AS eps_additive;
-- samples is mutually exclusive with the (eps,delta) path; unknown keys raise.
SELECT probability_evaluate(:root,'monte-carlo','samples=100,eps=0.1');  -- conflict
SELECT probability_evaluate(:root,'monte-carlo','foo=1');                -- unknown key

-- The *additive* (eps,delta) guarantee is surfaced as a machine-readable
-- NOTICE at verbose_level >= 5 (the level Studio sets); kind=additive, with
-- the derived sample count.  (karp-luby's relative guarantee is checked in the
-- karp_luby test.)
SET provsql.verbose_level = 5;
SELECT round(probability_evaluate(:root,'monte-carlo','eps=0.005,delta=0.01')::numeric,1) AS with_guarantee;
RESET provsql.verbose_level;

DROP TABLE mc_arg_in;
RESET provsql.monte_carlo_seed;
