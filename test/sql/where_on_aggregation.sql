\set ECHO none
\pset format unaligned

CREATE TABLE result_count AS SELECT
  city,
  COUNT(*) AS c
FROM personnel
GROUP BY city;

CREATE TABLE result_count2 AS SELECT
  city,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting
FROM result_count
WHERE c > 2
ORDER BY city;

SELECT remove_provenance('result_count2');
SELECT city, formula, counting
FROM result_count2;

DROP TABLE result_count2;
DROP TABLE result_count;

CREATE TABLE result_sum AS SELECT
  city,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting FROM (
    SELECT city, SUM(id) AS s FROM personnel GROUP BY city
  ) t
WHERE s > 2 AND city<>'Berlin'
ORDER BY city;

SELECT remove_provenance('result_sum');
SELECT city, formula, counting
FROM result_sum;

DROP TABLE result_sum;

-- An aggregate comparison written in an outer WHERE is migrated into this
-- query level's HAVING, and its provenance supersedes the compared group's
-- delta(+ tokens) -- but only that group's.  Every other input at this level,
-- in particular a join partner, must survive into the circuit.
CREATE TABLE woa_r(g int, v int);
INSERT INTO woa_r VALUES (1,10),(1,20),(1,30),(2,5),(2,7);
CREATE TABLE woa_s(g int, w int);
INSERT INTO woa_s VALUES (1,100),(2,200),(3,300);
SELECT add_provenance('woa_r');
SELECT add_provenance('woa_s');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM woa_r;
  PERFORM set_prob(provsql, 0.3) FROM woa_s;
END $$;

-- No comparison: delta(+ tokens) times the join partner.
-- g=1: (1-0.5^3)*0.3 = 0.2625;  g=2: (1-0.5^2)*0.3 = 0.225
CREATE TABLE woa_nocmp AS
  SELECT x.g AS g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa_r GROUP BY g) x, woa_s
  WHERE x.g = woa_s.g;
SELECT remove_provenance('woa_nocmp');
SELECT 'no comparison' AS shape, g, p FROM woa_nocmp ORDER BY g;
DROP TABLE woa_nocmp;

-- Comparison, no join partner: the cmp gate alone, the delta it supersedes
-- dropped rather than multiplied back in.  g=1: 0.5; g=2: 0.25
CREATE TABLE woa_nojoin AS
  SELECT x.g AS g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa_r GROUP BY g) x
  WHERE x.c >= 2;
SELECT remove_provenance('woa_nojoin');
SELECT 'comparison, no join' AS shape, g, p FROM woa_nojoin ORDER BY g;
DROP TABLE woa_nojoin;

-- Comparison above a join: the cmp gate AND the join partner.
-- g=1: 0.5*0.3 = 0.15;  g=2: 0.25*0.3 = 0.075
CREATE TABLE woa_join AS
  SELECT x.g AS g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa_r GROUP BY g) x, woa_s
  WHERE x.g = woa_s.g AND x.c >= 2;
SELECT remove_provenance('woa_join');
SELECT 'comparison above join' AS shape, g, p FROM woa_join ORDER BY g;
DROP TABLE woa_join;

-- Two groups compared, plus a join partner: both deltas superseded, both cmp
-- gates kept, the partner retained.  0.5 * 0.25 * 0.3 = 0.0375
CREATE TABLE woa_two AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa_r GROUP BY g) a,
       (SELECT g, count(*) AS c FROM woa_r GROUP BY g) b, woa_s
  WHERE a.g = 1 AND b.g = 2 AND woa_s.g = 1 AND a.c >= 2 AND b.c >= 2;
SELECT remove_provenance('woa_two');
SELECT 'two groups above join' AS shape, p FROM woa_two;
DROP TABLE woa_two;

DROP TABLE woa_r;
DROP TABLE woa_s;

