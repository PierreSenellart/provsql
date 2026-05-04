\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Regression test for tree-decomposition probability evaluation on
-- circuits containing mulinput gates (produced by repair_key).
--
-- This is the minimal reproducer from TODO_bug.md.  Both
-- 'tree-decomposition' and the default method are expected to agree
-- with 'possible-worlds' on this scenario.

CREATE TABLE detection_td (
    photo_id integer,
    species_id integer,
    confidence double precision
);

INSERT INTO detection_td VALUES
  (5, 1, 0.40), (5, 1, 0.42), (5, 1, 0.38),
  (5, 3, 0.43), (5, 3, 0.48),
  (14, 1, 0.84), (14, 13, 0.18),
  (18, 1, 0.61), (18, 3, 0.55),
  (28, 1, 0.76), (28, 3, 0.69);

ALTER TABLE detection_td
    ADD COLUMN photo_species text
        GENERATED ALWAYS AS (photo_id || '/' || species_id) STORED;

SELECT repair_key('detection_td', 'photo_species');
DO $$ BEGIN
  PERFORM set_prob(provenance(), confidence) FROM detection_td;
END $$;

-- Multi-alternative case: photos 18 and 28 each have one deer and one
-- fox alternative, so deer AND fox reduces to P(deer)*P(fox).  Photo 5
-- has multiple alternatives whose probabilities sum > 1, so the
-- conjunction is clipped to 1.0 across all methods.
CREATE TABLE result_join AS
SELECT photo_id,
  ROUND(probability_evaluate(provenance())::numeric, 4)                       AS def_prob,
  ROUND(probability_evaluate(provenance(), 'tree-decomposition')::numeric, 4) AS td,
  ROUND(probability_evaluate(provenance(), 'possible-worlds')::numeric, 4)    AS pw
FROM detection_td d1 JOIN detection_td d2 USING (photo_id)
WHERE d1.species_id = 1 AND d2.species_id = 3
GROUP BY photo_id;

SELECT remove_provenance('result_join');

-- All three methods should agree.
SELECT photo_id,
       def_prob = pw AS default_matches_pw,
       td       = pw AS td_matches_pw
FROM result_join ORDER BY photo_id;

DROP TABLE result_join;

-- Single-alternative case (photo 14): Red Deer EXCEPT Gray Wolf.
-- Photo 14 has prob 0.84 for deer and a single Gray Wolf alternative
-- with prob 0.18, so P(deer AND NOT wolf) = 0.84*0.82 = 0.6888.
CREATE TABLE result_except AS
SELECT photo_id,
  ROUND(probability_evaluate(provenance())::numeric, 4)                       AS def_prob,
  ROUND(probability_evaluate(provenance(), 'tree-decomposition')::numeric, 4) AS td
FROM (
    SELECT photo_id FROM detection_td WHERE species_id = 1
  EXCEPT
    SELECT photo_id FROM detection_td WHERE species_id = 13
) t
WHERE photo_id = 14
GROUP BY photo_id;

SELECT remove_provenance('result_except');

SELECT photo_id,
       def_prob = 0.6888 AS default_correct,
       td       = def_prob AS td_matches_default
FROM result_except;

DROP TABLE result_except;
DROP TABLE detection_td;

-- Correctness check for rewriteMultivaluedGates() on n=2 mulvars.
--
-- The query is a self-join on photo_id with GROUP BY, so identical
-- mulinputs reach the OR (plus) gate via two different paths.  This
-- defeats the independent-evaluation shortcut and forces every method
-- to go through rewriteMultivaluedGates(), exercising the recursive
-- splitter on a non-trivial (n=2) mulvar.
--
-- Setup: bbox 2 has two competing candidates (fox 0.7, deer 0.2);
-- bbox 1 and bbox 3 each have a single candidate (deer 0.8, fox 0.6).
-- Hand-computed value: P((deer1 OR deer2) AND (fox2 OR fox3)) = 0.728,
-- conditioning on bbox 2's mulvar.
CREATE TABLE detection_n2 (
    photo_id   integer,
    bbox_id    integer,
    species_id integer,
    confidence double precision
);
INSERT INTO detection_n2 VALUES
    (1, 1, 1, 0.8),
    (1, 2, 3, 0.7), (1, 2, 1, 0.2),
    (1, 3, 3, 0.6);

ALTER TABLE detection_n2
    ADD COLUMN photo_bbox text
        GENERATED ALWAYS AS (photo_id || '/' || bbox_id) STORED;

SELECT repair_key('detection_n2', 'photo_bbox');
DO $$ BEGIN
  PERFORM set_prob(provenance(), confidence) FROM detection_n2;
END $$;

CREATE TABLE result_n2 AS
SELECT photo_id,
  ROUND(probability_evaluate(provenance(), 'possible-worlds')::numeric, 6)    AS pw,
  ROUND(probability_evaluate(provenance(), 'tree-decomposition')::numeric, 6) AS td
FROM detection_n2 d1 JOIN detection_n2 d2 USING (photo_id)
WHERE d1.species_id = 1 AND d2.species_id = 3
GROUP BY photo_id;

SELECT remove_provenance('result_n2');
SELECT photo_id,
       pw = 0.728 AS pw_correct,
       td = 0.728 AS td_correct
FROM result_n2;

DROP TABLE result_n2;
DROP TABLE detection_n2;
