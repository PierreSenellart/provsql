\set ECHO none
\pset format unaligned

CREATE TABLE group_by_empty_result AS
  SELECT provenance()
  FROM personnel
  GROUP BY ();

SELECT remove_provenance('group_by_empty_result');
SELECT sr_minmax(provenance,'personnel_level','unclassified'::classification_level) FROM group_by_empty_result;
DROP TABLE group_by_empty_result;

-- GROUP BY (), a HAVING with neither an aggregate nor a GROUP BY, and the
-- empty set of ROLLUP are the aggregation without GROUP BY: one row in every
-- world, even the one with no input row, as SQL returns it -- probability 1
-- over two rows at one half, where the plus of a group would give 0.75.  A
-- HAVING with no aggregate used to read the provenance of a row the plan does
-- not have, and crashed.  A UNION with no column is a deduplication: one row
-- where some arm has one (0.75).
CREATE TABLE gbe(x int);
INSERT INTO gbe VALUES (1), (2);
SELECT add_provenance('gbe');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM gbe; END $$;
CREATE TABLE gbe_e AS SELECT x FROM gbe WHERE false;
CREATE TABLE gbe_r AS
  SELECT 'count GROUP BY ()' AS q,
         round(probability_evaluate(provenance())::numeric, 4) AS p
    FROM (SELECT count(*) FROM gbe GROUP BY ()) s
  UNION ALL
  SELECT 'constant GROUP BY ()', round(probability_evaluate(provenance())::numeric, 4)
    FROM (SELECT 'b' FROM gbe GROUP BY ()) s
  UNION ALL
  SELECT 'constant GROUP BY (), no row', round(probability_evaluate(provenance())::numeric, 4)
    FROM (SELECT 'b' FROM gbe_e GROUP BY ()) s
  UNION ALL
  SELECT 'HAVING true', round(probability_evaluate(provenance())::numeric, 4)
    FROM (SELECT 'c' FROM gbe HAVING true) s
  UNION ALL
  SELECT 'HAVING true, no row', round(probability_evaluate(provenance())::numeric, 4)
    FROM (SELECT 'c' FROM gbe_e HAVING true) s
  UNION ALL
  SELECT 'ROLLUP grand total', round(probability_evaluate(provenance())::numeric, 4)
    FROM (SELECT x, count(*) FROM gbe GROUP BY ROLLUP(x)) s WHERE x IS NULL
  UNION ALL
  SELECT 'UNION of no column', round(probability_evaluate(provenance())::numeric, 4)
    FROM (SELECT FROM gbe UNION SELECT FROM gbe) s;
SELECT remove_provenance('gbe_r');
SELECT * FROM gbe_r;
SELECT count(*) AS having_false_rows FROM (SELECT 'c' FROM gbe HAVING false) s;
DROP TABLE gbe_r;
SELECT remove_provenance('gbe');
SELECT remove_provenance('gbe_e');
DROP TABLE gbe, gbe_e;
