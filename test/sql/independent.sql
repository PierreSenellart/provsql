\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Will fail because this is not independent
CREATE TABLE independent_result AS
SELECT city, probability_evaluate(provenance(),'independent') AS prob
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

CREATE TABLE independent_result AS
SELECT city, probability_evaluate(provenance(),'independent') AS prob
FROM personnel
GROUP BY city
ORDER BY city;

SELECT remove_provenance('independent_result');

SELECT city, ROUND(prob::numeric,2) AS prob FROM independent_result;
DROP TABLE independent_result;

-- A shared subgraph whose leaves are all constants (probability 0
-- or 1) does not violate read-once-ness : revisiting a constant
-- yields the same constant value, no Boolean variable is shared.
-- Pre-fix, independentEvaluation threw "Not an independent circuit"
-- on the second visit ; the case BooleanGate::IN branch now skips
-- the seen-set bookkeeping for prob 0 / 1 leaves.  Build
-- times(plus(u, v), plus(u, v)) with u.prob = 1 and v.prob = 0 :
-- the shared plus(u, v) is reached from both arms of times, but the
-- two underlying leaves are constants so independent should succeed.
-- Use a dedicated table so no other test's probabilities are
-- mutated.
CREATE TABLE ind_const_t(id int);
INSERT INTO ind_const_t VALUES (1), (2);
SELECT add_provenance('ind_const_t');
DO $$
DECLARE u uuid; v uuid; sub uuid; root uuid;
BEGIN
  SELECT provsql INTO u FROM ind_const_t WHERE id = 1;
  SELECT provsql INTO v FROM ind_const_t WHERE id = 2;
  PERFORM set_prob(u, 1.0);
  PERFORM set_prob(v, 0.0);
  sub  := provenance_plus(ARRAY[u, v]);
  root := public.uuid_generate_v5(uuid_ns_provsql(),
                                  concat('ind-const-shared', sub));
  PERFORM create_gate(root, 'times', ARRAY[sub, sub]);
  PERFORM set_config('ind_const.root', root::text, false);
END $$;
-- Expected : plus(u=1, v=0) = 1 ; times(1, 1) = 1.
SELECT round(probability_evaluate(
                current_setting('ind_const.root')::uuid, 'independent')
              ::numeric, 9) AS p_const_shared;
DROP TABLE ind_const_t;
