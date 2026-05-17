\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- End-to-end exercise of provsql.boolean_provenance for a two-atom
-- self-join-free hierarchical conjunctive query of the form
-- q(x) :- A(x), B(x).  The rewrite is only sound for global /
-- existential interrogations of the result (per-row semantics
-- change because each atom is wrapped in SELECT DISTINCT), so the
-- canonical user-facing pattern is GROUP BY the root variable.
--
-- Multiple-match rows on each side make the read-once vs
-- shared-input distinction visible after GROUP BY:
--
--   GUC off, GROUP BY id  -> per-id Boolean expression is the OR
--      of all join-pair products, with every leaf appearing
--      multiple times.  Not read-once;
--      probability_evaluate('independent') rejects.
--   GUC on,  GROUP BY id  -> rewriter wraps each atom as
--      SELECT DISTINCT id; the per-id provenance becomes
--      gate_times(gate_plus(left ids), gate_plus(right ids));
--      each leaf appears once; 'independent' succeeds and the
--      probabilities match the baseline computed by the default
--      evaluator.

CREATE TABLE sqb_left (id int);
CREATE TABLE sqb_right(id int);
INSERT INTO sqb_left  VALUES (1),(3),(3);
INSERT INTO sqb_right VALUES (1),(3),(3);

SELECT add_provenance('sqb_left');
SELECT add_provenance('sqb_right');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM sqb_left;
  PERFORM set_prob(provsql, 0.4) FROM sqb_right;
END $$;

-- (1) Without the rewrite: the unrewritten per-id circuit has shared
--     input gates (each leaf appears in multiple join pairs), so
--     'independent' rejects on id=3.
SET provsql.boolean_provenance = off;
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM sqb_left a, sqb_right b
     WHERE a.id = b.id AND a.id = 3
     GROUP BY a.id;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject the unrewritten '
                    'id=3 circuit (shared input gates)';
  END IF;
END $$;

-- (2) With the rewrite: per-id provenance is read-once, so
--     'independent' succeeds and returns the analytical value:
--       id=1: 0.5 * 0.4                            = 0.20
--       id=3: (1-(1-0.5)^2) * (1-(1-0.4)^2) = 0.75 * 0.64 = 0.48
SET provsql.boolean_provenance = on;
CREATE TEMP TABLE sqb_prob AS
  SELECT a.id,
         probability_evaluate(provenance(), 'independent') AS p
    FROM sqb_left a, sqb_right b
   WHERE a.id = b.id
   GROUP BY a.id;
SELECT remove_provenance('sqb_prob');
SELECT id, ROUND(p::numeric, 6) AS prob FROM sqb_prob ORDER BY id;

-- (3) Cross-check: rewritten probabilities must match the default
--     baseline (which uses dDNNF / tree-decomposition / d4 fallback
--     on the unrewritten circuit).
SET provsql.boolean_provenance = off;
CREATE TEMP TABLE sqb_baseline AS
  SELECT a.id, probability_evaluate(provenance()) AS p
    FROM sqb_left a, sqb_right b
   WHERE a.id = b.id
   GROUP BY a.id;
SELECT remove_provenance('sqb_baseline');
SELECT b.id, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM sqb_baseline b JOIN sqb_prob r ON b.id = r.id
 ORDER BY b.id;

-- (4) Rewritten root gate shape: gate_times over two gate_plus children.
SET provsql.boolean_provenance = on;
CREATE TEMP TABLE sqb_shape AS
  SELECT a.id,
         get_gate_type(provenance())                  AS root,
         array_length(get_children(provenance()), 1)  AS root_nchildren
    FROM sqb_left a, sqb_right b
   WHERE a.id = b.id
   GROUP BY a.id;
SELECT remove_provenance('sqb_shape');
SELECT id, root, root_nchildren FROM sqb_shape ORDER BY id;

-- (5) The per-atom SELECT DISTINCT wraps collapse duplicate source
--     tuples on their projection slots, which would shrink the
--     user-visible row count of a query without an outer GROUP BY or
--     top-level DISTINCT.  The candidate gate refuses such queries
--     so the GUC stays observationally transparent.  Cardinality
--     under the GUC must match the GUC-off baseline: 1 row at id=1
--     (1x1 self-join) plus 4 rows at id=3 (2x2 self-join) = 5 rows.
SET provsql.boolean_provenance = off;
CREATE TEMP TABLE sqb_nogb_off AS
  SELECT a.id FROM sqb_left a, sqb_right b WHERE a.id = b.id;
SELECT remove_provenance('sqb_nogb_off');
SELECT count(*) AS rows_off FROM sqb_nogb_off;

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE sqb_nogb_on AS
  SELECT a.id FROM sqb_left a, sqb_right b WHERE a.id = b.id;
SELECT remove_provenance('sqb_nogb_on');
SELECT count(*) AS rows_on FROM sqb_nogb_on;

-- (6) SELECT DISTINCT 1 is the standard Boolean-query idiom: project
--     every body variable out and return a single tuple whose
--     provenance is the OR over all matching join tuples.  The
--     rewriter must accept it (distinctClause is non-NIL) and produce
--     a circuit whose probability matches the baseline.  With
--     duplicates on both sides (id=3 contributes 2x2 matching
--     pairs), the baseline circuit shares input gates and the
--     rewritten one does not, so 'independent' should succeed on the
--     rewritten circuit while still agreeing with the baseline
--     probability.  Expected value with p_a=0.5, p_b=0.4:
--       P(matched at id=1) = 0.5 * 0.4                    = 0.20
--       P(at least one A-id=3) = 1 - 0.5^2                = 0.75
--       P(at least one B-id=3) = 1 - 0.6^2                = 0.64
--       P(matched at id=3) = 0.75 * 0.64                  = 0.48
--       P(non-empty answer) = 1 - (1-0.20) * (1-0.48)
--                           = 1 - 0.8 * 0.52              = 0.584
SET provsql.boolean_provenance = off;
CREATE TEMP TABLE sqb_dist1_off AS
  SELECT DISTINCT 1 AS one FROM sqb_left a, sqb_right b WHERE a.id = b.id;
CREATE TEMP TABLE sqb_dist1_off_p AS
  SELECT one, ROUND(probability_evaluate(provsql)::numeric, 6) AS p
    FROM sqb_dist1_off;
SELECT remove_provenance('sqb_dist1_off_p');
SELECT count(*) AS rows_off, p AS p_off FROM sqb_dist1_off_p GROUP BY p;

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE sqb_dist1_on AS
  SELECT DISTINCT 1 AS one FROM sqb_left a, sqb_right b WHERE a.id = b.id;
CREATE TEMP TABLE sqb_dist1_on_p AS
  SELECT one, ROUND(probability_evaluate(provsql, 'independent')::numeric, 6) AS p
    FROM sqb_dist1_on;
SELECT remove_provenance('sqb_dist1_on_p');
SELECT count(*) AS rows_on, p AS p_on FROM sqb_dist1_on_p GROUP BY p;

SET provsql.boolean_provenance = off;
DROP TABLE sqb_left;
DROP TABLE sqb_right;