-- The aggregate result must keep its agg_token type however many subquery
-- levels separate the aggregation from the comparison.  Retyping only one
-- level up leaves the column declared as its pre-rewrite scalar type, and
-- the comparison is then executed natively on the raw composite datum --
-- silently, with neither the right rows nor the right annotation.
CREATE TABLE woa2_r(g int, v int);
INSERT INTO woa2_r VALUES (1,10),(1,20),(1,30),(2,5),(2,7);
SELECT add_provenance('woa2_r');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM woa2_r; END $$;

-- One, two and three levels of nesting all give the same annotation:
-- g=1 -> 0.5 (at least two of three), g=2 -> 0.25 (both).
CREATE TABLE woa2_n1 AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, count(*) AS c FROM woa2_r GROUP BY g) a
  WHERE c >= 2;
SELECT remove_provenance('woa2_n1');
SELECT 'one level' AS nesting, g, p FROM woa2_n1 ORDER BY g;
DROP TABLE woa2_n1;

CREATE TABLE woa2_n2 AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT * FROM (SELECT g, count(*) AS c FROM woa2_r GROUP BY g) a) b
  WHERE c >= 2;
SELECT remove_provenance('woa2_n2');
SELECT 'two levels' AS nesting, g, p FROM woa2_n2 ORDER BY g;
DROP TABLE woa2_n2;

CREATE TABLE woa2_n3 AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT * FROM
         (SELECT * FROM (SELECT g, count(*) AS c FROM woa2_r GROUP BY g) a) b) c2
  WHERE c >= 2;
SELECT remove_provenance('woa2_n3');
SELECT 'three levels' AS nesting, g, p FROM woa2_n3 ORDER BY g;
DROP TABLE woa2_n3;

-- A nested view is the same shape through the rewriter, and an equality
-- pins the value rather than just its existence: count(*) = 3 holds only in
-- the single world where all three of group 1's rows are present (0.125),
-- and never for group 2.
CREATE VIEW woa2_v1 AS SELECT g, count(*) AS c FROM woa2_r GROUP BY g;
CREATE VIEW woa2_v2 AS SELECT * FROM woa2_v1;

CREATE TABLE woa2_eq AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM woa2_v2 WHERE c = 3;
SELECT remove_provenance('woa2_eq');
SELECT 'nested view, c = 3' AS nesting, g, p FROM woa2_eq ORDER BY g;
DROP TABLE woa2_eq;

DROP VIEW woa2_v2;
DROP VIEW woa2_v1;
DROP TABLE woa2_r;

-- A comparison spanning two different groups enumerates the union of both
-- groups' contributors jointly.  A world in which one group has no row
-- present is a world in which that group's row does not exist, so it must
-- not satisfy the comparison -- even though the other group is non-empty
-- and the world is therefore not the empty one the enumeration already
-- skips.  Getting this wrong inflates the answer by the probability that
-- one side is empty.
CREATE TABLE woa3_r(g int, v int);
INSERT INTO woa3_r VALUES (1,10),(1,20),(1,30),(2,5),(2,7);
SELECT add_provenance('woa3_r');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM woa3_r; END $$;

-- count(g=1) > count(g=2), both groups non-empty.  With A ~ Bin(3,0.5) and
-- B ~ Bin(2,0.5): P(B=1)*P(A>=2) + P(B=2)*P(A=3)
--   = 0.5*0.5 + 0.25*0.125 = 0.28125.
-- Counting the worlds where group 2 is empty would add 7/32 = 0.21875.
CREATE TABLE woa3_cnt AS
  SELECT round(probability_evaluate(provenance())::numeric, 5) AS p
  FROM (SELECT g, count(*) AS c FROM woa3_r GROUP BY g) a,
       (SELECT g, count(*) AS c FROM woa3_r GROUP BY g) b
  WHERE a.g = 1 AND b.g = 2 AND a.c > b.c;
SELECT remove_provenance('woa3_cnt');
SELECT 'count(g1) > count(g2)' AS shape, p FROM woa3_cnt;
DROP TABLE woa3_cnt;

