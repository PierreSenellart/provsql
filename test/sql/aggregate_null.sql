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

-- Part 4: an aggregate under arithmetic in HAVING, where every contributed
-- value is NULL in some possible world.  Such a world leaves the aggregate
-- with no contributor, and SQL then reports NULL for every aggregate but
-- count -- so the comparison is NULL, i.e. false, and the world must not
-- be counted.  The arithmetic forces the joint possible-world enumeration
-- rather than the single-aggregate fast path.
CREATE TABLE an_n(g int, tag text, v int);
INSERT INTO an_n VALUES (1,'a',10),(1,'b',20);
SELECT add_provenance('an_n');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM an_n; END $$;

-- sum(CASE ...) contributes only for tag='b'.  World {a}: sum is NULL, so
-- NULL + 1 >= 1 is NULL -> false.  Worlds {b} and {a,b} hold: 0.25+0.25.
CREATE TABLE an_ns AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM an_n GROUP BY g
  HAVING sum(CASE WHEN tag='b' THEN v END) + count(*) >= 1;
SELECT remove_provenance('an_ns');
SELECT 'sum(CASE)+count(*)>=1' AS having, p FROM an_ns;
DROP TABLE an_ns;

-- min agrees with sum: the same 0.5, not the 0.75 an empty-sum-is-0 reading
-- would give.
CREATE TABLE an_nm AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM an_n GROUP BY g
  HAVING min(CASE WHEN tag='b' THEN v END) + count(*) >= 1;
SELECT remove_provenance('an_nm');
SELECT 'min(CASE)+count(*)>=1' AS having, p FROM an_nm;
DROP TABLE an_nm;

-- count(*) alone still sees every row, NULL-valued or not: all three
-- non-empty worlds have count(*) >= 1 -> 0.75.
CREATE TABLE an_nc AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM an_n GROUP BY g
  HAVING count(*) + count(*) >= 2;
SELECT remove_provenance('an_nc');
SELECT 'count(*)+count(*)>=2' AS having, p FROM an_nc;
DROP TABLE an_nc;

DROP TABLE an_n;
