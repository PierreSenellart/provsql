\set ECHO none
\pset format unaligned

-- ==========================================================================
-- The formula pseudo-semiring is a serialisation of the circuit rather than
-- an evaluation of it, so it renders EVERY gate type -- including the
-- measure-carrier ones (random-variable leaves, scalar arithmetic,
-- mixtures, guarded selection, observations, conditioning) that carry no
-- algebraic meaning and that every proper semiring refuses.
-- ==========================================================================

SET provsql.active = on;
SET search_path TO public, provsql;

-- Random-variable leaves; the last one is latent (its mean is itself a
-- token, written "$0" in the gate's extra encoding and substituted here).
SELECT sr_formula(normal(2.5, 0.5)::uuid)      AS rv_normal,
       sr_formula(exponential(0.7)::uuid)      AS rv_exponential,
       sr_formula(normal(normal(0,1), 1)::uuid) AS rv_latent;

-- Arithmetic gates, in ordinary arithmetic notation (deliberately distinct
-- from the semiring's ⊕ / ⊗ / ⊖).
SELECT sr_formula((normal(0,1) + uniform(0,1))::uuid)     AS a_plus,
       sr_formula((normal(0,1) * uniform(0,1))::uuid)     AS a_times,
       sr_formula((normal(0,1) - uniform(0,1))::uuid)     AS a_minus,
       sr_formula((normal(0,1) / as_random(2))::uuid)     AS a_div,
       sr_formula((-normal(0,1))::uuid)                   AS a_neg;

SELECT sr_formula((uniform(1,2) ^ as_random(2))::uuid)    AS a_pow,
       sr_formula(ln(uniform(1,2))::uuid)                 AS a_ln,
       sr_formula(exp(normal(0,1))::uuid)                 AS a_exp;

-- greatest / least wire their (commutative) operands in an unspecified
-- order, so normalise it here rather than pin one.
SELECT replace(sr_formula(greatest(uniform(0,1), uniform(2,3))::uuid),
               'uniform(2, 3), uniform(0, 1)',
               'uniform(0, 1), uniform(2, 3)') AS a_max,
       replace(sr_formula(least(uniform(0,1), uniform(2,3))::uuid),
               'uniform(2, 3), uniform(0, 1)',
               'uniform(0, 1), uniform(2, 3)') AS a_min;

-- Mixtures: the Bernoulli form renders as a conditional, the categorical
-- one as its weighted outcomes (whose payload lives on the gate, not in a
-- sub-circuit).  Both mint their mixing leaf with uuid_generate_v4 -- each
-- call is an independent RV by construction -- so the abbreviated UUID
-- those leaves render as is masked here.
SELECT regexp_replace(sr_formula(mixture(0.4, normal(0,1), normal(5,1))::uuid),
                      '[0-9a-f]{8}…', '<leaf>', 'g') AS m_bernoulli,
       regexp_replace(sr_formula(categorical(ARRAY[0.25,0.75],
                                             ARRAY[1.0,2.0])::uuid),
                      '[0-9a-f]{8}…', '<leaf>', 'g') AS m_categorical;

-- Observation (likelihood weighting) and conditioning.
SELECT sr_formula(observe(normal(normal(0,1), 1), 2.5)::uuid) AS observation;

CREATE TABLE rv_pair AS SELECT uniform(0,1) AS x, uniform(0,1) AS y;

SELECT sr_formula((x | (x <= y))::uuid) AS conditioned FROM rv_pair;

-- Guarded selection (CASE over random variables), first-match order kept.
SELECT sr_formula((CASE WHEN x >= y THEN x ELSE y END)::uuid) AS guarded
  FROM rv_pair;

-- Aggregates lowering to arithmetic over per-row scalars, including the
-- order-statistic percentile (interleaved [indicator] value wires).
CREATE TABLE rv_rows AS SELECT normal(i, 1) AS v FROM generate_series(1,3) i;

SELECT sr_formula(sum(v)::uuid) AS agg_sum,
       sr_formula(avg(v)::uuid) AS agg_avg,
       sr_formula((percentile_cont(0.5) WITHIN GROUP (ORDER BY v))::uuid)
         AS agg_percentile
  FROM rv_rows;

-- Variable leaves the mapping does not name render as an abbreviated
-- UUID, not as 𝟙: the identity would be absorbed by the enclosing ⊗ and
-- take the whole join structure with it.  A partial mapping names what it
-- covers and identifies the rest, as sr_boolexpr's x<id> fallback does.
-- The tokens come from add_provenance (uuid_generate_v4), so the
-- abbreviations are masked; what is pinned is that they are there and
-- distinct.
CREATE TABLE fmg_t(x int);
INSERT INTO fmg_t VALUES (1), (2);
SELECT add_provenance('fmg_t');
SELECT create_provenance_mapping('fmg_map', 'fmg_t', 'x');
-- Keep only one of the two tuples in the mapping, so the join below has
-- one named leaf and one unnamed one.
DELETE FROM fmg_map WHERE value = 2;

CREATE TABLE fmg_result AS
  SELECT regexp_replace(sr_formula(provenance()), '[0-9a-f]{8}…',
                        '<leaf>', 'g')                        AS unmapped,
         regexp_replace(sr_formula(provenance(), 'fmg_map'),
                        '[0-9a-f]{8}…', '<leaf>', 'g')        AS partially_mapped,
         sr_formula(provenance()) ~ '^[0-9a-f]{8}… ⊗ [0-9a-f]{8}…$'
                                                              AS shape_ok,
         split_part(sr_formula(provenance()), ' ⊗ ', 1)
           <> split_part(sr_formula(provenance()), ' ⊗ ', 2)  AS distinct_leaves
    FROM fmg_t a, fmg_t b WHERE a.x < b.x;
SELECT remove_provenance('fmg_result');
SELECT * FROM fmg_result;

-- A proper semiring still refuses these gates, with a message naming the
-- gate rather than the generic "invalid gate type".
SELECT provenance_evaluate_compiled(normal(0,1)::uuid, NULL, 'counting', 1);

SELECT remove_provenance('fmg_t');
DROP TABLE rv_pair;
DROP TABLE rv_rows;
DROP TABLE fmg_result;
DROP TABLE fmg_map;
DROP TABLE fmg_t;
