\set ECHO none
\set SHOW_CONTEXT never
\pset format unaligned

CREATE TABLE expected_result AS
SELECT city, expected(COUNT(*)) AS c1, expected(COUNT(id)) AS c2, expected(SUM(id)) AS s, expected(MIN(id)) AS min, expected(MAX(id)) as max
FROM personnel
GROUP BY CITY;

SELECT remove_provenance('expected_result');

SELECT city, ROUND(c1::numeric,2) AS c1, ROUND(c2::numeric,2) AS c2, ROUND(s::numeric,2) AS s, ROUND(min::numeric,2) AS min, ROUND(max::numeric,2) AS max
FROM expected_result
ORDER BY city;

DROP TABLE expected_result;

CREATE TABLE expected_result AS
SELECT city, expected(COUNT(*),provenance()) AS c1, expected(COUNT(id),provenance()) AS c2, expected(SUM(id),provenance()) AS s, expected(MIN(id),provenance()) AS min, expected(MAX(id),provenance()) as max
FROM personnel
GROUP BY CITY;

SELECT remove_provenance('expected_result');

SELECT city, ROUND(c1::numeric,2) AS c1, ROUND(c2::numeric,2) AS c2, ROUND(s::numeric,2) AS s, ROUND(min::numeric,2) AS min, ROUND(max::numeric,2) AS max
FROM expected_result
ORDER BY city;

DROP TABLE expected_result;

-- Non-supported
SELECT expected('toto'::text) FROM personnel;

-- AVG moments are supported: the exact independent-rows arm computes
-- E[AVG | at least one row present] from the joint (sum, count)
-- distribution, with no sampling.  Materialise + remove_provenance to
-- keep the run-dependent group token out of the output.
CREATE TABLE expected_result AS SELECT expected(AVG(id)) AS a FROM personnel;
SELECT remove_provenance('expected_result');
SELECT ROUND(a::numeric, 6) AS avg_expected FROM expected_result;
DROP TABLE expected_result;

-- A value defined in no world of positive probability (the group's only row
-- has probability 0) has no expectation: NULL, as SQL gives for an aggregate
-- over no row, whether expected() takes the aggregate path (sum, min, count)
-- or the scalar one (avg, arithmetic over aggregates, a window avg).
CREATE TABLE expected_undefined(g int, x int);
INSERT INTO expected_undefined VALUES (1, 10);
SELECT add_provenance('expected_undefined');
DO $$ BEGIN PERFORM set_prob(provenance(), 0) FROM expected_undefined; END $$;
CREATE TABLE expected_result AS
SELECT g, expected(sum(x)) AS e_sum, expected(min(x)) AS e_min,
       expected(count(x)) AS e_count, expected(avg(x)) AS e_avg,
       expected(sum(x) / 2) AS e_arith
FROM expected_undefined GROUP BY g;
SELECT remove_provenance('expected_result');
SELECT * FROM expected_result;
DROP TABLE expected_result;
CREATE TABLE expected_result AS
SELECT x, expected(avg(x) OVER ()) AS e_window_avg,
       expected(sum(x) OVER ()) AS e_window_sum
FROM expected_undefined;
SELECT remove_provenance('expected_result');
SELECT * FROM expected_result;
DROP TABLE expected_result;
DROP TABLE expected_undefined;

-- A NaN in the data is a value, not a world without one: it propagates, as
-- in SQL (max - min over (1, NaN) is NaN), where a NULL row value is skipped
-- (max - min over (1, NULL, 3) is 2).  Both through the scalar path.
CREATE TABLE expected_nan(g int, x float8);
INSERT INTO expected_nan VALUES (1, 1), (1, 'NaN'), (2, 'NaN'),
  (3, 1), (3, NULL), (3, 3);
SELECT add_provenance('expected_nan');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM expected_nan; END $$;
CREATE TABLE expected_result AS
SELECT g, expected(max(x) - min(x)) AS e_range, expected(avg(x) + 1) AS e_avg1
FROM expected_nan GROUP BY g;
SELECT remove_provenance('expected_result');
SELECT g, ROUND(e_range::numeric, 4) AS e_range, ROUND(e_avg1::numeric, 4) AS e_avg1
FROM expected_result ORDER BY g;
DROP TABLE expected_result;
DROP TABLE expected_nan;

-- A product of two aggregates over the same rows is not a product of
-- independent factors: an aggregate's dependence on the rows was not seen
-- (an empty footprint), and the expectation was E[factor] * E[sum].  By the
-- worlds: the factor is 1 only where the table is empty, where the sum has
-- no value, so 0; and count(*) * sum(num) is 24/8 over the 7/8 of worlds
-- with a row, 3.428571.
CREATE TABLE expected_prod(num numeric);
INSERT INTO expected_prod VALUES (1), (2), (0);
SELECT add_provenance('expected_prod');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM expected_prod; END $$;
CREATE TABLE expected_result AS
SELECT expected((CASE (SELECT count(t1.num) FROM expected_prod t1)
                   WHEN 0 THEN 1 ELSE 0 END) * sum(t2.num)) AS e_factor,
       (SELECT expected(count(*) * sum(num)) FROM expected_prod) AS e_prod
