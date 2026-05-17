\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Safe-query rewriter -- ancestry-based disjointness gate.
--
-- The candidate gate refuses any query whose FROM-list entries have
-- registered ancestor sets that overlap (the gate-level atoms would
-- not be independent across the join even when the syntactic relid
-- check accepts).  Same-relid pairs are still handled by the
-- existing shared-relid bail and its PK / disjoint-constant
-- approvals; the ancestry check fires on DIFFERENT-relid pairs whose
-- lineage traces back to a common base table.

CREATE TABLE sad_base(x int);
INSERT INTO sad_base VALUES (1), (1), (2);
SELECT add_provenance('sad_base');

CREATE TABLE sad_other(x int);
INSERT INTO sad_other VALUES (1), (2), (2);
SELECT add_provenance('sad_other');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM sad_base;
  PERFORM set_prob(provsql, 0.4) FROM sad_other;
END $$;

-- The CTAS lineage hook records ancestry on derived tables.
CREATE TABLE sad_derived_from_base  AS SELECT x, provsql FROM sad_base;
CREATE TABLE sad_derived_from_other AS SELECT x, provsql FROM sad_other;

-- Sanity: the derived tables inherited TID and the correct ancestors.
SELECT (get_table_info('sad_derived_from_base'::regclass::oid)).kind
         AS dfb_kind,
       get_ancestors('sad_derived_from_base'::regclass::oid)
         = ARRAY['sad_base'::regclass::oid]
         AS dfb_ancestors_correct;
SELECT (get_table_info('sad_derived_from_other'::regclass::oid)).kind
         AS dfo_kind,
       get_ancestors('sad_derived_from_other'::regclass::oid)
         = ARRAY['sad_other'::regclass::oid]
         AS dfo_ancestors_correct;

-- Probe helper: run the canonical 2-atom hierarchical CQ on (lhs, rhs)
-- under boolean_provenance=on and report whether the rewriter fired.
CREATE OR REPLACE FUNCTION sad_probe(lhs regclass, rhs regclass)
  RETURNS text AS $$
DECLARE
  gt   text;
BEGIN
  SET LOCAL provsql.boolean_provenance = on;
  EXECUTE format($f$
    CREATE TEMP TABLE sad_probe_res ON COMMIT DROP AS
      SELECT a.x AS x, provsql.provenance() AS p
        FROM %s a, %s b
       WHERE a.x = b.x
       GROUP BY a.x
  $f$, lhs, rhs);
  PERFORM provsql.remove_provenance('sad_probe_res');
  SELECT provsql.get_gate_type(p)::text INTO gt
    FROM sad_probe_res ORDER BY x LIMIT 1;
  DROP TABLE sad_probe_res;
  RETURN CASE WHEN gt = 'assumed_boolean' THEN 'fired' ELSE 'refused' END;
END;
$$ LANGUAGE plpgsql;

-- ---------------------------------------------------------------
-- (1) Derived table joined with one of its base ancestors:
--     ancestors {sad_base} overlap with {sad_base} -> REFUSE.
--     (The syntactic shared-relid bail does NOT catch this --
--     the two RTEs have different relids, sad_derived_from_base
--     and sad_base.)
-- ---------------------------------------------------------------
SELECT 'derived + its base' AS scenario,
       sad_probe('sad_derived_from_base', 'sad_base') AS result;

-- ---------------------------------------------------------------
-- (2) Two derived tables sharing an ancestor:
--     both came from sad_base -> ancestors {sad_base} vs {sad_base}
--     -> REFUSE.
-- ---------------------------------------------------------------
CREATE TABLE sad_derived_from_base_2 AS SELECT x, provsql FROM sad_base;
SELECT 'derived + derived (shared)' AS scenario,
       sad_probe('sad_derived_from_base', 'sad_derived_from_base_2') AS result;
DROP TABLE sad_derived_from_base_2;

-- ---------------------------------------------------------------
-- (3) Derived table joined with an unrelated base:
--     ancestors {sad_base} vs {sad_other} -> FIRE.
-- ---------------------------------------------------------------
SELECT 'derived + unrelated base' AS scenario,
       sad_probe('sad_derived_from_base', 'sad_other') AS result;

-- ---------------------------------------------------------------
-- (4) Two derived tables from disjoint bases:
--     ancestors {sad_base} vs {sad_other} -> FIRE.
-- ---------------------------------------------------------------
SELECT 'derived + derived (disjoint)' AS scenario,
       sad_probe('sad_derived_from_base', 'sad_derived_from_other') AS result;

-- ---------------------------------------------------------------
-- (5) Sanity: two unrelated bases still fire (ancestor sets
--     {sad_base} vs {sad_other} are trivially disjoint at the base
--     level too, so this is no regression).
-- ---------------------------------------------------------------
SELECT 'base + base (unrelated)' AS scenario,
       sad_probe('sad_base', 'sad_other') AS result;

DROP FUNCTION sad_probe(regclass, regclass);
DROP TABLE sad_derived_from_base;
DROP TABLE sad_derived_from_other;
DROP TABLE sad_base;
DROP TABLE sad_other;
