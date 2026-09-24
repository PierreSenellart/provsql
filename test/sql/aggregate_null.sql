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
-- A division is null where its divisor reads zero, which is no operand's
-- nullness: the gate ORs one more term onto the operands' own, the comparison
-- of the divisor against zero, read per world.  The semantics has the test in
-- the fragment -- a division by zero is null there, by the convention that
-- totalizes a function undefined at zero -- so a refusal here was a gap against
-- it and not a deviation.  SQL raises rather than answering null, in the worlds
-- where the divisor cancels only, which is the convention to argue with.
-- Read through a materialised table, as everything above: the truth gates are
-- derived from the per-run input tokens, so printing a provenance column here
-- would print a different uuid at every run.
-- The three reasons, one per group, each row at one half: group 1 divides 5 by 2
-- and is never null (false carries the group's half, true nothing); group 2
-- divides 7 by a b of 0, null for the divisor -- the reason that is ours and not
-- SQL's, which raises there; group 3 sums a single NULL b, null for an operand.
CREATE TABLE anx_d AS
  SELECT g, (sum(a)/sum(b)) IS NULL AS n, probability(provenance()) AS p
  FROM anx GROUP BY g;
SELECT remove_provenance('anx_d');
SELECT g, n, round(p::numeric, 6) AS p FROM anx_d ORDER BY g, n;
DROP TABLE anx_d;
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
-- Two rows in one group at one half, whose b cancels when both are there:
-- the empty world has no group at all (a quarter), {10/1} and {20/-1} divide
-- fine (a half together), and {30/0} is the null one (a quarter).  So IS NULL
-- carries 0.25 and IS NOT NULL 0.5, and the two add up to the 0.75 the group
-- exists with -- neither operand being null in any of those worlds, which is
-- what a strictness-only reading would have answered 0 for.
CREATE TABLE anz(g int, a int, b int);
INSERT INTO anz VALUES (1, 10, 1), (1, 20, -1);
SELECT add_provenance('anz');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM anz; END $$;
CREATE TABLE anz_r AS
  SELECT g, (sum(a)/sum(b)) IS NULL AS n, probability(provenance()) AS p
  FROM anz GROUP BY g;
SELECT remove_provenance('anz_r');
SELECT g, n, round(p::numeric, 6) AS p FROM anz_r ORDER BY n;
DROP TABLE anz_r;
CREATE TABLE anz_r AS
  SELECT g, (sum(a)/sum(b)) IS NOT NULL AS n, probability(provenance()) AS p
  FROM anz GROUP BY g;
SELECT remove_provenance('anz_r');
SELECT g, n, round(p::numeric, 6) AS p FROM anz_r ORDER BY n;
DROP TABLE anz_r;
-- The same in a HAVING, which keeps the one truth it asks for.
CREATE TABLE anz_r AS
  SELECT g, probability(provenance()) AS p
  FROM anz GROUP BY g HAVING (sum(a)/sum(b)) IS NULL;
SELECT remove_provenance('anz_r');
SELECT g, round(p::numeric, 6) AS p FROM anz_r;
DROP TABLE anz_r;
CREATE TABLE anz_r AS
  SELECT g, probability(provenance()) AS p
  FROM anz GROUP BY g HAVING (sum(a)/sum(b)) IS NOT NULL;
SELECT remove_provenance('anz_r');
SELECT g, round(p::numeric, 6) AS p FROM anz_r;
DROP TABLE anz_r;
-- A PLAIN divisor is the same number in every world, so the added term settles
-- rather than exploding: 2 is never zero, the truth is false wherever the group
-- is there (0.75) and true nowhere.  Refused before, for want of the term.
CREATE TABLE anz_r AS
  SELECT g, (sum(a)/2) IS NULL AS n, probability(provenance()) AS p
  FROM anz GROUP BY g;
SELECT remove_provenance('anz_r');
SELECT g, n, round(p::numeric, 6) AS p FROM anz_r ORDER BY n;
DROP TABLE anz_r;
SELECT remove_provenance('anz');
DROP TABLE anz;
DROP TABLE anx;
-- The nullness of a function ProvSQL carries is its argument's, the function
-- being strict, so sqrt(sum(x)) IS NULL is the sum's own nullness.  It was
-- refused by name until the reading was lifted, and the refusal outlived its
-- reason: here the NullTest's argument is still the pg_catalog sqrt over the
-- aggregate, not the counterpart the target list swaps in, so asking for that
-- swap is what says ProvSQL carries the function.
-- One row in group 1 and two in group 2, each at one half.  The sum is null in
-- no world where the group is there, so false carries the group's own
-- probability -- 0.5 and 0.75 -- and true carries nothing.
CREATE TABLE anw(g int, x int);
INSERT INTO anw VALUES (1, 4), (2, 1), (2, 3);
SELECT add_provenance('anw');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM anw; END $$;
CREATE TABLE anw_r AS
  SELECT g, sqrt(sum(x)) IS NULL AS n, probability(provenance()) AS p
  FROM anw GROUP BY g;
SELECT remove_provenance('anw_r');
SELECT g, n, round(p::numeric, 6) AS p FROM anw_r ORDER BY g, n;
DROP TABLE anw_r;
SELECT remove_provenance('anw');
DROP TABLE anw;
