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

-- B.  Two calls to mixture(...) with the same operands mint distinct
-- gate UUIDs (VOLATILE, like the rest of the RV constructors), but
-- the Bernoulli token slot is shared verbatim.
CREATE TEMP TABLE two_mix AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT p FROM bern),
             provsql.normal(0, 1),
             provsql.normal(0, 1))) AS u1,
         random_variable_uuid(
           provsql.mixture(
             (SELECT p FROM bern),
             provsql.normal(0, 1),
             provsql.normal(0, 1))) AS u2;
SELECT u1 <> u2                            AS distinct_gates,
       (get_children(u1))[1] = (get_children(u2))[1] AS shared_p_token
  FROM two_mix;

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

-- F.  Validation errors.  Keep VERBOSITY terse so the messages stay
-- compact and we capture them line-by-line.
\set VERBOSITY terse

-- F1. p_token is not a gate_input (here it is a gate_value from
--     as_random) -- rejected with a kind-mismatch message.
SELECT provsql.mixture(random_variable_uuid(provsql.as_random(0.5)),
                       provsql.normal(0, 1),
                       provsql.normal(0, 1));

-- F2. p_token has a probability outside [0,1].  Fresh gate_inputs
--     default to prob = 1.0 (a deterministic always-X mixture), so the
--     [0,1] guard only fires when set_prob explicitly sets a bad value.
CREATE TEMP TABLE bern_bad(p uuid);
INSERT INTO bern_bad VALUES (public.uuid_generate_v4());
SELECT create_gate((SELECT p FROM bern_bad), 'input');
SELECT set_prob((SELECT p FROM bern_bad), 1.5);
SELECT provsql.mixture((SELECT p FROM bern_bad),
                       provsql.normal(0, 1),
                       provsql.normal(0, 1));

\set VERBOSITY default
