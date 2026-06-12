\set ECHO none
-- Karp-Luby FPRAS for DNF-shaped provenance circuits ('karp-luby' method).
--
-- The estimator is randomised; we pin provsql.monte_carlo_seed and either
-- compare two same-seed runs for exact equality (reproducibility) or round a
-- high-sample estimate to two decimals and check it against the exact value
-- computed by an exact method.  The circuits are built from the public token
-- combinators provenance_plus / provenance_times (the same ones the query
-- rewriter uses), with each root stashed in a session GUC so the queries below
-- can address it.  provsql.boolean_provenance is off so the load-time folding
-- passes do not rewrite the DNF shape out from under the detector (e.g.
-- absorbing x1 OR (x1 AND x2) down to x1).
\pset format unaligned
SET provsql.provenance = 'semiring';
SET provsql.monte_carlo_seed = 42;

CREATE TABLE kl_in(id int);
INSERT INTO kl_in VALUES (1),(2),(3),(4);
SELECT add_provenance('kl_in');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM kl_in WHERE id=1;
  PERFORM set_prob(provsql, 0.3) FROM kl_in WHERE id=2;
  PERFORM set_prob(provsql, 0.4) FROM kl_in WHERE id=3;
  PERFORM set_prob(provsql, 0.1) FROM kl_in WHERE id=4;
END $$;

DO $$
DECLARE x1 uuid; x2 uuid; x3 uuid; x4 uuid;
BEGIN
  SELECT provsql INTO x1 FROM kl_in WHERE id=1;
  SELECT provsql INTO x2 FROM kl_in WHERE id=2;
  SELECT provsql INTO x3 FROM kl_in WHERE id=3;
  SELECT provsql INTO x4 FROM kl_in WHERE id=4;
  -- (i) flat DNF, disjoint clauses: (x1 AND x2) OR (x3 AND x4).
  --     Independent, so exact = 1-(1-.15)(1-.04) = 0.184.
  PERFORM set_config('kl.flat', provenance_plus(ARRAY[
            provenance_times(x1,x2), provenance_times(x3,x4)])::text, false);
  -- (ii) one-level sharing: (x1 AND x2) OR (x1 AND x3).
  --      = P(x1)*P(x2 OR x3) = .5*(1-.7*.6) = 0.29.  x1 shared across
  --      clauses, so not read-once: 'independent' refuses, Karp-Luby does not.
  PERFORM set_config('kl.shared', provenance_plus(ARRAY[
            provenance_times(x1,x2), provenance_times(x1,x3)])::text, false);
  -- (iii) clause subsumption: x1 OR (x1 AND x2).  = P(x1) = 0.5.  The
  --       smallest-index coverage rejection must keep the count right.
  PERFORM set_config('kl.subsume', provenance_plus(ARRAY[
            x1, provenance_times(x1,x2)])::text, false);
  -- (iv) non-DNF (an OR below an AND): (x1 OR x2) AND x3.
  PERFORM set_config('kl.nondnf', provenance_times(
            provenance_plus(ARRAY[x1,x2]), x3)::text, false);
END $$;

\set flat    '(current_setting(''kl.flat'')::uuid)'
\set shared  '(current_setting(''kl.shared'')::uuid)'
\set subsume '(current_setting(''kl.subsume'')::uuid)'
\set nondnf  '(current_setting(''kl.nondnf'')::uuid)'

-- (i)-(iii): Karp-Luby vs the exact value of each circuit.
SELECT 'flat'    AS circuit,
       round(probability_evaluate(:flat,    'independent')::numeric,2)        AS exact,
       round(probability_evaluate(:flat,    'karp-luby','300000')::numeric,2) AS karp_luby
UNION ALL
SELECT 'shared',
       round(probability_evaluate(:shared,  'tree-decomposition')::numeric,2),
       round(probability_evaluate(:shared,  'karp-luby','300000')::numeric,2)
UNION ALL
SELECT 'subsume',
       round(probability_evaluate(:subsume, 'possible-worlds')::numeric,2),
       round(probability_evaluate(:subsume, 'karp-luby','300000')::numeric,2)
ORDER BY circuit;

-- (v) reproducibility: two runs at the same pinned seed are bit-identical.
SELECT probability_evaluate(:shared,'karp-luby','100000')
     = probability_evaluate(:shared,'karp-luby','100000') AS reproducible;

-- (vii) the argument forms all resolve to the same DNF and estimate 0.29:
--       samples=, bare integer, eps=, and eps=,delta=,max_samples=.
SELECT round(probability_evaluate(:shared,'karp-luby','samples=300000')::numeric,2) AS samples_kw,
       round(probability_evaluate(:shared,'karp-luby','300000')::numeric,2)         AS samples_bare,
       round(probability_evaluate(:shared,'karp-luby','eps=0.05')::numeric,2)       AS eps_only,
       round(probability_evaluate(:shared,'karp-luby','eps=0.05,delta=0.01,max_samples=200000')::numeric,2) AS eps_delta_cap;

-- (iv) a non-DNF circuit is refused (warning + error).
SELECT probability_evaluate(:nondnf,'karp-luby','1000');

-- (viii) argument-grammar errors.
SELECT probability_evaluate(:shared,'karp-luby','samples=100,eps=0.1');     -- conflict
SELECT probability_evaluate(:shared,'karp-luby','delta=0.01');              -- delta needs epsilon
SELECT probability_evaluate(:shared,'karp-luby','samples=100,max_samples=50'); -- max_samples needs adaptive
SELECT probability_evaluate(:shared,'karp-luby','foo=1');                   -- unknown key
SELECT probability_evaluate(:shared,'karp-luby','not_a_number');            -- bad sample count

-- The (eps, delta) approximation guarantee is surfaced as a machine-readable
-- NOTICE at verbose_level >= 5 (the level Studio sets for evaluation), so a UI
-- can render it; plain evaluation at the default level stays quiet.  Relative
-- guarantee for karp-luby, over m clauses.
-- The reported sample count is the number of rounds the self-adjusting
-- stopping rule actually ran (it stops once the estimate provably meets the
-- target), which here is below the fixed worst-case bound over m clauses.
SET provsql.verbose_level = 5;
SELECT round(probability_evaluate(:shared,'karp-luby','eps=0.1,delta=0.05')::numeric,2) AS with_guarantee;
-- A max_samples below the stopping rule's threshold is hit before the
-- (eps, delta) target: a warning fires and the surfaced guarantee is
-- downgraded to the relative error achieved over the samples actually spent.
SELECT probability_evaluate(:shared,'karp-luby','eps=0.1,delta=0.05,max_samples=100') IS NOT NULL AS capped;
RESET provsql.verbose_level;

DROP TABLE kl_in;
RESET provsql.provenance;
RESET provsql.monte_carlo_seed;
