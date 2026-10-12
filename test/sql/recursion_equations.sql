\set ECHO none
\pset format unaligned

-- A recursive query is recorded as one equation per derived row.  A row
-- derived through no cycle gets the ordinary circuit of its derivations; the
-- rows of a cycle get fixpoint gates over their equations, which the semiring
-- evaluating them solves, each by the method its properties allow:
-- Dijkstra (absorptive and selective), value iteration (absorptive, exact
-- equality), a bounded number of rounds (absorptive, circuit-valued: the
-- Boolean circuit behind probabilities), Gaussian elimination with star (not
-- absorptive, with a star), the equations themselves (sr_formula), or a
-- refusal.
--
-- The graph, every edge at probability 0.5:
--   1 -a-> 2 -b-> 3 -c-> 1   (a cycle),   1 -e-> 3,   3 -d-> 4
-- and the recursion starts from the edges out of node 1, so that node 1 is
-- reached only around the cycle.  Nodes 1, 2 and 3 are derived through the
-- cycle, node 4 only reads it.
--   reach(2) = a                     (entered only through a)
--   reach(3) = a b + e
--   reach(1) = (a b + e) c
--   reach(4) = (a b + e) d

CREATE TABLE eq_e(src int, dst int, lbl text, p float8, cost float8,
                  neg float8, iv int4multirange);
INSERT INTO eq_e VALUES
  (1, 2, 'a', 0.5,  3,   3, '{[1,10)}'),
  (2, 3, 'b', 0.5,  4,   4, '{[5,20)}'),
  (3, 1, 'c', 0.5,  1, -20, '{[0,7)}'),
  (3, 4, 'd', 0.5,  5,   5, '{[8,9)}'),
  (1, 3, 'e', 0.5, 10,  10, '{[12,15)}');
SELECT add_provenance('eq_e');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM eq_e; END $$;
SELECT create_provenance_mapping('eq_lbl', 'eq_e', 'lbl');
SELECT create_provenance_mapping('eq_cost', 'eq_e', 'cost');
SELECT create_provenance_mapping('eq_neg', 'eq_e', 'neg');
SELECT create_provenance_mapping('eq_p', 'eq_e', 'p');
SELECT create_provenance_mapping('eq_iv', 'eq_e', 'iv');
SELECT create_provenance_mapping('eq_one', 'eq_e', '1');

CREATE TABLE eq_r AS
  WITH RECURSIVE reach(n) AS (
      SELECT dst FROM eq_e WHERE src = 1
    UNION
      SELECT e.dst FROM eq_e e JOIN reach r ON e.src = r.n)
  SELECT n, provenance() AS tok FROM reach;
SELECT remove_provenance('eq_r');

-- The rows of the cycle are fixpoints; node 4 reads them through an ordinary
-- product.
SELECT n, get_gate_type(tok) AS root FROM eq_r ORDER BY n;

-- Absorptive and selective, by Dijkstra.  Minimal cost:
--   2: a = 3;  3: min(a+b, e) = 7;  1: 7+c = 8;  4: 7+d = 12.
-- Most likely derivation:
--   2: .5;  3: max(.25, .5) = .5;  1: .25;  4: .25.
SELECT n, sr_tropical(tok, 'eq_cost', nonnegative => true) AS min_cost,
       sr_viterbi(tok, 'eq_p') AS viterbi,
       sr_boolean(tok, 'eq_lbl') AS reachable
FROM eq_r ORDER BY n;

-- Absorptive, circuit-valued: probabilities, through a bounded number of
-- rounds over the cycle.
--   2: P(a) = .5;  3: P(ab + e) = 1 - .75 * .5 = .625;
--   1 and 4: .625 * .5 = .3125.
SELECT n, round(probability_evaluate(tok)::numeric, 6) AS prob
FROM eq_r ORDER BY n;

-- Sampled on the equation systems themselves, without the Boolean expansion:
-- Monte Carlo and the stopping rule solve the recursion in each sampled world,
-- and agree with the exact values above within their tolerances, as does an
-- 'additive' request.
SET provsql.monte_carlo_seed = 1;
SELECT n,
       abs(probability_evaluate(tok, 'monte-carlo', '20000') - exact) < 0.02
         AS monte_carlo,
       abs(probability_evaluate(tok, 'stopping-rule', 'eps=0.05,delta=0.01')
           / exact - 1) < 0.1 AS stopping_rule,
       abs(probability_evaluate(tok, 'additive', 'eps=0.02,delta=0.01') - exact)
         < 0.02 AS additive
FROM eq_r JOIN (VALUES (1, 0.3125), (2, 0.5), (3, 0.625), (4, 0.3125))
               AS v(n, exact) USING (n)
ORDER BY n;
RESET provsql.monte_carlo_seed;

-- Absorptive, with an exact equality: validity intervals, by value
-- iteration.  a = [1,10), b = [5,20), c = [0,7), d = [8,9), e = [12,15):
--   2: a = [1,10);  3: (a ∩ b) ∪ e = [5,10) ∪ [12,15);
--   1: that ∩ c = [5,7);  4: that ∩ d = [8,9).
SELECT n, sr_interval_int(tok, 'eq_iv') AS validity FROM eq_r ORDER BY n;

