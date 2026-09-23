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

-- IS [NOT] NULL of an expression over aggregates is a truth per world, the
-- value it tests having one per world, so it explodes into the two rows those
-- truths give -- as a comparison against a constant does.  Read on the datum
-- instead it answered from the agg_token, which is never the null datum (the
-- row is there), and so said NOT NULL of a value that is null, silently.
-- Two rows and not three: a value either is null in a world or is not.
CREATE TABLE anx(g int, a int, b int);
INSERT INTO anx VALUES (1, 5, 2), (2, 7, 0), (3, 9, NULL);
SELECT add_provenance('anx');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM anx; END $$;
-- One row per group at one half.  sum(b) is null only for group 3, whose only
-- b is NULL, so the matching truth carries the group's own half and the other
-- carries nothing.
CREATE TABLE anx_r AS
  SELECT g, sum(b) IS NULL AS n, probability(provenance()) AS p
  FROM anx GROUP BY g;
SELECT remove_provenance('anx_r');
SELECT g, n, round(p::numeric, 6) AS p FROM anx_r ORDER BY g, n;
DROP TABLE anx_r;
-- A division is null where its divisor reads zero, which is not a nullness of
-- either operand, so the null gate cannot build it operand by operand: refused
-- by name rather than answered wrongly.  A gap against the semantics, which
-- gives a division by zero the value null and so has the test in the fragment,
-- and not a deviation: closing it needs the nullness of a division to be "an
-- operand is null OR the divisor reads zero".
SELECT g, (sum(a)/sum(b)) IS NULL FROM anx GROUP BY g;
-- As a SORT KEY the same test is not exploded -- a key is no answer -- and it
-- orders the rows on whether the value is null in the database as it is, which
-- is what a sort on an aggregate's value does.  That has to be SAID, though:
-- ordering on the value warns, and on a comparison warns, and this warned
-- nothing until the sort pass learned to look inside a boolean key.  Group 3 is
-- the null one, so it comes first under DESC.
-- Materialised so the rows carry no provenance column: printing one would print
-- a uuid derived from the per-run input tokens, which differs between runs.  The
-- order is read back by ctid, the insertion order of a table just created.
CREATE TABLE anx_o AS
  SELECT g FROM anx GROUP BY g ORDER BY (sum(b) IS NULL) DESC, g;
SELECT remove_provenance('anx_o');
SELECT string_agg(g::text, ',' ORDER BY ctid) AS sorted FROM anx_o;
DROP TABLE anx_o;
-- The same test in a SCALAR aggregation, which has no grouping to explode the
-- truths over: it answers on the database as it is -- sum(b) is 2 there, so
-- false -- where the worlds holding neither the b of group 1 nor that of group
-- 2, a quarter of the eight, make the sum null and the truth true.  That is the
-- right answer for that one world and says nothing about the others, which is
-- what every plain reading of an aggregate is reported for, and this one was
-- reported by nothing: a comparison in the same position is caught through the
-- frozen value it reads, and a null test reads none -- it is answered by the
-- token itself, which provenance_aggregate returns as the null datum exactly
-- where the aggregate has no value in the data as it is.
CREATE TABLE anx_s AS SELECT sum(b) IS NULL AS n FROM anx;
SELECT remove_provenance('anx_s');
SELECT n FROM anx_s;
DROP TABLE anx_s;
CREATE TABLE anx_s AS SELECT sum(b) IS NOT NULL AS n FROM anx;
SELECT remove_provenance('anx_s');
SELECT n FROM anx_s;
DROP TABLE anx_s;
-- plain() says so, and silences the report, as it does for every other reading.
CREATE TABLE anx_s AS SELECT plain(sum(b)) IS NULL AS n FROM anx;
SELECT remove_provenance('anx_s');
SELECT n FROM anx_s;
DROP TABLE anx_s;
DROP TABLE anx;
