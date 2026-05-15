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

SET provsql.boolean_provenance = off;
DROP TABLE sqb_left;
DROP TABLE sqb_right;