-- sum(g=1) > sum(g=2) over {10,20,30} and {5,7}: both non-empty is
-- (7/8)*(3/4) = 0.65625, and the only non-empty pair that fails is
-- {10} vs {5,7} (10 > 12 false), probability (1/8)*(1/4) = 0.03125.
CREATE TABLE woa3_sum AS
  SELECT round(probability_evaluate(provenance())::numeric, 5) AS p
  FROM (SELECT g, sum(v) AS c FROM woa3_r GROUP BY g) a,
       (SELECT g, sum(v) AS c FROM woa3_r GROUP BY g) b
  WHERE a.g = 1 AND b.g = 2 AND a.c > b.c;
SELECT remove_provenance('woa3_sum');
SELECT 'sum(g1) > sum(g2)' AS shape, p FROM woa3_sum;
DROP TABLE woa3_sum;

DROP TABLE woa3_r;

-- What a lifted comparison supersedes is the compared group's delta, not the
-- whole row annotation it happens to sit in.  Two shapes make the difference
-- visible: the annotation may mix that delta with other factors, and it may
-- not contain a delta at all.
CREATE TABLE woa4_r(g int, v int);
INSERT INTO woa4_r VALUES (1,10),(1,20),(1,30),(2,5),(2,7);
CREATE TABLE woa4_s(g int, w int);
INSERT INTO woa4_s VALUES (1,100),(2,200),(3,300);
SELECT add_provenance('woa4_r');
SELECT add_provenance('woa4_s');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM woa4_r;
  PERFORM set_prob(provsql, 0.3) FROM woa4_s;
END $$;

-- The aggregated relation reaches the comparison's level through a subquery
-- whose row token already combines the group's delta with the join partner:
-- the join sits one level below the comparison, so the outer level sees a
-- single attribute carrying times(delta, s).  Only the delta is superseded,
-- so the partner survives: 0.5*0.3 and 0.25*0.3.  (Written as a nested
-- subquery rather than a materialised CTE, whose syntax postdates the oldest
-- PostgreSQL this suite runs on.)
CREATE TABLE woa4_mixed AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT sub.g AS g, c, w
        FROM (SELECT g, count(*) AS c FROM woa4_r GROUP BY g) sub
        JOIN woa4_s ON sub.g = woa4_s.g) j
  WHERE c >= 2;
SELECT remove_provenance('woa4_mixed');
SELECT 'delta mixed with a join partner' AS shape, g, p FROM woa4_mixed ORDER BY g;
DROP TABLE woa4_mixed;

-- A second comparison on the same group: the row token it sees is the first
-- comparison's cmp gate, which the new one does not subsume at all.  Both are
-- kept, so g=1 is P(c = 2) = 3/8 rather than P(c <= 2) = 7/8, and g=2 is
-- P(c = 2) = 1/4.
CREATE TABLE woa4_seq AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT g, c FROM (SELECT g, count(*) AS c FROM woa4_r GROUP BY g) s1
        WHERE c >= 2) s2
  WHERE c <= 2;
SELECT remove_provenance('woa4_seq');
SELECT 'comparison over a comparison' AS shape, g, p FROM woa4_seq ORDER BY g;
DROP TABLE woa4_seq;

-- The supersede is exact where it applies: with nothing but the delta to
-- shed, the annotation is the comparison alone, whichever way the query is
-- written.  Identical tokens, not merely identical probabilities.
CREATE TABLE woa4_fused AS
  SELECT g, provenance() AS tok FROM woa4_r GROUP BY g HAVING count(*) >= 2;
SELECT remove_provenance('woa4_fused');
CREATE TABLE woa4_sub AS
  SELECT g, provenance() AS tok
  FROM (SELECT g, count(*) AS c FROM woa4_r GROUP BY g) x WHERE c >= 2;
SELECT remove_provenance('woa4_sub');
SELECT 'fused token = subquery token' AS shape,
       bool_and(f.tok = u.tok) AS identical
  FROM woa4_fused f JOIN woa4_sub u ON f.g = u.g;
DROP TABLE woa4_fused;
DROP TABLE woa4_sub;

DROP TABLE woa4_r;
DROP TABLE woa4_s;
