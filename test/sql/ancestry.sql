\set ECHO none
\pset format unaligned

-- Exercises the per-table base-ancestor registry: get_ancestors /
-- set_ancestors / remove_ancestors and the auto-population by
-- add_provenance / repair_key.  The registry feeds the safe-query
-- rewriter's disjoint-base-ancestor check (a follow-up slice consults
-- it instead of the current syntactic shared-relid bail).
--
-- Output is intentionally not sensitive to the specific OIDs (which
-- shift across runs): we compare ancestor lists against the relid of
-- the relation under test.

-- (1) Base TID table: add_provenance seeds ancestors = {self}.
CREATE TABLE anc_a(x int);
SELECT add_provenance('anc_a');
SELECT get_ancestors('anc_a'::regclass::oid)
       = ARRAY['anc_a'::regclass::oid] AS auto_seeded_to_self;

-- (2) Base BID table: repair_key also seeds ancestors = {self}.
CREATE TABLE anc_b(k int, v int);
INSERT INTO anc_b VALUES (1, 10), (1, 11), (2, 20);
SELECT repair_key('anc_b', 'k');
SELECT get_ancestors('anc_b'::regclass::oid)
       = ARRAY['anc_b'::regclass::oid] AS bid_seeded_to_self;

-- (3) Manual set_ancestors overwrites the auto-seeded entry without
--     disturbing the kind half (still TID for anc_a, BID for anc_b).
SELECT set_ancestors('anc_a'::regclass::oid,
                     ARRAY['anc_a'::regclass::oid,
                           'anc_b'::regclass::oid]);
SELECT array_length(get_ancestors('anc_a'::regclass::oid), 1)
         AS anc_a_ancestor_count,
       (get_table_info('anc_a'::regclass::oid)).kind
         AS anc_a_kind_unchanged;

-- (4) remove_ancestors clears the ancestor half but keeps kind.
SELECT remove_ancestors('anc_a'::regclass::oid);
SELECT get_ancestors('anc_a'::regclass::oid) IS NULL AS ancestors_cleared,
       (get_table_info('anc_a'::regclass::oid)).kind
         AS kind_still_present;

-- (5) remove_table_info wipes the whole record (both halves).
SELECT remove_table_info('anc_b'::regclass::oid);
SELECT get_ancestors('anc_b'::regclass::oid) IS NULL AS both_cleared,
       get_table_info('anc_b'::regclass::oid) IS NULL AS table_info_cleared;

-- (6) get_ancestors on an untracked relation returns NULL.
CREATE TABLE anc_untracked(x int);
SELECT get_ancestors('anc_untracked'::regclass::oid) IS NULL
         AS untracked_returns_null;

-- (7) set_ancestors on an untracked relation is silently a no-op
--     (the worker doesn't create an OPAQUE-by-default record).
SELECT set_ancestors('anc_untracked'::regclass::oid,
                     ARRAY['anc_untracked'::regclass::oid]);
SELECT get_ancestors('anc_untracked'::regclass::oid) IS NULL
         AS still_no_record;

-- (8) DROP TABLE on a tracked relation: the existing
--     cleanup_table_info event trigger fires remove_table_info,
--     which tombstones the whole record (kind + ancestors).
CREATE TABLE anc_dropme(x int);
SELECT add_provenance('anc_dropme');
SELECT 'anc_dropme'::regclass::oid AS anc_dropme_oid \gset
DROP TABLE anc_dropme;
SELECT get_ancestors(:anc_dropme_oid) IS NULL
         AS dropped_relid_has_no_ancestors;

-- (9) Cap check: set_ancestors with > 64 entries raises a clear error.
DO $$
DECLARE
  too_many oid[];
  raised   boolean := false;
BEGIN
  SELECT array_agg(i::oid) INTO too_many FROM generate_series(1, 65) g(i);
  BEGIN
    PERFORM provsql.set_ancestors('anc_a'::regclass::oid, too_many);
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected set_ancestors to reject a 65-entry ancestor set';
  END IF;
END $$;

DROP TABLE anc_a;
DROP TABLE anc_b;
DROP TABLE anc_untracked;
