\set ECHO none
\pset format unaligned

-- A latent random variable built in a CTE and referenced from several
-- scopes must resolve to a SINGLE leaf gate: SQL gives a WITH query
-- containing volatile functions single-evaluation semantics, and the
-- RV-comparison rewrite must preserve them.  A CTE body that needs no
-- rewriting is left as a real CTE (evaluated once by PostgreSQL) instead
-- of being inlined -- and thus re-evaluated -- at each reference.
-- Regression test for the shared-latent posterior-inference bug where the
-- evidence conditioned an independent copy of the latent, silently
-- collapsing the posterior to the prior.

SET search_path TO provsql, public;
SET provsql.monte_carlo_seed = 1;

-- Normal-Normal conjugate update: prior N(20, 5), observations 23/24/22
-- with likelihood sd 2.  Exact posterior: mean 22.848, variance 1.266
-- (the buggy decoupled form returned the prior: mean 20, variance 25).
WITH g  AS (SELECT normal(20, 5) AS mu),
     ev AS (SELECT and_agg(| (normal(g.mu, 2) = d)) AS e
            FROM g CROSS JOIN (VALUES (23.0), (24.0), (22.0)) AS t(d))
SELECT abs(expected(g.mu)       - 20.0)   < 0.5 AS prior_mean,
       abs(expected(g.mu, ev.e) - 22.848) < 0.5 AS posterior_mean,
       abs(variance(g.mu, ev.e) - 1.266)  < 0.5 AS posterior_var
FROM g, ev;

-- Leaf identity across scopes: the CTE's volatile constructor runs once,
-- so the outer reference and the one inside the (rewritten) evidence
-- subquery read the same token.
WITH g  AS (SELECT normal(0, 1) AS x),
     ev AS (SELECT (g.x > 0) AS e, g.x AS inner_x FROM g)
SELECT g.x::uuid = ev.inner_x::uuid AS leaf_shared FROM g, ev;

-- The residual multi-evaluation case: a CTE whose body itself needs the
-- rewrite (it contains an RV comparison) is still inlined per reference;
-- the re-evaluation of its volatile constructors is surfaced as a warning
-- rather than left silent.
WITH e AS (SELECT (| (normal(0, 1) > 0)) AS t)
SELECT a.t <> b.t AS decoupled FROM e a, e b;
