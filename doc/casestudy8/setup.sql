-- ======================================================================
-- Case Study 8: ProvSQL as a Probability Calculator
--
-- Fixture for an epidemiology desk that uses ProvSQL as an exact,
-- correlation-aware probability calculator.  Three small probabilistic
-- models, each backing one classic problem:
--
--   screening  – the (disease, test) joint sample space (discrete Bayes)
--   risk       – risk factors that share a common cause (correlation)
--   cases      – per-day case contributions (aggregation)
--
-- The continuous thread builds its biomarker inline with normal(), so it
-- needs no table.
-- ======================================================================

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET search_path TO public, provsql;

-- ----------------------------------------------------------------------
-- Discrete: the joint sample space of (disease, test-positive) for a
-- screening programme, as four mutually-exclusive worlds of one group.
-- Prevalence 1%, sensitivity 90%, specificity 95%:
--   P(D=1)=.01, P(+|D=1)=.9, P(+|D=0)=.05  =>  the four joint masses below.
-- repair_key turns the four rows of group 1 into mutually-exclusive
-- possible worlds; set_prob assigns each its mass.
-- ----------------------------------------------------------------------
CREATE TABLE screening(grp int, disease boolean, positive boolean, p float);
INSERT INTO screening VALUES
  (1, true,  true,  0.009),   -- diseased, true positive
  (1, true,  false, 0.001),   -- diseased, false negative
  (1, false, true,  0.0495),  -- healthy, false positive
  (1, false, false, 0.9405);  -- healthy, true negative
SELECT repair_key('screening', 'grp');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM screening; END $$;

-- ----------------------------------------------------------------------
-- Correlation: three independent risk factors.  Two derived conditions
-- A = shared AND a1 and B = shared AND a2 both depend on `shared`, so
-- they are correlated -- the point of the second problem.
-- ----------------------------------------------------------------------
CREATE TABLE risk(id text, p float);
INSERT INTO risk VALUES ('shared', 0.5), ('a1', 0.6), ('a2', 0.7);
SELECT add_provenance('risk');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM risk; END $$;

-- ----------------------------------------------------------------------
-- Aggregation: per-day case contributions.  Each row is an uncertain
-- count that is included in its region's total with probability p.
-- ----------------------------------------------------------------------
CREATE TABLE cases(day int, region text, n int, p float);
INSERT INTO cases VALUES (1, 'North', 3, 0.5), (1, 'North', 4, 0.5), (1, 'South', 2, 0.8);
SELECT add_provenance('cases');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM cases; END $$;
