\set ECHO none
\pset format unaligned

-- Case Study 9: A Sales Forecast Dashboard.  Twelve deals in three regions,
-- each closing independently with its win probability.  Every value below is
-- the one the case study states, checked against a brute-force count over the
-- 4096 sets of closing deals (plain SQL over each, weighted by its
-- probability).  Needs FETCH ... WITH TIES, hence PostgreSQL 13 or later.

CREATE TABLE cs9_region (name text PRIMARY KEY, target integer NOT NULL);
INSERT INTO cs9_region VALUES ('North', 150), ('South', 120), ('West', 100);
CREATE TABLE cs9_deal (
  id integer PRIMARY KEY, customer text NOT NULL, region text NOT NULL,
  quarter text NOT NULL, amount integer NOT NULL,
  win_prob double precision NOT NULL);
INSERT INTO cs9_deal VALUES
  ( 1, 'Arctis',    'North', 'Q1',  80, 0.9), ( 2, 'Borealis', 'North', 'Q1',  45, 0.5),
  ( 3, 'Fjordline', 'North', 'Q2', 120, 0.3), ( 4, 'Glacier',  'North', 'Q2',  30, 0.8),
  ( 5, 'Meridian',  'South', 'Q1',  60, 0.7), ( 6, 'Solstice', 'South', 'Q1',  25, 0.9),
  ( 7, 'Tropica',   'South', 'Q2',  90, 0.4), ( 8, 'Zenith',   'South', 'Q2',  40, 0.6),
  ( 9, 'Canyon',    'West',  'Q1',  70, 0.5), (10, 'Horizon',  'West',  'Q1',  35, 0.8),
  (11, 'Mesa',      'West',  'Q2',  55, 0.6), (12, 'Sierra',   'West',  'Q2', 150, 0.2);
SELECT add_provenance('cs9_deal');
DO $$ BEGIN PERFORM set_prob(provenance(), win_prob) FROM cs9_deal; END $$;

-- Step 2: the pipeline when every deal closes (North 275 (*)), expected revenue
-- given some deal closes (North 155.59), and the forecast counting nothing as 0
-- (North 154.50 = 80*.9 + 45*.5 + 120*.3 + 30*.8).
CREATE TABLE cs9_r AS
  SELECT region, sum(amount) AS pipeline,
         round(expected(sum(amount))::numeric, 2) AS if_any_closes,
         round(expected(coalesce(sum(amount), 0))::numeric, 2) AS forecast,
         round(avg(amount), 1) AS avg_deal
  FROM cs9_deal GROUP BY region;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;

-- Step 3: ROLLUP subtotals add up (94.50 + 60.00 = 154.50; total 405.00).
CREATE TABLE cs9_r AS
  SELECT region, quarter,
         round(expected(coalesce(sum(amount), 0))::numeric, 2) AS forecast
  FROM cs9_deal GROUP BY ROLLUP (region, quarter);
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region, quarter;
DROP TABLE cs9_r;

-- Step 4: aggregates over windows, conditioned on the row's deal closing:
-- revenue to date (Fjordline 120 + 80*.9 + 45*.5 + 30*.8 = 238.50) and each
-- deal's share of its region (Sierra 63.39); unconditioned, Sierra's share
-- averages over the worlds where it did not close too (166.87).
CREATE TABLE cs9_r AS
  SELECT customer, region, quarter, amount,
         sum(amount) OVER (PARTITION BY region ORDER BY quarter) AS to_date,
         round(expected(sum(amount) OVER (PARTITION BY region ORDER BY quarter),
                        provenance())::numeric, 2) AS expected_to_date,
         round(expected(100.0 * amount / sum(amount) OVER (PARTITION BY region),
                        provenance())::numeric, 2) AS pct_of_region,
         round(expected(100.0 * amount
                        / sum(amount) OVER (PARTITION BY region))::numeric, 2)
           AS pct_unconditioned
  FROM cs9_deal;
SELECT remove_provenance('cs9_r');
SELECT customer, region, quarter, amount, to_date, expected_to_date,
       pct_of_region, pct_unconditioned
FROM cs9_r ORDER BY region, quarter, customer;
DROP TABLE cs9_r;

