\set ECHO none
\if `which c2d > /dev/null 2>&1 && echo true || echo false`
\pset format unaligned

CREATE TABLE c2d_result AS
SELECT city, probability_evaluate(provenance(),'compilation','c2d') AS prob
FROM (
  SELECT DISTINCT city
  FROM personnel
EXCEPT
  SELECT p1.city
  FROM personnel p1,personnel p2
  WHERE p1.id<p2.id AND p1.city=p2.city
  GROUP BY p1.city
) t
ORDER BY CITY;

SELECT remove_provenance('c2d_result');

SELECT city, ROUND(prob::numeric,2) AS prob FROM c2d_result;
DROP TABLE c2d_result;

CREATE TABLE c2d_result AS
  SELECT name, city, shapley(c.provenance,p.provenance,'compilation','c2d') FROM (
    SELECT provenance() from (SELECT DISTINCT 1 FROM (
      (SELECT DISTINCT city FROM personnel)
    EXCEPT
      (SELECT p1.city
      FROM personnel p1, personnel p2
      WHERE p1.city = p2.city AND p1.id < p2.id
      GROUP BY p1.city
      ORDER BY p1.city)
      ) t
    ) u)
  AS c,
  (SELECT *, provenance() FROM personnel) AS p;

SELECT remove_provenance('c2d_result');

SELECT name, city, ROUND(shapley::numeric,3) AS shapley FROM c2d_result
ORDER BY city, name;

DROP TABLE c2d_result;

-- With the cache of compiled d-DNNFs extended to methods called by name,
-- c2d is not answered from what tree-decomposition compiled for the same
-- circuit: it runs, and is then served its own d-DNNF.
CREATE TABLE c2d_lineage AS
  SELECT provenance() AS lin FROM
    (SELECT DISTINCT 1 FROM personnel p1, personnel p2
     WHERE p1.city = p2.city AND p1.id < p2.id) t;
SELECT remove_provenance('c2d_lineage');
SELECT ROUND(probability_evaluate(lin,'possible-worlds')::numeric,6)
  AS by_possible_worlds FROM c2d_lineage;
SELECT prob_cache_reset();
SET provsql.prob_cache = on;
SET provsql.idp_fold = off;
SET provsql.last_eval_method = '';
SELECT ROUND(probability_evaluate(lin,'tree-decomposition')::numeric,6)
  AS by_tree_decomposition FROM c2d_lineage;
SHOW provsql.last_eval_method;
SET provsql.last_eval_method = '';
SELECT ROUND(probability_evaluate(lin,'compilation','c2d')::numeric,6)
  AS by_c2d FROM c2d_lineage;
SHOW provsql.last_eval_method;
SET provsql.last_eval_method = '';
SELECT ROUND(probability_evaluate(lin,'compilation','c2d')::numeric,6)
  AS by_c2d_again FROM c2d_lineage;
SHOW provsql.last_eval_method;
RESET provsql.prob_cache;
RESET provsql.idp_fold;
SELECT prob_cache_reset();
DROP TABLE c2d_lineage;
\else
\echo 'SKIPPING: c2d not available'
\endif
