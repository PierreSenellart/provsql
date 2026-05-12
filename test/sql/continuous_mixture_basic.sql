\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- A.  Constructor builds a gate_mixture with three wires
-- [p_token (gate_input), x_token, y_token] and returns a random_variable.

CREATE TEMP TABLE bern(p uuid);
INSERT INTO bern VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT p FROM bern), 0.3);

SELECT get_gate_type(random_variable_uuid(
         provsql.mixture(
           (SELECT p FROM bern),
           provsql.normal(-2, 1),
           provsql.normal(2, 1)))) AS mixture_kind;

-- The mixture gate carries exactly three children, in [p, x, y] order:
-- the first wire is the Bernoulli (gate_input), the next two are the
-- branch RVs (here gate_rv normals).  Materialise via a temp table so
-- the volatile constructor fires exactly once.
CREATE TEMP TABLE basic_mix AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT p FROM bern),
             provsql.normal(-2, 1),
             provsql.normal(2, 1))) AS u;

SELECT array_length(get_children(u), 1) AS nb_children,
       get_gate_type((get_children(u))[1]) AS wire0_kind,
       get_gate_type((get_children(u))[2]) AS wire1_kind,
       get_gate_type((get_children(u))[3]) AS wire2_kind
  FROM basic_mix;

-- The first wire is the SAME UUID as the Bernoulli token we passed
-- in -- the constructor must not re-mint the gate_input.  Sharing the
-- token across calls is exactly how users couple branch selection.
SELECT (get_children(u))[1] = (SELECT p FROM bern) AS p_token_preserved
  FROM basic_mix;

-- B.  v5 structural sharing: two calls to mixture(...) with identical
-- (p, x, y) operands collapse to the same gate_mixture node, exactly
-- like arith(PLUS, X, Y).  Draw independence is controlled by p, not
-- by gate identity.  Here we share p (deterministic coin slot) AND
-- the same Dirac branches, so the two mixtures must collide.
CREATE TEMP TABLE two_mix AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT p FROM bern),
             provsql.as_random(-5),
             provsql.as_random(5))) AS u1,
         random_variable_uuid(
           provsql.mixture(
             (SELECT p FROM bern),
             provsql.as_random(-5),
             provsql.as_random(5))) AS u2;
SELECT u1 = u2                            AS structural_sharing_collapses,
       (get_children(u1))[1] = (get_children(u2))[1] AS shared_p_token
  FROM two_mix;

-- B2.  Different operands stay distinct: changing any of (p, x, y)
-- yields a different v5 hash.  Probe by swapping the x-leaf only.
CREATE TEMP TABLE two_mix_diff AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT p FROM bern),
             provsql.as_random(-5),
             provsql.as_random(5))) AS u1,
         random_variable_uuid(
           provsql.mixture(
             (SELECT p FROM bern),
             provsql.as_random(-6),
             provsql.as_random(5))) AS u2;
SELECT u1 <> u2 AS distinct_when_operands_differ
  FROM two_mix_diff;

-- C.  Compositional: x and y may themselves be mixtures.  Build a
-- mixture-of-mixtures and verify the nested structure.
CREATE TEMP TABLE bern2(p uuid);
INSERT INTO bern2 VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT p FROM bern2), 0.5);

CREATE TEMP TABLE nested_mix AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT p FROM bern),
             provsql.normal(-5, 0.5),
             provsql.mixture(
               (SELECT p FROM bern2),
               provsql.normal(5, 0.5),
               provsql.normal(10, 0.5)))) AS u;

SELECT get_gate_type(u)                                      AS outer_kind,
       get_gate_type((get_children(u))[3])                    AS outer_y_kind,
       array_length(get_children((get_children(u))[3]), 1)    AS inner_nb_children
  FROM nested_mix;

-- D.  Mixture of arith expressions: branch RVs may be gate_arith.
CREATE TEMP TABLE arith_mix AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT p FROM bern),
             provsql.normal(0, 1) + provsql.as_random(3),
             provsql.normal(0, 1) * provsql.as_random(2))) AS u;