-- Step 5: expected share of the total, per world (North 0.3891).
CREATE TABLE cs9_r AS
  SELECT region, round(expected(sum(amount)::numeric
                                / (SELECT sum(amount) FROM cs9_deal))::numeric, 4)
                   AS share
  FROM cs9_deal GROUP BY region;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;

-- Step 6: the top region (North 0.4516, South 0.2790, West 0.2960) and the
-- expected rank (North 1.7398, South 2.0766, West 2.0925).
CREATE TABLE cs9_r AS
  SELECT region, round(probability_evaluate(provenance())::numeric, 4) AS p_top
  FROM (SELECT region, sum(amount) AS revenue FROM cs9_deal GROUP BY region
        ORDER BY revenue DESC FETCH FIRST 1 ROW WITH TIES) t;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;
CREATE TABLE cs9_r AS
  SELECT region, rk, round(expected(rk)::numeric, 4) AS expected_rank
  FROM (SELECT region, rank() OVER (ORDER BY sum(amount) DESC) AS rk
        FROM cs9_deal GROUP BY region) t;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;

-- Step 7: on target, compared with the region's target, a grouping column
-- (North true 0.5490, false 0.4440).  The target and the amounts are NOT
-- NULL, so the comparison is never unknown, and there is no unknown row.
CREATE TABLE cs9_r AS
  SELECT d.region, sum(d.amount) >= r.target AS on_target,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM cs9_deal d JOIN cs9_region r ON r.name = d.region
  GROUP BY d.region, r.target;
SELECT remove_provenance('cs9_r');
SELECT region, coalesce(on_target::text, 'unknown') AS on_target, p
FROM cs9_r ORDER BY region, on_target;
DROP TABLE cs9_r;

-- Step 8: growth from Q1 to Q2, a difference of two aggregates (North
-- (36 + 24) - (72 + 22.5) = -34.50, South -4.50, West 0.00), and the regions
-- whose Q2 beats their Q1 in a HAVING (North 0.2580, South 0.4852, West
-- 0.3720).
CREATE TABLE cs9_r AS
  SELECT region,
         round(expected(coalesce(sum(amount) FILTER (WHERE quarter = 'Q2'), 0)
                        - coalesce(sum(amount) FILTER (WHERE quarter = 'Q1'), 0)
                       )::numeric, 2) AS growth
  FROM cs9_deal GROUP BY region;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;
CREATE TABLE cs9_r AS
  SELECT region, round(probability_evaluate(provenance())::numeric, 4) AS p_growth
  FROM cs9_deal GROUP BY region
  HAVING sum(amount) FILTER (WHERE quarter = 'Q2')
       > sum(amount) FILTER (WHERE quarter = 'Q1');
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;

-- Step 10: a big deal (>= 100) closes: bool_or, read in the worlds where the
-- region closes a deal (North 0.3 / 0.993 = 0.3021, West 0.2066), and EXISTS
-- over the untracked regions, in every world (North 0.3000, West 0.2000).
CREATE TABLE cs9_r AS
  SELECT region, bool_or(amount >= 100) AS big_deal,
         round(expected(bool_or(amount >= 100)::int)::numeric, 4) AS p_big
  FROM cs9_deal GROUP BY region;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;
CREATE TABLE cs9_r AS
  SELECT r.name,
         EXISTS (SELECT * FROM cs9_deal d
                 WHERE d.region = r.name AND d.amount >= 100) AS big_deal,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM cs9_region r;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY name, big_deal;
DROP TABLE cs9_r;

-- Step 9: a percentage with FILTER and a NULLIF divisor (North 49.35).
-- Step 11: best and average region (194.73 and 137.14).
CREATE TABLE cs9_r AS
  SELECT region,
         round(expected(100.0 * count(*) FILTER (WHERE amount >= 60)
                        / NULLIF(count(*), 0))::numeric, 2) AS pct_big
  FROM cs9_deal GROUP BY region;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;
CREATE TABLE cs9_r AS
  SELECT round(expected(max(revenue))::numeric, 2) AS best_region,
         round(expected(avg(revenue))::numeric, 2) AS average_region
  FROM (SELECT region, sum(amount) AS revenue FROM cs9_deal GROUP BY region) t;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r;
DROP TABLE cs9_r;

-- Step 12: regions grouped by the number of deals they close (2: 0.7629).
CREATE TABLE cs9_r AS
  SELECT n AS deals_closed, round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT region, count(*) AS n FROM cs9_deal GROUP BY region) t
  GROUP BY n;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY deals_closed;
