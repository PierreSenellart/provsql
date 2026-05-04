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
