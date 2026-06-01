\set ECHO none
-- Whole-circuit (eps,delta)-RELATIVE probability via the Dagum-Karp-Luby-Ross
-- stopping rule ('stopping-rule' method, src/MonteCarloSampler.cpp
-- monteCarloRVStopping).  Unlike 'karp-luby' it samples the whole circuit
-- through the RV-aware sampler's evalBool, so it is NOT restricted to a
-- DNF shape: a circuit with an OR below an AND -- which karp-luby refuses -- is
-- estimated here.  The estimator is randomised; we pin provsql.monte_carlo_seed
-- and check (a) the estimate lands within the relative target of the exact
-- value, (b) two same-seed runs are bit-identical, (c) the argument grammar,
-- and (d) the (eps,delta) guarantee NOTICE.  Circuits are built from the public
-- token combinators, with provsql.boolean_provenance off so the load-time
-- folding passes leave the shape intact.
\pset format unaligned
SET search_path TO provsql_test,provsql;
SET provsql.boolean_provenance = off;
SET provsql.monte_carlo_seed = 42;

CREATE TABLE sr_in(id int);
INSERT INTO sr_in VALUES (1),(2),(3),(4);
SELECT add_provenance('sr_in');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM sr_in WHERE id=1;
  PERFORM set_prob(provsql, 0.3) FROM sr_in WHERE id=2;
  PERFORM set_prob(provsql, 0.4) FROM sr_in WHERE id=3;
  PERFORM set_prob(provsql, 0.1) FROM sr_in WHERE id=4;
END $$;

DO $$
DECLARE x1 uuid; x2 uuid; x3 uuid; x4 uuid;
BEGIN
  SELECT provsql INTO x1 FROM sr_in WHERE id=1;
  SELECT provsql INTO x2 FROM sr_in WHERE id=2;
  SELECT provsql INTO x3 FROM sr_in WHERE id=3;
  SELECT provsql INTO x4 FROM sr_in WHERE id=4;
  -- (i) flat DNF, disjoint clauses: (x1 AND x2) OR (x3 AND x4).
  --     Independent, exact = 1-(1-.15)(1-.04) = 0.184.
  PERFORM set_config('sr.flat', provenance_plus(ARRAY[
            provenance_times(x1,x2), provenance_times(x3,x4)])::text, false);
  -- (ii) one-level sharing: (x1 AND x2) OR (x1 AND x3) = .5*(1-.7*.6) = 0.29.
  PERFORM set_config('sr.shared', provenance_plus(ARRAY[
            provenance_times(x1,x2), provenance_times(x1,x3)])::text, false);
  -- (iii) NON-DNF (an OR below an AND): (x1 OR x2) AND x3 = (1-.5*.7)*.4 = 0.26.
  --       karp-luby refuses this; the whole-circuit sampler handles it.
  PERFORM set_config('sr.nondnf', provenance_times(
            provenance_plus(ARRAY[x1,x2]), x3)::text, false);
END $$;

\set flat   '(current_setting(''sr.flat'')::uuid)'
\set shared '(current_setting(''sr.shared'')::uuid)'
\set nondnf '(current_setting(''sr.nondnf'')::uuid)'

-- (i)-(iii): the relative estimate lands within the (loosened to 2x the
-- target, for portability) relative tolerance of the exact value -- including
-- the non-DNF circuit.  Booleans rather than rounded estimates: a stopping
-- time Y1/N is sensitive to the platform RNG sequence, but the *relative
-- guarantee* is exactly what is portable.
SELECT 'flat'   AS circuit,
       abs(probability_evaluate(:flat,  'stopping-rule','eps=0.05,delta=0.01')
           - probability_evaluate(:flat,  'independent'))
         <= 0.10 * probability_evaluate(:flat,  'independent') AS within_tol
UNION ALL
SELECT 'shared',
       abs(probability_evaluate(:shared,'stopping-rule','eps=0.05,delta=0.01')
           - probability_evaluate(:shared,'tree-decomposition'))
         <= 0.10 * probability_evaluate(:shared,'tree-decomposition')
UNION ALL
SELECT 'nondnf',
       abs(probability_evaluate(:nondnf,'stopping-rule','eps=0.05,delta=0.01')
           - probability_evaluate(:nondnf,'tree-decomposition'))
         <= 0.10 * probability_evaluate(:nondnf,'tree-decomposition')
ORDER BY circuit;

-- reproducibility: two runs at the same pinned seed are bit-identical.
SELECT probability_evaluate(:shared,'stopping-rule','eps=0.05,delta=0.01')
     = probability_evaluate(:shared,'stopping-rule','eps=0.05,delta=0.01') AS reproducible;

-- argument grammar: eps-only and eps+delta+max_samples are accepted and land
-- in range (0.29); a fixed sample count is rejected (adaptive only).
SELECT abs(probability_evaluate(:shared,'stopping-rule','eps=0.1') - 0.29) <= 0.05      AS eps_only,
       abs(probability_evaluate(:shared,'stopping-rule','eps=0.1,delta=0.05,max_samples=2000000') - 0.29) <= 0.05 AS eps_delta_cap;

SELECT probability_evaluate(:shared,'stopping-rule','1000');             -- fixed-count: rejected
SELECT probability_evaluate(:shared,'stopping-rule','delta=0.01');       -- delta needs epsilon
SELECT probability_evaluate(:shared,'stopping-rule','foo=1');            -- unknown key

-- The (eps, delta) RELATIVE guarantee is surfaced as a machine-readable NOTICE
-- at verbose_level >= 5 (the level Studio sets for evaluation).
SET provsql.verbose_level = 5;
SELECT probability_evaluate(:shared,'stopping-rule','eps=0.1,delta=0.05') IS NOT NULL AS with_guarantee;
-- A tiny max_samples is hit before the relative target: a warning fires and the
-- surfaced guarantee downgrades to the ADDITIVE error achieved over the spent
-- budget (we never claim a relative guarantee we did not meet).
SELECT probability_evaluate(:shared,'stopping-rule','eps=0.001,delta=0.01,max_samples=50') IS NOT NULL AS capped;
RESET provsql.verbose_level;

DROP TABLE sr_in;
RESET provsql.boolean_provenance;
RESET provsql.monte_carlo_seed;
