\set ECHO none
\pset format unaligned

-- gate_annotation is a transparent single-child wrapper: identity for every
-- consumer (probability + all semirings + PROV-XML), and -- unlike the
-- children-only content-addressing convention -- its UUID folds in the extra
-- so distinct annotations over one child are distinct gates while equal ones
-- coincide.  This is the carrier for the inversion-free certificate / order
-- keys; here we only check it is inert.

CREATE TABLE annot_t(id int, lbl text);
INSERT INTO annot_t VALUES (1,'a'),(2,'b'),(3,'c');
SELECT add_provenance('annot_t');
SELECT create_provenance_mapping('annot_lbl','annot_t','lbl');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM annot_t; END $$;

-- root = plus over the three input tokens; two equal annotations and one with
-- a different extra payload.
CREATE TEMP TABLE annot_w (root uuid, ann uuid, ann_same uuid, ann_diff uuid);
DO $$
DECLARE r uuid := public.uuid_generate_v4();
BEGIN
  PERFORM provsql.create_gate(r, 'plus', (SELECT array_agg(provsql) FROM annot_t));
  INSERT INTO annot_w
    SELECT r, annotate(r, 'Crecipe'), annotate(r, 'Crecipe'), annotate(r, 'Kother');
END $$;

-- (1) Structure: the wrapper is a single-child 'annotation' gate over the root.
SELECT get_gate_type(ann)::text         AS ann_type,
       get_children(ann) = ARRAY[root]  AS child_is_root
FROM annot_w;

-- (2) Identity: the extra is folded into the UUID -- equal extras coincide,
--     different extras are distinct gates (the property a children-only token
--     could not provide).
SELECT (ann = ann_same)  AS equal_extra_coincide,
       (ann <> ann_diff) AS diff_extra_distinct
FROM annot_w;

-- (3) Probability is transparent through the wrapper.
SELECT round(probability_evaluate(root)::numeric, 6)
         = round(probability_evaluate(ann)::numeric, 6) AS prob_transparent
FROM annot_w;

-- (4) Every semiring is transparent (counting, boolean, why) -- in particular
--     the annotation is NOT a Boolean poison pill (contrast gate_assumed).
SELECT (sr_counting(root,'annot_lbl')       =  sr_counting(ann,'annot_lbl'))       AS counting_transparent,
       (sr_boolean(root,'annot_lbl')::text  =  sr_boolean(ann,'annot_lbl')::text)  AS boolean_transparent,
       (sr_why(root,'annot_lbl')::text      =  sr_why(ann,'annot_lbl')::text)      AS why_transparent
FROM annot_w;

-- (5) PROV-XML export does not error on the wrapper.
SELECT to_provxml(ann) IS NOT NULL AS provxml_ok FROM annot_w;
