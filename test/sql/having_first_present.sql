\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- HAVING predicates decided by the *first present* occurrence of the
-- group -- choose() (PICKFIRST) against a text constant, and bool_or /
-- bool_and against a boolean constant -- must carry the possible-world
-- semantics in every m-semiring: a world W is annotated
-- ∏_{W} k ⊗ (𝟙 ⊖ ⊕_{U∖W} k), summed over the non-empty worlds that
-- satisfy the predicate.  In absorptive semirings whose ⊗ distributes
-- over ⊖ the worlds sharing a first occurrence collapse into one term
-- (𝟙 ⊖ ⊕_{before} k) ⊗ k_i; elsewhere (why, formula, counting, the
-- security semiring) they are enumerated.
--
-- Every predicate below is paired with a MIN / MAX predicate that holds
-- in exactly the same worlds and is evaluated by the generic
-- possible-worlds machinery (the enumeration in non-absorptive
-- semirings, the MIN / MAX scan in absorptive distributive ones), so the
-- two columns of each row must agree in every semiring.
-- ----------------------------------------------------------------------

-- choose(name ORDER BY v) = 'y' over x:10, w:20, y:30, z:40 holds in
-- the worlds whose first present occurrence is y: {y} and {y, z}, the
-- same worlds as min(v) = 30.
CREATE TABLE hfp(a int, v int, name text, cost float);
INSERT INTO hfp VALUES (1, 10, 'x', 1), (1, 20, 'w', 2), (1, 30, 'y', 3),
                       (1, 40, 'z', 4);
SELECT add_provenance('hfp');
SELECT create_provenance_mapping('hfp_name', 'hfp', 'name');
SELECT create_provenance_mapping('hfp_cost', 'hfp', 'cost');

CREATE TABLE hfp_c AS
  SELECT a, sr_why(provenance(), 'hfp_name') AS why,
         sr_formula(provenance(), 'hfp_name') AS formula,
         sr_counting(provenance(), 'hfp_cost') AS counting,
         sr_tropical(provenance(), 'hfp_cost', nonnegative => true) AS tropical
  FROM hfp GROUP BY a HAVING choose(name ORDER BY v) = 'y';
SELECT remove_provenance('hfp_c');
CREATE TABLE hfp_m AS
  SELECT a, sr_why(provenance(), 'hfp_name') AS why,
         sr_formula(provenance(), 'hfp_name') AS formula,
         sr_counting(provenance(), 'hfp_cost') AS counting,
         sr_tropical(provenance(), 'hfp_cost', nonnegative => true) AS tropical
  FROM hfp GROUP BY a HAVING min(v) = 30;
SELECT remove_provenance('hfp_m');
SELECT 'choose = y' AS shape, why, formula, counting, tropical FROM hfp_c
UNION ALL
SELECT 'min = 30', why, formula, counting, tropical FROM hfp_m;
SELECT c.why = m.why AS why_agree, c.counting = m.counting AS counting_agree,
       c.tropical = m.tropical AS tropical_agree
FROM hfp_c c, hfp_m m;
DROP TABLE hfp_c, hfp_m;

-- choose(name ORDER BY v) <> 'x': first present occurrence is not x,
-- i.e. x is absent and the group is non-empty -- the worlds of
-- min(v) > 10.
CREATE TABLE hfp_c AS
  SELECT a, sr_why(provenance(), 'hfp_name') AS why,
         sr_counting(provenance(), 'hfp_cost') AS counting
  FROM hfp GROUP BY a HAVING choose(name ORDER BY v) <> 'x';
SELECT remove_provenance('hfp_c');
CREATE TABLE hfp_m AS
  SELECT a, sr_why(provenance(), 'hfp_name') AS why,
         sr_counting(provenance(), 'hfp_cost') AS counting
  FROM hfp GROUP BY a HAVING min(v) > 10;
SELECT remove_provenance('hfp_m');
SELECT 'choose <> x' AS shape, why, counting FROM hfp_c
UNION ALL
SELECT 'min > 10', why, counting FROM hfp_m;
DROP TABLE hfp_c, hfp_m;

-- Boolean aggregates over p:true, q:false, s:true, with ti = t::int so
-- that bool_and(t) = true is min(ti) = 1, bool_or(t) = false is
-- max(ti) = 0, bool_or(t) = true is max(ti) = 1 and bool_and(t) = false
-- is min(ti) = 0.
CREATE TABLE hfb(a int, t boolean, ti int, name text, cost float);
INSERT INTO hfb VALUES (1, true, 1, 'p', 1), (1, false, 0, 'q', 2),
                       (1, true, 1, 's', 3);
SELECT add_provenance('hfb');
SELECT create_provenance_mapping('hfb_name', 'hfb', 'name');
SELECT create_provenance_mapping('hfb_cost', 'hfb', 'cost');

CREATE FUNCTION hfb_pair(bool_pred text, num_pred text)
RETURNS TABLE(shape text, why text, formula text, counting int,
              tropical float) AS $$
DECLARE
  r record;
BEGIN
  FOR r IN EXECUTE format(
    'SELECT sr_why(provenance(), ''hfb_name'') AS why, '
    '       sr_formula(provenance(), ''hfb_name'') AS formula, '
    '       sr_counting(provenance(), ''hfb_cost'') AS counting, '
    '       sr_tropical(provenance(), ''hfb_cost'', nonnegative => true) '
    '         AS tropical '
    'FROM hfb GROUP BY a HAVING %s', bool_pred)
  LOOP
    shape := bool_pred; why := r.why; formula := r.formula;
    counting := r.counting; tropical := r.tropical;
    RETURN NEXT;
  END LOOP;
  FOR r IN EXECUTE format(
    'SELECT sr_why(provenance(), ''hfb_name'') AS why, '
    '       sr_formula(provenance(), ''hfb_name'') AS formula, '
    '       sr_counting(provenance(), ''hfb_cost'') AS counting, '
    '       sr_tropical(provenance(), ''hfb_cost'', nonnegative => true) '
    '         AS tropical '
    'FROM hfb GROUP BY a HAVING %s', num_pred)
  LOOP
    shape := num_pred; why := r.why; formula := r.formula;
    counting := r.counting; tropical := r.tropical;
    RETURN NEXT;
  END LOOP;
END
$$ LANGUAGE plpgsql;

SELECT * FROM hfb_pair('bool_and(t) = true', 'min(ti) = 1');
SELECT * FROM hfb_pair('bool_or(t) = false', 'max(ti) = 0');
SELECT * FROM hfb_pair('bool_or(t) = true', 'max(ti) = 1');
SELECT * FROM hfb_pair('bool_and(t) = false', 'min(ti) = 0');
SELECT * FROM hfb_pair('every(t) <> true', 'min(ti) <> 1');

DROP FUNCTION hfb_pair(text, text);
DROP TABLE hfb_name, hfb_cost, hfb, hfp_name, hfp_cost, hfp;
