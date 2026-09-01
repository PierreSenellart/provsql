\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- HAVING MIN / MAX against a constant, single-scan closed form.
--
-- In an absorptive m-semiring whose ⊗ distributes over ⊖ (Boolean,
-- tropical, Viterbi, Łukasiewicz, interval-union; the Boolean-circuit
-- construction behind probability_evaluate when it is not certifying),
-- the possible-world provenance of MIN(a) op C / MAX(a) op C is computed
-- in one scan of the group from the ⊕-sums L, L', G, G', E of the
-- contributors whose value is <, <=, >=, >, = C:
--   MIN <  C : L               MIN >= C : (𝟙 ⊖ L)  ⊗ G
--   MIN <= C : L'              MIN >  C : (𝟙 ⊖ L') ⊗ G'
--   MIN =  C : (𝟙 ⊖ L) ⊗ E     MIN <> C : L ⊕ (𝟙 ⊖ L') ⊗ G'
-- (MAX: exchange < and >).  Other semirings keep the exhaustive
-- possible-worlds enumeration.
--
-- Group 1 holds a:1, b:3, c:5 and group 2 holds d:4; every comparison
-- is against 3.
-- ----------------------------------------------------------------------

CREATE TABLE mms_plain(g int, v int, name text, cost float, w float);
INSERT INTO mms_plain VALUES
  (1, 1, 'a', 1, 0.5), (1, 3, 'b', 2, 0.4), (1, 5, 'c', 3, 0.8),
  (2, 4, 'd', 4, 0.6);
CREATE TABLE mms AS SELECT * FROM mms_plain;
SELECT add_provenance('mms');
SELECT create_provenance_mapping('mms_name', 'mms', 'name');
SELECT create_provenance_mapping('mms_cost', 'mms', 'cost');
SELECT create_provenance_mapping('mms_w', 'mms', 'w');

-- (0) The enumeration, for reference: sr_formula is not absorptive, so
--     it keeps the possible-worlds form the closed form must agree with.
CREATE TABLE mms_ref AS
  SELECT g, sr_formula(provenance(), 'mms_name') AS formula
  FROM mms GROUP BY g HAVING min(v) >= 3;
SELECT remove_provenance('mms_ref');
SELECT 'min >= 3' AS shape, g, formula FROM mms_ref ORDER BY g;
DROP TABLE mms_ref;
CREATE TABLE mms_ref AS
  SELECT g, sr_formula(provenance(), 'mms_name') AS formula
  FROM mms GROUP BY g HAVING max(v) <= 3;
SELECT remove_provenance('mms_ref');
SELECT 'max <= 3' AS shape, g, formula FROM mms_ref ORDER BY g;
DROP TABLE mms_ref;

-- (1) Boolean semiring: sweep every valuation of {a, b, c, d}, every
--     operator, MIN and MAX, with the constant on either side, and
--     compare sr_boolean with the truth computed directly on the
--     untracked copy (the empty group compares as false, since MIN / MAX
--     of an empty group is NULL).  Only mismatches are returned.
CREATE TABLE mms_bool(value boolean, provenance uuid, name text);
INSERT INTO mms_bool SELECT false, provenance, value FROM mms_name;

CREATE FUNCTION mms_check(agg text, op text, c int, const_left boolean)
RETURNS TABLE(grp int, world text, expected boolean, got boolean) AS $$
DECLARE
  r record;
  m int;
  present text[];
  having_clause text;
BEGIN
  IF const_left THEN
    having_clause := format('%s %s %s(v)', c, op, agg);
  ELSE
    having_clause := format('%s(v) %s %s', agg, op, c);
  END IF;
  FOR m IN 0..15 LOOP
    present := ARRAY(SELECT n FROM unnest(ARRAY['a','b','c','d'])
                     WITH ORDINALITY AS u(n, i)
                     WHERE (m >> (i - 1)::int) & 1 = 1);
    UPDATE mms_bool SET value = (name = ANY(present));
    FOR r IN EXECUTE format(
      'SELECT g, provenance() AS tok FROM mms GROUP BY g HAVING %s',
      having_clause)
    LOOP
      grp := r.g;
      world := array_to_string(present, '');
      EXECUTE format('SELECT coalesce((SELECT %s FROM mms_plain '
                     'WHERE g = %s AND name = ANY($1)), false)',
                     having_clause, r.g)
        INTO expected USING present;
      got := sr_boolean(r.tok, 'mms_bool');
      IF expected IS DISTINCT FROM got THEN
        RETURN NEXT;
      END IF;
    END LOOP;
  END LOOP;
END
$$ LANGUAGE plpgsql;

SELECT a.agg, o.op, s.const_left,
       (SELECT count(*) FROM mms_check(a.agg, o.op, 3, s.const_left))
         AS mismatches
FROM (VALUES ('min'), ('max')) AS a(agg),
     (VALUES ('<'), ('<='), ('>='), ('>'), ('='), ('<>')) AS o(op),
     (VALUES (false), (true)) AS s(const_left)
ORDER BY a.agg, o.op, s.const_left;

-- One valuation spelled out: a, b, d present, c absent.  Group 1's
-- minimum is then 1, its maximum 3; group 2 is {d} = {4}.
UPDATE mms_bool SET value = (name <> 'c');
CREATE TABLE mms_b AS
  SELECT g, 'min >= 3' AS shape, sr_boolean(provenance(), 'mms_bool') AS b
  FROM mms GROUP BY g HAVING min(v) >= 3;
SELECT remove_provenance('mms_b');
CREATE TABLE mms_b2 AS
  SELECT g, 'min >= 1' AS shape, sr_boolean(provenance(), 'mms_bool') AS b
  FROM mms GROUP BY g HAVING min(v) >= 1;
SELECT remove_provenance('mms_b2');
CREATE TABLE mms_b3 AS
  SELECT g, 'max = 3' AS shape, sr_boolean(provenance(), 'mms_bool') AS b
  FROM mms GROUP BY g HAVING max(v) = 3;
SELECT remove_provenance('mms_b3');
CREATE TABLE mms_b4 AS
  SELECT g, 'max > 3' AS shape, sr_boolean(provenance(), 'mms_bool') AS b
  FROM mms GROUP BY g HAVING max(v) > 3;
SELECT remove_provenance('mms_b4');
SELECT * FROM mms_b UNION ALL SELECT * FROM mms_b2
UNION ALL SELECT * FROM mms_b3 UNION ALL SELECT * FROM mms_b4
ORDER BY shape, g;
DROP TABLE mms_b, mms_b2, mms_b3, mms_b4;

-- (2) Tropical (nonnegative, min-plus: 𝟙 = 0, 𝟘 = ∞, x ⊖ y = x if x < y
--     else ∞) and Viterbi (max-times on [0, 1]: x ⊖ y = x if x > y else
--     0) over the cost / weight mappings, MIN and MAX, all six operators.
--     Tropical, group 1 (costs a:1, b:2, c:3): min < 3 is L = 1;
--     min >= 3 is (0 ⊖ 1) + min(2, 3) = 0 + 2 = 2; min > 3 is
--     (0 ⊖ 1) + 3 = 3; min <> 3 is min(1, 3) = 1.  Group 2 (d:4) has no
--     contributor below 3, so min >= 3 is 4 and min < 3 is ∞.
CREATE FUNCTION mms_costs(agg text, op text, c int)
RETURNS TABLE(shape text, grp int, tropical float, viterbi float) AS $$
DECLARE
  r record;
BEGIN
  shape := format('%s %s %s', agg, op, c);
  FOR r IN EXECUTE format(
    'SELECT g, provenance() AS tok FROM mms GROUP BY g HAVING %s(v) %s %s',
    agg, op, c)
  LOOP
    grp := r.g;
    tropical := sr_tropical(r.tok, 'mms_cost', nonnegative => true);
    viterbi := round(sr_viterbi(r.tok, 'mms_w')::numeric, 6);
    RETURN NEXT;
  END LOOP;
END
$$ LANGUAGE plpgsql;

SELECT k.shape, k.grp, k.tropical, k.viterbi
FROM (VALUES ('min'), ('max')) AS a(agg),
     (VALUES ('<'), ('<='), ('>='), ('>'), ('='), ('<>')) AS o(op),
     LATERAL mms_costs(a.agg, o.op, 3) AS k
ORDER BY a.agg, o.op, k.grp;

-- (3) Probabilities over a JOIN with a second tracked table: every
--     contributor is a product x_i ∧ y_g, not an independent literal, so
--     the Boolean-circuit construction does not certify the enumeration
--     and takes the closed form.  With every probability 0.5, group 1 has
--     Pr(min >= 3) = Pr(y ∧ ¬a ∧ (b ∨ c)) = 0.5 · 0.5 · 0.75 = 0.1875,
--     Pr(min < 3) = Pr(y ∧ a) = 0.25, Pr(min = 3) = Pr(y ∧ ¬a ∧ b) =
--     0.125, Pr(min <> 3) = Pr(y ∧ (a ∨ (¬a ∧ ¬b ∧ c))) = 0.3125,
--     Pr(max <= 3) = Pr(y ∧ ¬c ∧ (a ∨ b)) = 0.1875 and Pr(max > 3) =
--     Pr(y ∧ c) = 0.25; group 2 has Pr(min >= 3) = Pr(y ∧ d) = 0.25.
--     provsql.cmp_probability_evaluation on / off must agree (its MIN /
--     MAX pre-pass declines the shared y and leaves the circuit alone).
CREATE TABLE mmy(g int, ytag text);
INSERT INTO mmy VALUES (1, 'y1'), (2, 'y2');
SELECT add_provenance('mmy');
SELECT create_provenance_mapping('mmy_name', 'mmy', 'ytag');
CREATE TABLE mm_names AS
  SELECT value, provenance FROM mms_name
  UNION ALL SELECT value, provenance FROM mmy_name;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM mms; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM mmy; END $$;

CREATE FUNCTION mms_join_prob(agg text, op text, c int)
RETURNS TABLE(shape text, grp int, p_on numeric, p_off numeric,
              boolexpr text) AS $$
DECLARE
  r record;
BEGIN
  shape := format('%s %s %s', agg, op, c);
  FOR r IN EXECUTE format(
    'SELECT s.g, provenance() AS tok FROM mms s JOIN mmy y ON s.g = y.g '
    'GROUP BY s.g HAVING %s(s.v) %s %s', agg, op, c)
  LOOP
    grp := r.g;
    SET provsql.cmp_probability_evaluation = on;
    p_on := round(probability_evaluate(r.tok)::numeric, 6);
    SET provsql.cmp_probability_evaluation = off;
    p_off := round(probability_evaluate(r.tok)::numeric, 6);
    RESET provsql.cmp_probability_evaluation;
    boolexpr := sr_boolexpr(r.tok, 'mm_names');
    RETURN NEXT;
  END LOOP;
END
$$ LANGUAGE plpgsql;

SELECT k.shape, k.grp, k.p_on, k.p_off, k.boolexpr
FROM (VALUES ('min'), ('max')) AS a(agg),
     (VALUES ('<'), ('<='), ('>='), ('>'), ('='), ('<>')) AS o(op),
     LATERAL mms_join_prob(a.agg, o.op, 3) AS k
ORDER BY a.agg, o.op, k.grp;

DROP FUNCTION mms_check(text, text, int, boolean);
DROP FUNCTION mms_costs(text, text, int);
DROP FUNCTION mms_join_prob(text, text, int);
DROP TABLE mm_names, mmy_name, mms_bool, mms_w, mms_cost, mms_name;
DROP TABLE mmy, mms, mms_plain;