SELECT get_gate_type(u)                              AS arith_mix_kind,
       get_gate_type((get_children(u))[2])           AS arith_mix_x_kind,
       get_gate_type((get_children(u))[3])           AS arith_mix_y_kind
  FROM arith_mix;

-- E.  Mixture of gate_value Diracs: degenerates to a discrete
-- Bernoulli over two constants.  Still a valid mixture root.
SELECT get_gate_type(random_variable_uuid(
         provsql.mixture(
           (SELECT p FROM bern),
           provsql.as_random(-1.0),
           provsql.as_random(1.0)))) AS dirac_mix_kind;

-- E2. Probability-shorthand overload mixture(double, x, y) mints an
-- anonymous gate_input and pins its probability.  The resulting root
-- gate is a gate_mixture with three wires whose first wire is a fresh
-- gate_input (different UUID for two calls -- the convenience form is
-- not designed for coupling).
CREATE TEMP TABLE adhoc_mix AS
  SELECT random_variable_uuid(
           provsql.mixture(0.4::float8,
                           provsql.normal(0, 1),
                           provsql.normal(5, 1))) AS u1,
         random_variable_uuid(
           provsql.mixture(0.4::float8,
                           provsql.normal(0, 1),
                           provsql.normal(5, 1))) AS u2;

SELECT get_gate_type(u1)                              AS adhoc_kind,
       array_length(get_children(u1), 1)              AS adhoc_nb_children,
       get_gate_type((get_children(u1))[1])           AS adhoc_wire0_kind,
       abs(get_prob((get_children(u1))[1]) - 0.4) < 1e-12
                                                      AS adhoc_prob_pinned,
       (get_children(u1))[1] <> (get_children(u2))[1] AS adhoc_distinct_bernoulli
  FROM adhoc_mix;

-- G.  Compound Boolean p is accepted.  The constructor only checks
-- that p is a Boolean gate kind (input / mulinput / update / plus /
-- times / monus / project / eq / cmp / zero / one); π for compound p
-- is computed at expectation/variance time via the probability
-- evaluator.

-- G1. p = times(b1, b2): conjunctive selector.  Two probability-0.5
-- inputs combined via provenance_times; the resulting mixture is
-- well-formed and routes through the Boolean-probability evaluator
-- when its moments are queried.
CREATE TEMP TABLE bern_g(b1 uuid, b2 uuid);
INSERT INTO bern_g VALUES (public.uuid_generate_v4(), public.uuid_generate_v4());
SELECT create_gate((SELECT b1 FROM bern_g), 'input');
SELECT create_gate((SELECT b2 FROM bern_g), 'input');
SELECT set_prob((SELECT b1 FROM bern_g), 0.5);
SELECT set_prob((SELECT b2 FROM bern_g), 0.5);
SELECT get_gate_type(random_variable_uuid(
         provsql.mixture(
           provenance_times((SELECT b1 FROM bern_g), (SELECT b2 FROM bern_g)),
           provsql.as_random(-5),
           provsql.as_random( 5)))) AS compound_times_p_kind;

-- G2. p = monus(b1, b2): b1 AND NOT b2.
SELECT get_gate_type(random_variable_uuid(
         provsql.mixture(
           provenance_monus((SELECT b1 FROM bern_g), (SELECT b2 FROM bern_g)),
           provsql.as_random(-5),
           provsql.as_random( 5)))) AS compound_monus_p_kind;

-- F.  Validation errors.  Keep VERBOSITY terse so the messages stay
-- compact and we capture them line-by-line.
\set VERBOSITY terse

-- F1. p is not a Boolean gate (here it is a gate_value from
--     as_random) -- rejected with a kind-mismatch message.
SELECT provsql.mixture(random_variable_uuid(provsql.as_random(0.5)),
                       provsql.normal(0, 1),
                       provsql.normal(0, 1));

-- F2. probability-shorthand overload: out-of-range scalar.
SELECT provsql.mixture(1.5::float8, provsql.normal(0, 1), provsql.normal(0, 1));
SELECT provsql.mixture((-0.1)::float8, provsql.normal(0, 1), provsql.normal(0, 1));
SELECT provsql.mixture('NaN'::float8, provsql.normal(0, 1), provsql.normal(0, 1));

\set VERBOSITY default
