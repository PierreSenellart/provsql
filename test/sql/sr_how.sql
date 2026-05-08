\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Mirrors the sr_why test: cross-join of personnel with id1 < id2,
-- grouped by city. Each city's polynomial is a sum of length-2
-- monomials (one per pair).
CREATE TABLE result_how AS SELECT
  p1.city,
  sr_how(provenance(), 'personnel_name') AS how
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_how');
SELECT city, how
FROM result_how;

DROP TABLE result_how;

-- Canonicalisation showcase: a full self-cross-join makes squared
-- monomials (X^2) and coefficients > 1 visible. (Magdalen,Dave) and
-- (Dave,Magdalen) collapse to the same canonical monomial Dave*Magdalen,
-- so its coefficient is 2.
CREATE TABLE result_how_self AS SELECT
  p1.city,
  sr_how(provenance(), 'personnel_name') AS how
FROM personnel p1, personnel p2
WHERE p1.city = p2.city
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_how_self');
SELECT city, how
FROM result_how_self;

DROP TABLE result_how_self;

-- Round-trip: each leaf is already a polynomial (2⋅name); times
-- multiplies coefficients, so each cross-pair contributes a monomial
-- with coefficient 4.
SELECT create_provenance_mapping('personnel_poly',
  'personnel',
  $$'2⋅' || name$$);

CREATE TABLE result_how_poly AS SELECT
  p1.city,
  sr_how(provenance(), 'personnel_poly') AS how
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_how_poly');
SELECT city, how
FROM result_how_poly;

DROP TABLE result_how_poly;
