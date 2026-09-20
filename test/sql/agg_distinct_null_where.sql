\set ECHO none
\pset format unaligned

-- AGG(DISTINCT) is computed in subqueries joined back to the query on its
-- GROUP BY columns.  Two things must hold of that join:
--  * a group whose key is NULL matches (GROUP BY puts NULL keys together);
--  * it is added to the WHERE clause of the query, not put in its place: the
--    aggregates without DISTINCT are computed at the query's own level.
-- Every result is compared with the same query over an untracked copy.
--
--   g=1: (v,w) = (5,1), (5,2), (6,3)    g=NULL: (7,4), (7,5)    g=2: (1,6)
--   h is NULL on the rows of g=1, 'x' elsewhere; k is declared NOT NULL.

CREATE TABLE adn(g int, h text, k int NOT NULL, v int, w int);
INSERT INTO adn VALUES (1,NULL,1,5,1), (1,NULL,1,5,2), (1,NULL,1,6,3),
  (NULL,'x',0,7,4), (NULL,'x',0,7,5), (2,'x',2,1,6);
CREATE TABLE adn_u AS SELECT * FROM adn;
SELECT add_provenance('adn');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM adn; END $$;

-- Compare a query over adn with the same query over adn_u: no row may differ.
CREATE FUNCTION adn_check(label text, query text) RETURNS TABLE(q text, differences bigint)
LANGUAGE plpgsql AS $$
BEGIN
  SET LOCAL client_min_messages = warning;
  EXECUTE 'CREATE TEMP TABLE adn_t AS ' || replace(query, '@', 'adn');
  PERFORM provsql.remove_provenance('adn_t');
  EXECUTE 'CREATE TEMP TABLE adn_s AS ' || replace(query, '@', 'adn_u');
  -- The tracked side holds aggregates as agg_token, displayed "3 (*)": bring
  -- the row text back to that of the plain values.
  CREATE TEMP TABLE adn_tt AS
    SELECT regexp_replace((t.*)::text, ' \(\*\)|"', '', 'g') AS r FROM adn_t t;
  RETURN QUERY EXECUTE
    'SELECT $1, (SELECT count(*) FROM
        ((SELECT r FROM adn_tt EXCEPT ALL SELECT (s.*)::text FROM adn_s s)
         UNION ALL
         (SELECT (s.*)::text FROM adn_s s EXCEPT ALL SELECT r FROM adn_tt)) x)'
    USING label;
  DROP TABLE adn_t, adn_s, adn_tt;
END $$;

SELECT * FROM adn_check('NULL key',
  'SELECT g, count(DISTINCT v) AS cd FROM @ GROUP BY g');
SELECT * FROM adn_check('NULL key, other aggregates',
  'SELECT g, count(*) AS c, count(DISTINCT v) AS cd, sum(w) AS sw FROM @ GROUP BY g');
SELECT * FROM adn_check('two keys, one NULL in turn',
  'SELECT g, h, count(DISTINCT v) AS cd, sum(w) AS sw FROM @ GROUP BY g, h');
SELECT * FROM adn_check('NOT NULL key',
  'SELECT k, count(DISTINCT v) AS cd, sum(w) AS sw FROM @ GROUP BY k');
SELECT * FROM adn_check('WHERE kept',
  'SELECT g, count(*) AS c, count(DISTINCT v) AS cd, sum(w) AS sw FROM @ WHERE v > 5 GROUP BY g');
SELECT * FROM adn_check('WHERE kept, NOT NULL key',
  'SELECT k, count(*) AS c, count(DISTINCT v) AS cd, min(w) AS mw FROM @ WHERE w <> 2 GROUP BY k');
SELECT * FROM adn_check('two DISTINCT aggregates',
  'SELECT g, count(DISTINCT v) AS cv, count(DISTINCT w) AS cw, count(*) AS c FROM @ WHERE w < 6 GROUP BY g');
SELECT * FROM adn_check('sum(DISTINCT)',
  'SELECT g, sum(DISTINCT v) AS sd, sum(v) AS s FROM @ WHERE w > 1 GROUP BY g');
SELECT * FROM adn_check('no GROUP BY',
  'SELECT count(DISTINCT v) AS cd, count(*) AS c FROM @ WHERE v > 5');

-- DISTINCT in HAVING, NULL key included.  count(DISTINCT v) >= 1 is group
-- existence: 1-0.5^3 = 0.875, 0.5, and 1-0.5^2 = 0.75 for the NULL key; with
-- the row w = 3 excluded by WHERE, g=1 has two rows left, 0.75.
CREATE TABLE adn_h AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM adn GROUP BY g HAVING count(DISTINCT v) >= 1;
SELECT remove_provenance('adn_h');
SELECT 'HAVING cd >= 1' AS q, g, p FROM adn_h ORDER BY g;
CREATE TABLE adn_h2 AS SELECT g, round(probability_evaluate(provenance())::numeric,4) AS p
  FROM adn WHERE w <> 3 GROUP BY g HAVING count(DISTINCT v) >= 1;
SELECT remove_provenance('adn_h2');
SELECT 'HAVING cd >= 1, WHERE' AS q, g, p FROM adn_h2 ORDER BY g;

-- The aggregates beside the DISTINCT one range over the rows WHERE selects,
-- in the circuit too: E[count(*)] over w <> 2 is 0.5 per remaining row,
-- conditioned on the group having a row (so 1/0.75 for two rows, 0.5/0.5 for
-- one).
CREATE TABLE adn_e AS SELECT g, count(*) AS c, count(DISTINCT v) AS cd
  FROM adn WHERE w <> 2 GROUP BY g;
SELECT remove_provenance('adn_e');
-- rounded, so that the digits of a float8 do not depend on the server version
SELECT 'expected beside DISTINCT' AS q, g,
       round(expected(c)::numeric, 6) AS ec FROM adn_e ORDER BY g;

DROP FUNCTION adn_check(text, text);
DROP TABLE adn, adn_u, adn_h, adn_h2, adn_e;