FROM expected_prod t2;
SELECT remove_provenance('expected_result');
SELECT ROUND(e_factor::numeric, 6) AS e_factor, ROUND(e_prod::numeric, 6) AS e_prod
FROM expected_result;
DROP TABLE expected_result;
SELECT remove_provenance('expected_prod');
DROP TABLE expected_prod;

-- The first two moments of a SUM or COUNT over rows split into independence
-- components (agg_row_components): one probability per row, and one per pair
-- within a component, instead of one per pair of rows.  Checked against the
-- enumeration of every pair, which conditioning on a certain event forces:
-- independent rows (one component each), rows sharing inputs through a
-- self-join (one component), and the alternatives of a block-independent
-- table (one component per block).
CREATE TABLE evi_certain(x int);
INSERT INTO evi_certain VALUES (1);
SELECT add_provenance('evi_certain');
DO $$ BEGIN PERFORM set_prob(provenance(), 1) FROM evi_certain; END $$;
CREATE TABLE evi_ind(g int, v int);
INSERT INTO evi_ind VALUES (1,1), (1,2), (1,5), (1,7);
SELECT add_provenance('evi_ind');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.1 * v) FROM evi_ind; END $$;
CREATE TABLE evi_bid(k int, v int, p float);
INSERT INTO evi_bid VALUES (1,3,0.3), (1,4,0.5), (2,6,0.4), (2,1,0.4), (3,2,0.9);
SELECT repair_key('evi_bid', 'k');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM evi_bid; END $$;
CREATE TABLE evi_r_ind AS SELECT count(*) AS c, sum(v) AS s FROM evi_ind GROUP BY g;
CREATE TABLE evi_r_self AS
  SELECT sum(a.v + b.v) AS s FROM evi_ind a, evi_ind b WHERE a.v <= b.v;
CREATE TABLE evi_r_bid AS SELECT count(*) AS c, sum(v) AS s FROM evi_bid;
SELECT remove_provenance('evi_r_ind');
SELECT remove_provenance('evi_r_self');
SELECT remove_provenance('evi_r_bid');
SET provsql.active = off;
SELECT provsql AS evi_one FROM evi_certain \gset
SELECT 'independent' AS shape, agg_row_components(s::uuid) AS components,
       round(variance(s)::numeric, 6) AS var_sum,
       round(variance(s)::numeric, 9) = round(variance(s, :'evi_one'::uuid)::numeric, 9) AS same_sum,
       round(variance(c)::numeric, 9) = round(variance(c, :'evi_one'::uuid)::numeric, 9) AS same_count
  FROM evi_r_ind
UNION ALL
SELECT 'self-join', agg_row_components(s::uuid),
       round(variance(s)::numeric, 6),
       round(variance(s)::numeric, 9) = round(variance(s, :'evi_one'::uuid)::numeric, 9),
       NULL
  FROM evi_r_self
UNION ALL
SELECT 'blocks', agg_row_components(s::uuid),
       round(variance(s)::numeric, 6),
       round(variance(s)::numeric, 9) = round(variance(s, :'evi_one'::uuid)::numeric, 9),
       round(variance(c)::numeric, 9) = round(variance(c, :'evi_one'::uuid)::numeric, 9)
  FROM evi_r_bid;
SET provsql.active = on;
DROP TABLE evi_r_ind, evi_r_self, evi_r_bid;
SELECT remove_provenance('evi_bid');
SELECT remove_provenance('evi_ind');
SELECT remove_provenance('evi_certain');
DROP TABLE evi_bid, evi_ind, evi_certain;

-- A COUNT of comparisons over random variables, each row with latent
-- variables of its own: every row is a component, so the variance takes one
-- probability per row (it took one Monte Carlo estimate per pair).  Rows
-- sharing a latent share a component.
SET provsql.monte_carlo_seed = 1;
CREATE TABLE evi_rows(id int, a float);
INSERT INTO evi_rows SELECT g, (g % 3) - 1 FROM generate_series(1, 6) g;
CREATE TABLE evi_shared(b random_variable);
INSERT INTO evi_shared VALUES (normal(0, 1));
CREATE TABLE evi_ev AS
  SELECT r.id, r.a, normal(0, 1) AS x,
         CASE WHEN r.id <= 2 THEN s.b ELSE normal(0, 1) END AS b
  FROM evi_rows r, evi_shared s;
SELECT add_provenance('evi_ev');
CREATE TABLE evi_cnt AS SELECT count(*) AS c FROM evi_ev WHERE x < a + b;
SELECT remove_provenance('evi_cnt');
SELECT agg_row_components(c::uuid) AS components FROM evi_cnt;
DROP TABLE evi_cnt;
SELECT remove_provenance('evi_ev');
DROP TABLE evi_ev, evi_shared, evi_rows;
RESET provsql.monte_carlo_seed;
