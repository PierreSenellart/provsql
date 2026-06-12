\set ECHO none
\pset format unaligned

-- NULL inputs never participate in an aggregate (SQL semantics): sum / min /
-- max / avg / count(expr) ignore NULL-valued rows, while count(*) counts every
-- row.  count(expr) is special: an all-NULL group still has a defined result
-- of 0, so a NULL-valued row stays present (contributing 0) rather than being
-- dropped.  This matters once the outer-join lowering manufactures NULL-padded
-- rows.

-- Part 1: deterministic value check on a plain GROUP BY with a NULL value.
CREATE TABLE an_s(g int, v int);
INSERT INTO an_s VALUES (1,10),(1,NULL),(1,20);
SELECT add_provenance('an_s');

-- sum=30, min=10, max=20, avg=15, count(v)=2, count(*)=3 (NULL ignored).
CREATE TABLE an_agg AS
  SELECT g, sum(v) AS s, min(v) AS mn, max(v) AS mx, avg(v) AS av,
         count(v) AS cv, count(*) AS cs
  FROM an_s GROUP BY g;
SELECT remove_provenance('an_agg');
SELECT * FROM an_agg ORDER BY g;
DROP TABLE an_agg;
DROP TABLE an_s;

-- Part 2: count over a LEFT JOIN's NULL-padded row, across possible worlds.
-- r1.k=1 present always; q has (1,10),(1,20) independent at 0.5.  The k=1 group
-- always exists (the LEFT JOIN keeps r1), and count(q.k) counts only matched
-- rows: it is 0 in the world where neither q row is present.
CREATE TABLE an_r1(k int);
CREATE TABLE an_q(k int, v int);
INSERT INTO an_r1 VALUES (1);
INSERT INTO an_q  VALUES (1,10),(1,20);
SELECT add_provenance('an_r1');
SELECT add_provenance('an_q');
DO $$ BEGIN
  PERFORM set_prob(provsql, 1.0) FROM an_r1;
  PERFORM set_prob(provsql, 0.5) FROM an_q;
END $$;

-- count(q.k)=0  -> P(no match)  = 0.25
CREATE TABLE an_c0 AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM an_r1 LEFT JOIN an_q ON an_q.k = an_r1.k GROUP BY an_r1.k
  HAVING count(an_q.k) = 0;
SELECT remove_provenance('an_c0');
SELECT 'count(q.k)=0' AS having, p FROM an_c0;
DROP TABLE an_c0;

-- count(q.k)>=1 -> P(>=1 match) = 0.75
CREATE TABLE an_c1 AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM an_r1 LEFT JOIN an_q ON an_q.k = an_r1.k GROUP BY an_r1.k
  HAVING count(an_q.k) >= 1;
SELECT remove_provenance('an_c1');
SELECT 'count(q.k)>=1' AS having, p FROM an_c1;
DROP TABLE an_c1;

-- count(q.k)<=1 -> P(<=1 match) = 0.75
CREATE TABLE an_c2 AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM an_r1 LEFT JOIN an_q ON an_q.k = an_r1.k GROUP BY an_r1.k
  HAVING count(an_q.k) <= 1;
SELECT remove_provenance('an_c2');
SELECT 'count(q.k)<=1' AS having, p FROM an_c2;
DROP TABLE an_c2;

-- count(*)<=1 counts the NULL-padded row too: both->2, one->1, none->1 -> 0.75
CREATE TABLE an_cs AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM an_r1 LEFT JOIN an_q ON an_q.k = an_r1.k GROUP BY an_r1.k
  HAVING count(*) <= 1;
SELECT remove_provenance('an_cs');
SELECT 'count(*)<=1' AS having, p FROM an_cs;
DROP TABLE an_cs;

DROP TABLE an_r1;
DROP TABLE an_q;
