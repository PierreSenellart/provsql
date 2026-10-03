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
