\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Recursive queries over cyclic data under the 'absorptive' provenance
-- class: the fixpoint stops once every minimal (tuple-repetition-free)
-- derivation is covered -- longer derivations are absorbed in any
-- absorptive semiring (Deutch, Milo, Roy & Tannen, ICDT 2014) -- and
-- the resulting tokens carry the 'absorptive' assumption marker.
-- Absorptive evaluations (probability, nonnegative min-plus) proceed
-- exactly; non-absorptive ones (counting, plain tropical) refuse.

SET provsql.provenance = 'absorptive';

-- Triangle 1->2->3->1 with a tail 3->4; probabilities and costs.
CREATE TABLE absr_edge(src int, dst int, p float8, cost float8);
INSERT INTO absr_edge VALUES
  (1,2,0.5,3),(2,3,0.5,4),(3,1,0.5,1),(3,4,0.5,5);
SELECT add_provenance('absr_edge');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM absr_edge; END $$;
SELECT create_provenance_mapping('absr_cost', 'absr_edge', 'cost');

CREATE TABLE absr_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM absr_edge e JOIN reach r ON e.src = r.node
  )
  SELECT node, get_gate_type(provenance()) AS root_type,
         get_extra(provenance()) AS assumption,
         round(probability_evaluate(provenance())::numeric, 6) AS prob,
         sr_tropical(provenance(), 'absr_cost', nonnegative => true)
           AS min_cost
  FROM reach;
SELECT remove_provenance('absr_r');
SELECT * FROM absr_r ORDER BY node;
DROP TABLE absr_r;

-- Non-absorptive evaluation refuses the tagged tokens: counting (the
-- number of derivations is genuinely infinite on cyclic data) and the
-- unrestricted tropical semiring (negative costs would make cyclic
-- min-cost unbounded).
CREATE TABLE absr_t AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM absr_edge e JOIN reach r ON e.src = r.node
  )
  SELECT node, provenance() pv FROM reach;
SELECT remove_provenance('absr_t');
SELECT sr_counting(pv, 'absr_cost') FROM absr_t WHERE node = 4;
SELECT sr_tropical(pv, 'absr_cost') FROM absr_t WHERE node = 4;
-- ... and the nonnegative variant rejects negative costs.
CREATE TABLE absr_neg(v float8);
INSERT INTO absr_neg VALUES (-1);
SELECT add_provenance('absr_neg');
SELECT create_provenance_mapping('absr_negmap', 'absr_neg', 'v');
SELECT sr_tropical(provenance(), 'absr_negmap', nonnegative => true)
FROM absr_neg;
DROP TABLE absr_t;
DROP TABLE absr_neg;

-- Acyclic data reaches the structural fixpoint even under the
-- 'absorptive' class: the circuit is universal, untagged, and counting
-- works (2 derivations of node 3: direct and via the shortcut).
CREATE TABLE absr_dag(src int, dst int, n int);
INSERT INTO absr_dag VALUES (1,2,1),(2,3,1),(1,3,1);
SELECT add_provenance('absr_dag');
SELECT create_provenance_mapping('absr_dagmap', 'absr_dag', 'n');
CREATE TABLE absr_d AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM absr_dag e JOIN reach r ON e.src = r.node
  )
  SELECT node, get_gate_type(provenance()) AS root_type,
         sr_counting(provenance(), 'absr_dagmap') AS nb_derivations
  FROM reach;
SELECT remove_provenance('absr_d');
SELECT * FROM absr_d ORDER BY node;
DROP TABLE absr_d;
DROP TABLE absr_dag;
DROP TABLE absr_edge;

RESET provsql.provenance;
