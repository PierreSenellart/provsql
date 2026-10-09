\set ECHO none
\pset format unaligned

-- Case Study 3, Part 2, on a miniature network (the real GTFS data is a
-- separate download): journeys with changes as a WITH RECURSIVE query over
-- the hops of the network, evaluated in several semirings.  Every hop runs
-- both ways, so the stations are derived through cycles.
--   A -B- (line 1, rail, 2 min, accessible)      B -C- (line 1, rail, 3 min)
--   A -C- (line 2, bus, 7 min, accessible)       C -D- (line 2, bus, 4 min)
-- From A: B in 2 min, C in 5 (through B), D in 9; an accessible journey to B
-- and C (A -> C directly), none to D.  With rail hops at 0.95 and bus hops at
-- 0.9, P(D) = P(C) * 0.9, P(C) = 1 - 0.1 * (1 - 0.95^2) = 0.99025, so
-- P(D) = 0.891225.

CREATE TABLE cs3_hop(from_station text, to_station text, line text,
                     route_type int, minutes float8, accessible int);
INSERT INTO cs3_hop VALUES
  ('A', 'B', '1', 2, 2, 1), ('B', 'A', '1', 2, 2, 1),
  ('B', 'C', '1', 2, 3, 0), ('C', 'B', '1', 2, 3, 0),
  ('A', 'C', '2', 3, 7, 1), ('C', 'A', '2', 3, 7, 1),
  ('C', 'D', '2', 3, 4, 0), ('D', 'C', '2', 3, 4, 0);
SELECT add_provenance('cs3_hop');
SELECT create_provenance_mapping('cs3_hop_minutes', 'cs3_hop', 'minutes');
SELECT create_provenance_mapping('cs3_hop_access', 'cs3_hop', 'accessible');
SELECT create_provenance_mapping('cs3_hop_one', 'cs3_hop', '1');
SELECT create_provenance_mapping('cs3_hop_name', 'cs3_hop',
                                 'from_station || ''->'' || to_station');
DO $$ BEGIN
  PERFORM set_prob(provenance(), CASE WHEN route_type = 3 THEN 0.9 ELSE 0.95 END)
  FROM cs3_hop;
END $$;

CREATE TABLE cs3_reach AS
  WITH RECURSIVE reach(station) AS (
      SELECT 'A'::text
    UNION
      SELECT h.to_station FROM cs3_hop h JOIN reach r ON h.from_station = r.station)
  SELECT station, provenance() AS tok FROM reach;
SELECT remove_provenance('cs3_reach');

-- Shortest travel time (Dijkstra, nonnegative tropical; the same by Gaussian
-- elimination without nonnegative) and accessible journeys (Boolean).
SELECT station,
       sr_tropical(tok, 'cs3_hop_minutes', nonnegative => true) AS minutes,
       sr_tropical(tok, 'cs3_hop_minutes') AS minutes_elimination,
       sr_boolean(tok, 'cs3_hop_access') AS accessible
FROM cs3_reach ORDER BY station;

-- Counting has no answer: infinitely many derivations.
SELECT sr_counting(tok, 'cs3_hop_one') FROM cs3_reach WHERE station = 'B';

-- The equations of B.
SELECT sr_formula(tok, 'cs3_hop_name') FROM cs3_reach WHERE station = 'B';

-- Probabilities: exact on this small network, and sampled on the equations
-- within the tolerance asked.
SET provsql.monte_carlo_seed = 1;
SELECT station,
       round(probability_evaluate(tok)::numeric, 6) AS exact,
       abs(probability_evaluate(tok, 'additive', 'eps=0.01,delta=0.01')
           - probability_evaluate(tok)) < 0.01 AS additive_ok,
       abs(probability_evaluate(tok, 'monte-carlo', '20000')
           - probability_evaluate(tok)) < 0.02 AS monte_carlo_ok
FROM cs3_reach WHERE station <> 'A' ORDER BY station;
RESET provsql.monte_carlo_seed;

DROP TABLE cs3_reach, cs3_hop_minutes, cs3_hop_access, cs3_hop_one,
           cs3_hop_name;
SELECT remove_provenance('cs3_hop');
DROP TABLE cs3_hop;
