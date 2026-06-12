\set ECHO none
\pset format unaligned

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

-- Acyclic data under the 'absorptive' class: the reachability shape
-- routes through the decomposition-aligned compilation like any
-- other, so the tokens are tagged -- exact for absorptive evaluations
-- (min-plus below) and refused for counting.
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
  SELECT node, provenance() pv FROM reach;
SELECT remove_provenance('absr_d');
SELECT node, get_gate_type(pv) AS root_type,
       sr_tropical(pv, 'absr_dagmap', nonnegative => true) AS min_cost
FROM absr_d ORDER BY node;
SELECT sr_counting(pv, 'absr_dagmap') FROM absr_d WHERE node = 3;
DROP TABLE absr_d;

-- Under the 'semiring' class the same acyclic recursion reaches the
-- structural fixpoint: the circuit is universal, untagged, and
-- counting works (2 derivations of node 3: direct and via the
-- shortcut).
SET provsql.provenance = 'semiring';
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
SET provsql.provenance = 'absorptive';
DROP TABLE absr_edge;

-- Absorptive folds: under the 'absorptive' class the load-time
-- simplification applies the rules sound in every absorptive semiring
-- (plus-idempotence, plus-with-one, plus-absorbs-times), marking the
-- rewritten gates; the value is unchanged for absorptive evaluations
-- and refused for the rest -- including when the fold collapses the
-- marked gate onto a preloaded input leaf.
CREATE TABLE absf_a(x int, c float8);
CREATE TABLE absf_b(x int, c float8);
INSERT INTO absf_a VALUES (1, 2.0);
INSERT INTO absf_b VALUES (1, 10.0);
SELECT add_provenance('absf_a');
SELECT add_provenance('absf_b');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM absf_a; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM absf_b; END $$;
SELECT create_provenance_mapping('absf_amap', 'absf_a', 'c');
SELECT create_provenance_mapping('absf_bmap', 'absf_b', 'c');
CREATE TABLE absf_map AS TABLE absf_amap UNION ALL TABLE absf_bmap;

-- plus(a, times(a, b)): 2 + 2*10 = 22 under 'semiring' counting...
SET provsql.provenance = 'semiring';
CREATE TABLE absf_u AS
  SELECT x FROM absf_a
  UNION SELECT absf_a.x FROM absf_a JOIN absf_b ON absf_a.x = absf_b.x;
CREATE TABLE absf_u_p AS
  SELECT x, sr_counting(provenance(), 'absf_map') AS cnt
  FROM absf_u GROUP BY x, provenance();
SELECT remove_provenance('absf_u_p');
SELECT * FROM absf_u_p;
DROP TABLE absf_u_p;
-- ... while under 'absorptive' the fold absorbs the times into its
-- dominating sibling: min-plus reads the (identical) min-cost, and
-- counting refuses the folded gate rather than returning 2.
SET provsql.provenance = 'absorptive';
CREATE TABLE absf_u_t AS
  SELECT x, sr_tropical(provenance(), 'absf_map', nonnegative => true)
    AS min_cost
  FROM absf_u GROUP BY x, provenance();
SELECT remove_provenance('absf_u_t');
SELECT * FROM absf_u_t;
DROP TABLE absf_u_t;
SELECT x, sr_counting(provenance(), 'absf_map') AS must_refuse
FROM absf_u GROUP BY x, provenance();
SELECT remove_provenance('absf_u');
DROP TABLE absf_u;
DROP TABLE absf_map;
DROP TABLE absf_amap;
DROP TABLE absf_bmap;
DROP TABLE absf_a;
DROP TABLE absf_b;

RESET provsql.provenance;
