\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TEMP TABLE p(t uuid);
INSERT INTO p VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p), 0.5);

-- A.  Two disjoint uniform supports merge into the spanning interval.
--     U(0,1) ∪ U(5,10) → (0, 10).
SELECT lo, hi
  FROM provsql.rv_support((
         provsql.mixture(
           (SELECT t FROM p),
           provsql.uniform(0, 1),
           provsql.uniform(5, 10)))::uuid);

-- B.  Normal + Uniform: normal has unbounded support, so the union
--     is all real numbers.  -Infinity / Infinity round-trip via float8.
SELECT lo, hi
  FROM provsql.rv_support((
         provsql.mixture(
           (SELECT t FROM p),
           provsql.normal(0, 1),
           provsql.uniform(5, 10)))::uuid);

-- C.  Two exponentials (both supported on [0, +∞)): union is the
--     same half-line, hi = +Infinity.
SELECT lo, hi
  FROM provsql.rv_support((
         provsql.mixture(
           (SELECT t FROM p),
           provsql.exponential(1),
           provsql.exponential(2)))::uuid);

-- D.  Nested mixture: support is union of A, B, C below.
--     A = U(0,1)         → (0,1)
--     B = U(20,30)       → (20,30)
--     C = U(-5,-1)       → (-5,-1)
--     Union → (-5, 30).
CREATE TEMP TABLE p1(t uuid);
INSERT INTO p1 VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p1), 0.4);
CREATE TEMP TABLE p2(t uuid);
INSERT INTO p2 VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p2), 0.7);

SELECT lo, hi
  FROM provsql.rv_support((provsql.mixture(
           (SELECT t FROM p1)::uuid,
           provsql.uniform(0, 1),
           provsql.mixture(
             (SELECT t FROM p2),
             provsql.uniform(20, 30),
             provsql.uniform(-5, -1)))));

-- E.  Mixture of constants: support is the pointwise hull
--     {-1.5, 7.25} → (-1.5, 7.25).
SELECT lo, hi
  FROM provsql.rv_support((
         provsql.mixture(
           (SELECT t FROM p),
           provsql.as_random(-1.5),
           provsql.as_random( 7.25)))::uuid);