DROP TABLE cs9_r;

-- Step 13: the largest deal of each region (Arctis 0.9 * (1 - 0.3) = 0.63).
CREATE TABLE cs9_r AS
  SELECT region, customer, amount,
         round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT DISTINCT ON (region) region, customer, amount
        FROM cs9_deal ORDER BY region, amount DESC) t;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region, amount DESC;
DROP TABLE cs9_r;

-- Step 14: the standard deviation when every deal closes (North 40.1 (*)), and
-- its expectation over the worlds with two deals or more (North 33.31).
CREATE TABLE cs9_r AS
  SELECT region, round(stddev(amount), 1) AS sd,
         round(expected(stddev(amount))::numeric, 2) AS expected_sd
  FROM cs9_deal GROUP BY region;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;

-- Step 15: a label reads the value on the data as it is, with a warning;
-- plain() says it is meant, and the warning goes away.
CREATE TABLE cs9_r AS
  SELECT region, sum(amount)::text || ' k€' AS label
  FROM cs9_deal GROUP BY region;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;
CREATE TABLE cs9_r AS
  SELECT region, plain(sum(amount))::text || ' k€' AS label
  FROM cs9_deal GROUP BY region;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;

-- Step 15 (continued): with provsql.implicit_freeze = 'error', the label is
-- refused, and the plain() one still runs.
SET provsql.implicit_freeze = 'error';
SELECT region, sum(amount)::text || ' k€' AS label FROM cs9_deal GROUP BY region;
CREATE TABLE cs9_r AS
  SELECT region, plain(sum(amount))::text || ' k€' AS label
  FROM cs9_deal GROUP BY region;
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY region;
DROP TABLE cs9_r;
RESET provsql.implicit_freeze;

-- Step 16: the regions with no big deal (>= 100), for each purpose.  South:
-- every purpose.  North: analytics and marketing only, because Fjordline's
-- deal (120, forecasting only) is hidden from them; over all deals it is
-- there, so North is not consented for marketing and both purposes conflict.
-- West: Sierra (150) is consented for everything, no purpose.
CREATE TYPE cs9_purpose AS ENUM ('forecasting', 'analytics', 'marketing');
CREATE TABLE cs9_consent (customer text PRIMARY KEY, purposes cs9_purpose[] NOT NULL);
INSERT INTO cs9_consent VALUES
  ('Arctis', '{forecasting,analytics,marketing}'), ('Borealis', '{forecasting,analytics}'),
  ('Fjordline', '{forecasting}'), ('Glacier', '{forecasting,analytics,marketing}'),
  ('Meridian', '{forecasting,analytics,marketing}'), ('Solstice', '{forecasting}'),
  ('Tropica', '{forecasting,analytics,marketing}'), ('Zenith', '{forecasting,analytics,marketing}'),
  ('Canyon', '{forecasting,analytics,marketing}'), ('Horizon', '{forecasting,analytics}'),
  ('Mesa', '{forecasting,analytics,marketing}'), ('Sierra', '{forecasting,analytics,marketing}');
CREATE TABLE cs9_deal_consent AS
  SELECT d.provsql AS provenance, c.purposes AS value
  FROM cs9_deal d JOIN cs9_consent c USING (customer);
CREATE TABLE cs9_r AS
  SELECT r.name,
         (sr_consent(provenance(), 'cs9_deal_consent', 'forecasting'::cs9_purpose)).*,
         consented_for(provenance(), 'cs9_deal_consent', 'marketing') AS for_marketing,
         consent_conflicts(provenance(), 'cs9_deal_consent',
                           'forecasting'::cs9_purpose) AS conflicts,
         consent_purposes(provenance(), 'cs9_deal_consent',
                          'forecasting'::cs9_purpose) AS allowed
  FROM cs9_region r
  WHERE NOT EXISTS (SELECT * FROM cs9_deal d
                    WHERE d.region = r.name AND d.amount >= 100);
SELECT remove_provenance('cs9_r');
SELECT * FROM cs9_r ORDER BY name;
DROP TABLE cs9_r, cs9_deal_consent, cs9_consent;
DROP TYPE cs9_purpose;

SELECT remove_provenance('cs9_deal');
DROP TABLE cs9_deal;
DROP TABLE cs9_region;
