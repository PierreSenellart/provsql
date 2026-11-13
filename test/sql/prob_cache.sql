\set ECHO none
\pset format unaligned

SELECT prob_cache_reset();
SET provsql.idp_fold = off;

CREATE TABLE city_lineage AS
  SELECT city, provenance() AS lin FROM (SELECT DISTINCT city FROM personnel) t;
SELECT remove_provenance('city_lineage');

SELECT city, circuit_canonical_form(lin) AS kappa
FROM city_lineage ORDER BY city;

CREATE TABLE exc_lineage AS SELECT city, provenance() AS lin FROM
  (SELECT DISTINCT city FROM personnel
   EXCEPT
   SELECT p1.city FROM personnel p1, personnel p2
   WHERE p1.id<p2.id AND p1.city=p2.city GROUP BY p1.city) t;
SELECT remove_provenance('exc_lineage');

SELECT city, circuit_canonical_form(lin) AS kappa
FROM exc_lineage ORDER BY city;
SELECT city, circuit_island_key(lin) AS island_key
FROM exc_lineage ORDER BY city;

SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,4)
  AS p_berlin FROM city_lineage WHERE city='Berlin';
SELECT prob_cache_stats() - 'bytes' AS stats_after_berlin;

SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,4)
  AS p_new_york FROM city_lineage WHERE city='New York';
SELECT prob_cache_stats() - 'bytes' AS stats_after_new_york;

SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,4)
  AS p_paris FROM city_lineage WHERE city='Paris';
SELECT prob_cache_stats() - 'bytes' AS stats_after_paris;

CREATE TABLE conj AS SELECT provenance() AS lin FROM
  (SELECT DISTINCT 1 AS x FROM personnel WHERE city='Berlin') a
  JOIN (SELECT DISTINCT 1 AS x FROM personnel WHERE city='New York') b
  USING (x);
SELECT remove_provenance('conj');

SELECT circuit_island_key(lin) AS island_key FROM conj;

SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,6)
  AS p_conj FROM conj;
SELECT prob_cache_stats() - 'bytes' AS stats_after_conj;

SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,6)
  AS p_conj_repeat FROM conj;
SELECT prob_cache_stats() - 'bytes' AS stats_after_conj_repeat;

SELECT ROUND(probability_evaluate(lin,'possible-worlds')::numeric,6)
  AS p_conj_possible_worlds FROM conj;

CREATE TABLE pair AS SELECT provenance() AS lin FROM
  (SELECT 1 FROM personnel p1, personnel p2 WHERE p1.id=1 AND p2.id=2) t;
SELECT remove_provenance('pair');

SELECT circuit_island_key(lin) AS island_key FROM pair;

SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,6)
  AS p_pair FROM pair;
SELECT prob_cache_stats() - 'bytes' AS stats_after_pair;

SELECT prob_cache_reset();
SET provsql.prob_cache = off;
SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,4)
  AS p_berlin_uncached FROM city_lineage WHERE city='Berlin';
SELECT prob_cache_stats() - 'bytes' AS stats_cache_off;
SET provsql.prob_cache = on;

SELECT prob_cache_reset();
SET provsql.idp_fold = on;
SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,6)
  AS p_pair_folded FROM pair;
SELECT prob_cache_stats() - 'bytes' AS stats_after_folded_pair;
SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,4)
  AS p_berlin_folded FROM city_lineage WHERE city='Berlin';
SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,4)
  AS p_new_york_folded FROM city_lineage WHERE city='New York';
SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,6)
  AS p_conj_folded FROM conj;
SELECT prob_cache_stats() - 'bytes' AS stats_after_folded;
SELECT prob_cache_reset();
SET provsql.prob_cache = off;
SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,4)
  AS p_berlin_fold_only FROM city_lineage WHERE city='Berlin';
SELECT prob_cache_stats() - 'bytes' AS stats_fold_only;
SET provsql.prob_cache = on;
RESET provsql.idp_fold;

CREATE TABLE chain(id int, grp int);
INSERT INTO chain
  SELECT i, g FROM generate_series(1,13) i, generate_series(1,2) g;
SELECT add_provenance('chain');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM chain; END $$;
CREATE TABLE chains AS SELECT provenance() AS lin FROM
  (SELECT DISTINCT 1 FROM chain a, chain b
   WHERE a.grp=b.grp AND b.id=a.id+1) t;
SELECT remove_provenance('chains');

SELECT prob_cache_reset();
SET provsql.last_eval_method = '';
SELECT ROUND(probability_evaluate(lin)::numeric,6)
  AS p_chains_default FROM chains;
SHOW provsql.last_eval_method;
SELECT prob_cache_stats() - 'bytes' AS stats_after_chains_default;

SELECT prob_cache_reset();
SET provsql.idp_fold = off;
SET provsql.last_eval_method = '';
SELECT ROUND(probability_evaluate(lin)::numeric,6)
  AS p_chains_default_unfolded FROM chains;
SHOW provsql.last_eval_method;
SELECT prob_cache_stats() - 'bytes' AS stats_after_chains_default_unfolded;
RESET provsql.idp_fold;
SELECT prob_cache_reset();

DROP TABLE chains;
DROP TABLE chain;
DROP TABLE city_lineage;
DROP TABLE exc_lineage;
DROP TABLE conj;
DROP TABLE pair;