-- Not absorptive, with a star, by Gaussian elimination.  With c = -20 the
-- cycle a b c costs -13: no derivation is cheapest, every row reads -∞.
SELECT n, sr_tropical(tok, 'eq_neg') AS min_cost_negative FROM eq_r ORDER BY n;
-- Why-provenance is finite on a cycle: the witnesses of node 2 are exactly
-- {a}, {a,b,c}, {a,c,e} and {a,b,c,e} (2 is entered only through a, and
-- using b or e forces a return to 1 through c).  Which-provenance is their
-- union.
SELECT n, sr_why(tok, 'eq_lbl') AS why, sr_which(tok, 'eq_lbl') AS which
FROM eq_r WHERE n = 2;
-- Counting: every row has infinitely many derivations, which an integer
-- count cannot hold.  A deliberate refusal: SQLSTATE 0A000, tagged.
DO $$
DECLARE r record; c text; d text;
BEGIN
  FOR r IN SELECT n, tok FROM eq_r ORDER BY n LOOP
    BEGIN
      c := sr_counting(r.tok, 'eq_one')::text;
    EXCEPTION WHEN OTHERS THEN
      GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL;
      c := 'count refused, ' || SQLSTATE || ' / ' || coalesce(d, 'untagged');
    END;
    RAISE NOTICE 'node %: %', r.n, c;
  END LOOP;
END $$;
-- How-provenance (formal power series) has no finite value: refused, tagged.
DO $$
DECLARE d text;
BEGIN
  PERFORM sr_how(tok, 'eq_lbl') FROM eq_r WHERE n = 2;
  RAISE NOTICE 'how-provenance was not refused';
EXCEPTION WHEN OTHERS THEN
  GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL;
  RAISE NOTICE 'how-provenance refused, % / %', SQLSTATE, coalesce(d, 'untagged');
END $$;

-- The equations themselves: three unknowns, one per row of the cycle, and
-- node 4 as an expression over them.  (The numbering of the unknowns follows
-- the order of the rows, which the plan decides: only the shape is shown.)
SELECT n, regexp_replace(f, '^(.*) where .*$', '\1') LIKE '%x%' AS reads_unknowns,
       array_length(regexp_split_to_array(regexp_replace(f, '^.* where ', ''), ', x'), 1) AS equations
FROM (SELECT n, sr_formula(tok, 'eq_lbl') AS f FROM eq_r) s ORDER BY n;

-- One statement evaluating every row solves each system once, and agrees with
-- evaluating the rows one by one (the per-statement cache of solutions).
CREATE TABLE eq_each AS
  SELECT n, sr_tropical(tok, 'eq_cost', nonnegative => true) AS c FROM eq_r;
SELECT count(*) AS rows_agreeing
FROM (WITH RECURSIVE reach(n) AS (
          SELECT dst FROM eq_e WHERE src = 1
        UNION
          SELECT e.dst FROM eq_e e JOIN reach r ON e.src = r.n)
      SELECT n, sr_tropical(provenance(), 'eq_cost', nonnegative => true) AS c
      FROM reach) s
  JOIN eq_each USING (n, c);
DROP TABLE eq_each;

-- PROV-XML names the wires of a system and of a fixpoint: a fixpoint reads
-- one component; the system pairs each unknown with its equation.
SELECT count(*) FILTER (WHERE l LIKE 'component %') AS components,
       count(*) FILTER (WHERE l LIKE 'unknown %') AS unknowns,
       count(*) FILTER (WHERE l LIKE 'equation %') AS equations
FROM (SELECT (regexp_matches(to_provxml(tok), '<prov:label>([^<]*)</prov:label>', 'g'))[1] AS l
      FROM eq_r WHERE n = 2) s;

-- Where-provenance does not reach through a cycle: a tagged refusal.
DO $$
DECLARE d text;
BEGIN
  PERFORM where_provenance(tok) FROM eq_r WHERE n = 2;
EXCEPTION WHEN feature_not_supported THEN
  GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL;
  RAISE NOTICE '%', d;
END $$;

DROP TABLE eq_r, eq_lbl, eq_cost, eq_neg, eq_p, eq_iv, eq_one;
SELECT remove_provenance('eq_e');
DROP TABLE eq_e;

-- A recursion over an outer join derives, in some worlds, rows absent from
-- the database as it is: here the null-padded row of node 2 when its id is
-- missing, through which node 3 is still reached.  Those rows are tuples of
-- the recursion as much as the others.  With ids 22 (node 2) and 33 (node 3)
-- each present at probability 0.5, the row of node 3 holds whenever 33 does:
-- 0.5, not 0.25, which would wrongly require 22 as well.  (The links are
-- certain; both tables are tracked, an outer join with the null-padded side
-- alone tracked being one ProvSQL does not lower.)
CREATE TABLE eq_link(hid int, parent_hid int);
INSERT INTO eq_link VALUES (2, 1), (3, 2);
CREATE TABLE eq_ids(id int, node int);
INSERT INTO eq_ids VALUES (22, 2), (33, 3);
SELECT add_provenance('eq_link');
SELECT add_provenance('eq_ids');
DO $$ BEGIN PERFORM set_prob(provenance(), 1) FROM eq_link; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eq_ids; END $$;
CREATE TABLE eq_paths AS
  WITH RECURSIVE p AS (
      SELECT id, hid, parent_hid || '->' || hid AS path FROM l WHERE parent_hid = 1
    UNION
      SELECT t.id, t.hid, s.path || '->' || t.hid
      FROM l t JOIN p s ON s.hid = t.parent_hid),
  l AS (SELECT id, hid, parent_hid FROM eq_link LEFT JOIN eq_ids ON node = hid)
  SELECT id, hid, path, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM p WHERE id IS NOT NULL;
SELECT remove_provenance('eq_paths');
SELECT * FROM eq_paths ORDER BY hid;
DROP TABLE eq_paths;
SELECT remove_provenance('eq_link');
SELECT remove_provenance('eq_ids');
DROP TABLE eq_link, eq_ids;
