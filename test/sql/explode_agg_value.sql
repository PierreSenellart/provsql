\set ECHO none
\pset format unaligned

-- The value of an aggregate is not one value of the database but one per
-- possible world, so grouping rows by it, or deduplicating on it, is no
-- operation on the data as it is: the aggregate is exploded into one row per
-- value it takes, each annotated by the comparison [aggregate = value].  The
-- rows of one group are then pairwise exclusive and exactly one of them is in
-- each world where the group is.

CREATE TABLE eav(g int, v int);
INSERT INTO eav VALUES (1, 10), (1, 20), (2, 30), (3, 40), (3, 50);
SELECT add_provenance('eav');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eav; END $$;

-- GROUP BY on a count: group 1 counts 1 or 2, group 2 counts 1, group 3
-- counts 1 or 2, so the count is 1 unless every group has both its rows
-- (1 - 1/8 = 0.875) and 2 as soon as one of groups 1 and 3 has both
-- (1 - 3/4 * 3/4 = 0.4375).
CREATE TABLE eav_c AS
  SELECT c, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT g, count(*) AS c FROM eav GROUP BY g) s GROUP BY c;
SELECT remove_provenance('eav_c');
SELECT * FROM eav_c ORDER BY c;
DROP TABLE eav_c;

-- DISTINCT on the same count: the same rows, deduplicated on the value.
CREATE TABLE eav_d AS
  SELECT DISTINCT c FROM (SELECT g, count(*) AS c FROM eav GROUP BY g) s;
SELECT remove_provenance('eav_d');
SELECT * FROM eav_d ORDER BY c;
DROP TABLE eav_d;

-- GROUP BY on a max: each value is the maximum of the world where only its
-- own row is (10 with the row of 20 gone: 0.25), and a value that is the
-- largest of its group is the maximum as soon as its row is (0.5).
CREATE TABLE eav_m AS
  SELECT m, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT g, max(v) AS m FROM eav GROUP BY g) s GROUP BY m;
SELECT remove_provenance('eav_m');
SELECT * FROM eav_m ORDER BY m;
DROP TABLE eav_m;

-- An aggregate over the exploded rows is an aggregate over rows that are
-- uncertain like any others: the displayed value is that of the database as
-- it is (one group of count 1, two groups of count 2), and the expectation is
-- taken over the worlds where the row is.
CREATE TABLE eav_n AS
  SELECT c, count(*)::text AS n,
         round(expected(count(*))::numeric, 6) AS e_n,
         round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT g, count(*) AS c FROM eav GROUP BY g) s GROUP BY c;
SELECT remove_provenance('eav_n');
SELECT * FROM eav_n ORDER BY c;
DROP TABLE eav_n;

-- A whole-table count takes every value from none of its rows to all of them.
CREATE TABLE eav_s AS
  SELECT c, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT count(*) AS c FROM eav) s GROUP BY c;
SELECT remove_provenance('eav_s');
SELECT * FROM eav_s ORDER BY c;
DROP TABLE eav_s;

-- A DISTINCT over the aggregates of its own level: the aggregation moves to a
-- subquery and the DISTINCT deduplicates the values, giving the rows of the
-- GROUP BY above.
CREATE TABLE eav_sd AS
  SELECT DISTINCT count(*) AS c FROM eav GROUP BY g;
SET provsql.active = off;
SELECT c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_sd ORDER BY c;
SET provsql.active = on;
DROP TABLE eav_sd;

-- The same with the grouping column kept: one row per group and per value it
-- takes, each with the probability of the group taking it.
CREATE TABLE eav_sg AS
  SELECT DISTINCT g, count(*) AS c FROM eav GROUP BY g;
SET provsql.active = off;
SELECT g, c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_sg ORDER BY g, c;
SET provsql.active = on;
DROP TABLE eav_sg;

-- A UNION (non-ALL) of aggregate results: the values of both arms are
-- exploded, and the deduplication is over them.  The second arm counts only
-- the rows over 20, so the value 1 is there unless every group of either arm
-- has all its rows (0.875) and 2 as soon as one of them has two (0.4375).
CREATE TABLE eav_u AS
  SELECT count(*) AS c FROM eav GROUP BY g
  UNION
  SELECT count(*) FROM eav WHERE v > 20 GROUP BY g;
SET provsql.active = off;
SELECT c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_u ORDER BY c;
SET provsql.active = on;
DROP TABLE eav_u;

-- EXCEPT and INTERSECT match the rows they remove or keep on those values,
-- which is why the explosion is done in each arm and not on the result: the
-- second arm counts only the rows over 20, so group 1 (whose rows are 10 and
-- 20) has no counterpart there and its values survive the difference, while
-- groups 2 and 3 count the same rows in both arms and cancel.  Over the 32
-- worlds: 1 with probability 0.125 and 2 with 0.1875 for the difference, 0.75
-- and 0.25 for the intersection.
CREATE TABLE eav_e AS
  SELECT count(*) AS c FROM eav GROUP BY g
  EXCEPT
  SELECT count(*) FROM eav WHERE v > 20 GROUP BY g;
SET provsql.active = off;
SELECT c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_e ORDER BY c;
SET provsql.active = on;
DROP TABLE eav_e;

CREATE TABLE eav_i AS
  SELECT count(*) AS c FROM eav GROUP BY g
  INTERSECT
  SELECT count(*) FROM eav WHERE v > 20 GROUP BY g;
SET provsql.active = off;
SELECT c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_i ORDER BY c;
SET provsql.active = on;
DROP TABLE eav_i;

-- NULL is a value of an aggregate over the whole table like any other, and the
-- explosion offers it: the aggregation gives its row in every world, including
-- the world holding none of the rows it reads, and there its sum is NULL.  The
-- first arm keeps no row at all, so NULL is its only value and its row is
-- certain; the second sums one row, so it takes 30 where that row is there and
-- NULL where it is not, one row each -- the second of which SQL returns in that
-- world and ProvSQL did not list (which made the answer silently wrong: the
-- three values of the first arm's kind were found in the campaign's corpora).
-- The row of the NULL value is annotated by no row contributing, which is the
-- comparison count(v) = 0 over the same argument.
CREATE TABLE eav_nul AS
  SELECT 'none' AS k, sum(v) AS s FROM eav WHERE g = 99
  UNION
  SELECT 'one', sum(v) FROM eav WHERE g = 2;
SET provsql.active = off;
SELECT k, s::text AS s, present(provsql) AS present,
       round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_nul ORDER BY k, s;
SET provsql.active = on;
DROP TABLE eav_nul;

-- A set operation whose other arm aggregates nothing at that column is
-- refused: only the values of a whole column, exploded in every arm, are
-- matched together.
SELECT count(*) AS c FROM eav GROUP BY g UNION SELECT 1;
SELECT count(*) AS c FROM eav GROUP BY g EXCEPT SELECT 1;

-- The values of a sum over an integer column are its subset sums, reached by
-- adding its contributions one at a time: group 1 sums 10 and 20, so it takes
-- 10, 20 or 30, group 2 takes 30, and group 3 takes 40, 50 or 90.  Over the 32
-- worlds each of those is one pair of rows away (0.25), except 30, which two
-- groups reach (0.25 + 0.5 - 0.125 = 0.625).
CREATE TABLE eav_s2 AS
  SELECT total, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT g, sum(v) AS total FROM eav GROUP BY g) s GROUP BY total;
SELECT remove_provenance('eav_s2');
SELECT * FROM eav_s2 ORDER BY total;
DROP TABLE eav_s2;

-- A sum over a column that is not an integer is refused: its value is read
-- back through the evaluator's own arithmetic, that of a double, which a
-- subset sum of such numbers does not reach exactly.  So is an avg(), and any
-- other aggregate whose values cannot be enumerated.
CREATE TABLE eav_n(g int, v numeric);
INSERT INTO eav_n VALUES (1, 0.1), (1, 0.2);
SELECT add_provenance('eav_n');
SELECT total FROM (SELECT g, sum(v) AS total FROM eav_n GROUP BY g) s
  GROUP BY total;
DROP TABLE eav_n;
SELECT a FROM (SELECT g, avg(v) AS a FROM eav GROUP BY g) s GROUP BY a;
SELECT DISTINCT avg(v) AS a FROM eav GROUP BY g;

-- A count() counts the rows whose value is not NULL, and the null-padded row
-- of an outer join is not one of them: the count of such a group is 0 although
-- the group is there, so 0 is one of the values to explode it into.  Here the
-- month 'b' has no right row at all and 'a' has one: over the 8 worlds, 0 is
-- reached whenever 'b' is there or 'a' is there without its right row (0.625),
-- and 1 exactly when both of the latter are (0.25).
CREATE TABLE eavl(m text);
CREATE TABLE eavr(m text, x int);
INSERT INTO eavl VALUES ('a'), ('b');
INSERT INTO eavr VALUES ('a', 1);
SELECT add_provenance('eavl');
SELECT add_provenance('eavr');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eavl;
        PERFORM set_prob(provenance(), 0.5) FROM eavr; END $$;
CREATE TABLE eav_z AS
  SELECT c, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT l.m, count(r.x) AS c FROM eavl l LEFT JOIN eavr r ON r.m = l.m
        GROUP BY l.m) s GROUP BY c;
SELECT remove_provenance('eav_z');
SELECT * FROM eav_z ORDER BY c;
DROP TABLE eav_z; DROP TABLE eavl; DROP TABLE eavr;

-- The values of a min() or a max() over a text column are exploded the same
-- way, the value read back through its own type: over the 8 worlds of three
-- rows, 'a' is the maximum of its group only without the 'b' beside it
-- (0.25), while 'b' and 'c' are maxima as soon as they are there (0.5).
CREATE TABLE eav_t(g int, s text);
INSERT INTO eav_t VALUES (1, 'b'), (1, 'a'), (2, 'c');
SELECT add_provenance('eav_t');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eav_t; END $$;
CREATE TABLE eav_tm AS
  SELECT m, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT g, max(s) AS m FROM eav_t GROUP BY g) x GROUP BY m;
SELECT remove_provenance('eav_tm');
SELECT * FROM eav_tm ORDER BY m;
DROP TABLE eav_tm;
CREATE TABLE eav_tn AS SELECT DISTINCT min(s) AS m FROM eav_t GROUP BY g;
SELECT remove_provenance('eav_tn');
SELECT * FROM eav_tn ORDER BY m;
DROP TABLE eav_tn; DROP TABLE eav_t;

-- An arm of a set operation that groups by columns it does not expose: those
-- grouping keys are junk entries of the arm, no columns of it, and a row read
-- from one is no row of the range table (it used to crash the planner).  Each
-- group is a single row here, so the count is 1 in every world.
CREATE TABLE eav_j AS
  SELECT count(*) AS c FROM eav WHERE g < 3 GROUP BY g, v
  UNION
  SELECT count(*) FROM eav WHERE g = 3 GROUP BY g, v;
SELECT remove_provenance('eav_j');
SELECT * FROM eav_j ORDER BY c;
DROP TABLE eav_j;

-- The displayed value of an aggregate reads the rows that hold in the data as
-- it is, and the row of an exploded value holds there by a comparison of
-- aggregate results: its truth is read off the values they record, not over
-- the worlds of what they aggregate (one term per subset of the
-- contributions, which a group of fifteen rows would already make
-- unanswerable).  The answer keeps the value the data gives.
CREATE TABLE eav_many(g int, v int);
INSERT INTO eav_many SELECT 1, i FROM generate_series(1, 15) AS i;
SELECT add_provenance('eav_many');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eav_many; END $$;
CREATE TABLE eav_mx AS
  SELECT g, degree FROM (
    SELECT * FROM (SELECT g, count(*) AS degree FROM eav_many GROUP BY g) foo
    GROUP BY g, degree HAVING degree = MAX(degree)) z
  WHERE present(provenance());
SELECT remove_provenance('eav_mx');
SELECT * FROM eav_mx ORDER BY g, degree;
DROP TABLE eav_mx;
-- and an aggregate over those exploded rows, whose own displayed value reads
-- the same comparisons
CREATE TABLE eav_mn AS
  SELECT count(*)::text AS n FROM (
    SELECT * FROM (SELECT g, count(*) AS degree FROM eav_many GROUP BY g) foo
    GROUP BY g, degree HAVING degree = MAX(degree)) z;
SELECT remove_provenance('eav_mn');
SELECT * FROM eav_mn;
DROP TABLE eav_mn; DROP TABLE eav_many;

-- The plain value of the sum, said explicitly, groups as plain SQL does.
CREATE TABLE eav_p AS
  SELECT total::numeric AS total FROM (SELECT g, sum(v) AS total FROM eav
  GROUP BY g) s GROUP BY total::numeric;
SELECT remove_provenance('eav_p');
SELECT * FROM eav_p ORDER BY total;
DROP TABLE eav_p;

-- An EXPRESSION over the aggregate's value is refused, rather than grouped by
-- the token each row carries -- which is what PostgreSQL would do with it, one
-- group per row, silently: floor(ln(cnt)) takes values of its own, none of them
-- among the contributions the aggregate reads, so there is nothing to
-- enumerate.  Found by difftest on sede/c83bcfbb51, where three rows that SQL
-- puts in one group came back as three groups of one.  Both forms refuse, the
-- bare one and the one whose cast sits inside the expression (which read the
-- token's bytes as a varlena: "compressed lz4 data is corrupt").
SELECT floor(ln(total)) AS k, count(*) AS n
  FROM (SELECT g, sum(v) AS total FROM eav GROUP BY g) s GROUP BY 1;
SELECT floor(ln(total::numeric)) AS k, count(*) AS n
  FROM (SELECT g, sum(v) AS total FROM eav GROUP BY g) s GROUP BY 1;
-- The same over a window RANK, which the check above cannot see: at the point it
-- runs the column is a WindowFunc, neither an aggregate nor yet an agg_token, so
-- nothing identifies it -- and the key then groups by one token per row.  Caught
-- instead once every rewriting has run, where the key's own type answers it.
-- Found by difftest on dba/249853: thirty rows that SQL puts in ONE group (the
-- row_number runs 1..30 and 30/1000 truncates to 0) came back as thirty groups
-- of one, silently.  Neither of us read it as a grouping bug at first -- the
-- query computes sqrt(avg(power(x,2))) and looked like an arithmetic one.
CREATE TABLE eav_rank(i int);
INSERT INTO eav_rank SELECT g FROM generate_series(1, 30) g;
SELECT add_provenance('eav_rank');
SELECT index / 1000 AS k, count(*) AS n
  FROM (SELECT row_number() OVER (ORDER BY i) AS index FROM eav_rank) s GROUP BY 1;
-- Marked plain(), which the refusal names, it groups as SQL does: one group.
CREATE TABLE eav_rk AS
  SELECT plain(index / 1000) AS k, count(*) AS n
    FROM (SELECT row_number() OVER (ORDER BY i) AS index FROM eav_rank) s GROUP BY 1;
SELECT remove_provenance('eav_rk');
SELECT k, n::text AS n FROM eav_rk ORDER BY k;
DROP TABLE eav_rk;
-- And a key that is a plain column of the same subquery is untouched.
CREATE TABLE eav_rk AS
  SELECT i / 1000 AS k, count(*) AS n
    FROM (SELECT i, row_number() OVER (ORDER BY i) AS index FROM eav_rank) s GROUP BY 1;
SELECT remove_provenance('eav_rk');
SELECT k, n::text AS n FROM eav_rk ORDER BY k;
DROP TABLE eav_rk;
SELECT remove_provenance('eav_rank');
DROP TABLE eav_rank;

-- Reading such an expression WITHOUT grouping by it is CARRIED, one row per
-- group: `floor` and `ln` are functions ProvSQL carries, and an aggregate that
-- arrives as a subquery COLUMN now has them swapped onto their counterparts like
-- any other reader.  It was frozen until then -- the arithmetic operators over
-- such a column were swapped and the functions were not.  What is refused is
-- GROUPING by it (just above), which is a different question: the values the
-- expression takes are not among the aggregate's contributions, so there is
-- nothing to explode.  The ORDER BY below reads the stored token's value on the
-- data as it is and says so, which is the ordering policy.
CREATE TABLE eav_expr AS
  SELECT floor(ln(total)) AS k
    FROM (SELECT g, sum(v) AS total FROM eav GROUP BY g) s;
SELECT remove_provenance('eav_expr');
SELECT k FROM eav_expr ORDER BY k;
DROP TABLE eav_expr;

DROP TABLE eav;

-- The TRUTH of a comparison of an aggregate against a constant is one truth
-- per world too, so a select-list comparison, or one in the condition of a
-- CASE whose branches are not aggregates, explodes each row into the truths
-- it takes: the rows are grouped by a two-row (three-row, where the aggregate
-- can have no value) untracked source and the comparison moves into the
-- HAVING, which annotates each copy with the provenance of that truth.
-- Over the rows of ect, each present with probability one half:
--   g = 1, its rows of 1 and 5: count(v) is 1 in each of the two singleton
--     worlds and 2 in the world holding both, so count(v) = 1 weighs 0.5 and
--     its false 0.25; sum(v) > 4 holds in {5} and {1,5}, 0.5, and fails in
--     {1}, 0.25.
--   g = 2, its two rows null-valued: count(v) is 0 in every world the group
--     exists in (0.75), and sum(v) has no value there, so the comparison is
--     unknown with probability 0.75 and its true and false rows are dropped
--     as impossible in every world.
--   g = 3, its single row of 7: true with the row, 0.5.
-- A row of probability zero is a row present in no world, ProvSQL's reading
-- of a zero annotation; the ones that are provably so are dropped outright.
CREATE TABLE ect(g int, v int);
INSERT INTO ect VALUES (1,1), (1,5), (2,NULL), (2,NULL), (3,7);
SELECT add_provenance('ect');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ect; END $$;
CREATE TABLE ect_c AS
  SELECT g, count(v) = 1 AS f, probability(provenance()) AS p
  FROM ect GROUP BY g;
SELECT remove_provenance('ect_c');
SELECT g, coalesce(f::text, 'unknown') AS f, round(p::numeric, 6) AS p
FROM ect_c ORDER BY g, f;
DROP TABLE ect_c;
CREATE TABLE ect_s AS
  SELECT g, sum(v) > 4 AS f, probability(provenance()) AS p
  FROM ect GROUP BY g;
SELECT remove_provenance('ect_s');
SELECT g, coalesce(f::text, 'unknown') AS f, round(p::numeric, 6) AS p
FROM ect_s ORDER BY g, f;
DROP TABLE ect_s;
-- The condition of a CASE of another type than a number: the exploded row
-- holds a truth, so PostgreSQL picks the branch of that truth.
CREATE TABLE ect_case AS
  SELECT g, CASE WHEN count(v) = 1 THEN 'one' ELSE 'other' END AS lbl,
         probability(provenance()) AS p
  FROM ect GROUP BY g;
SELECT remove_provenance('ect_case');
SELECT g, lbl, round(p::numeric, 6) AS p FROM ect_case ORDER BY g, lbl;
DROP TABLE ect_case;
-- Declined, and the value read as plain SQL reads it (one row per group, no
-- (*) marker): two aggregates compared with each other, an aggregate whose
-- NULL-ness is not the reading of "no value" (stddev over a single row), a
-- comparison against a column, and a scalar aggregation, whose single row
-- exists in every world -- including the one where no row of the table is.
SET client_min_messages = error;
CREATE TABLE ect_no AS
  SELECT g, count(v) > sum(v) AS f1, stddev(v) > 1 AS f2, count(v) > g AS f3
  FROM ect GROUP BY g;
CREATE TABLE ect_sc AS SELECT count(v) > 1 AS f FROM ect;
RESET client_min_messages;
SELECT remove_provenance('ect_no');
SELECT g, coalesce(f1::text,'NULL') AS f1, coalesce(f2::text,'NULL') AS f2,
       coalesce(f3::text,'NULL') AS f3 FROM ect_no ORDER BY g;
SELECT remove_provenance('ect_sc');
SELECT * FROM ect_sc;
DROP TABLE ect_no, ect_sc, ect;

-- Through a VIEW, which is where the explosion segfaulted (difftest's
-- five-line reproduction, and four queries of the SQLShare corpus): the entry
-- of a view carries the permission info of the query the rewriter expanded it
-- in, so copying it into the wrapper the explosion builds left an index into a
-- list that query does not have.  Answers what plain SQL answers, 1|1 and
-- 2|2, the counts of the two names being one value each and the sum of a
-- single value being that value.
CREATE TABLE ecv_base(name text);
INSERT INTO ecv_base VALUES ('a'), ('a'), ('b');
SELECT add_provenance('ecv_base');
CREATE VIEW ecv(name, cnt) AS
  SELECT name, count(name) AS cnt FROM ecv_base GROUP BY name;
CREATE TABLE ecv_r AS SELECT cnt, sum(cnt) AS s FROM ecv GROUP BY cnt;
SELECT remove_provenance('ecv_r');
SELECT cnt::text AS cnt, s::text AS s FROM ecv_r ORDER BY 1;
DROP TABLE ecv_r;
-- And the same grouping read through a join of two views, the other shape the
-- corpus holds.
CREATE VIEW ecv2(name, total) AS
  SELECT name, count(*) AS total FROM ecv_base GROUP BY name;
CREATE TABLE ecv_r AS
  SELECT total, sum(cnt) AS s FROM ecv JOIN ecv2 ON ecv.name = ecv2.name
  GROUP BY total;
SELECT remove_provenance('ecv_r');
SELECT total::text AS total, s::text AS s FROM ecv_r ORDER BY 1;
DROP TABLE ecv_r;
DROP VIEW ecv2, ecv;
SELECT remove_provenance('ecv_base');
DROP TABLE ecv_base;

-- TWO aggregate columns in a set-operation arm.  Each column is exploded into
-- the values its aggregate takes, and whether NULL is among them is read off
-- that column's own Aggref -- so the columns have to be exploded in ONE pass:
-- a pass that had wrapped the arm first would leave the next column a plain Var
-- of that wrapper, with no Aggref to read and nowhere to put the companion
-- count, and NULL would be a candidate for the first column only.  It was, and
-- the row of the world where the arm holds NO row -- both sums NULL -- was
-- therefore never produced, its probability carried by nothing.  difftest's
-- sede/2b7555de26, whose arms are SUM, SUM against COUNT(CASE), verdict B wrong.
-- Two rows per arm at one half, so four worlds each, a quarter apiece.
--   arm A, sum(v) and sum(g) over (10,1) and (20,2):
--     no row -> (NULL, NULL) | {10,1} -> (10,1) | {20,2} -> (20,2) | both -> (30,3)
--   arm B, count(CASE x>4) and count(CASE x>5) over 5 and 6, never NULL:
--     no row -> (0,0) | {5} -> (1,0) | {6} -> (1,1) | both -> (2,1)
-- Eight rows at 0.25, and they sum to 2 -- one row of each arm in every world.
-- The combinations no world realises come back with a provenance of zero, which
-- says of itself that no world holds them; the rows below are the ones that do.
CREATE TABLE eua(v int, g int);
INSERT INTO eua VALUES (10, 1), (20, 2);
CREATE TABLE eub(x int);
INSERT INTO eub VALUES (5), (6);
SELECT add_provenance('eua');
SELECT add_provenance('eub');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eua; END $$;
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eub; END $$;
CREATE TABLE eua_r AS
  SELECT src, a, b, probability(provenance()) AS p FROM (
    SELECT 'A' AS src, sum(v) AS a, sum(g) AS b FROM eua
    UNION
    SELECT 'B', count(CASE WHEN x > 4 THEN 1 ELSE NULL END),
                count(CASE WHEN x > 5 THEN 1 ELSE NULL END) FROM eub) z;
SELECT remove_provenance('eua_r');
SELECT src, a, b, round(p::numeric, 6) AS p FROM eua_r WHERE p > 0
  ORDER BY src, a NULLS FIRST, b;
SELECT round(sum(p)::numeric, 6) AS total_mass FROM eua_r;
DROP TABLE eua_r;
SELECT remove_provenance('eua');
SELECT remove_provenance('eub');
DROP TABLE eua, eub;

-- The swap itself, over an aggregate that arrives as a subquery column: the
-- readers ProvSQL carries keep the token, where until now only the arithmetic
-- operators did.  `a / b` was carried and `round(a / b, 2)` was not -- found by
-- diagnosing the eleven queries prevalence had counted under `round`, where the
-- function was only the neighbourhood and the argument was what froze.  An
-- operator needs no swap because `/` is declared over agg_token; `round` is not,
-- the counterparts being named provsql_round and provsql_abs so as not to shadow
-- pg_catalog (abs('0.20') would be ambiguous otherwise).  The rename and the
-- swap look like opposite decisions and are the same one.
-- Two rows in one group at one half: sum(v) is 40 and count(*) is 2 on the data
-- as it is, so the values below are those of 40 and of 40/2 = 20, each carrying
-- the token.  What must NOT change is the value: every one is the number plain
-- SQL gives.
CREATE TABLE eavs(g int, v int);
INSERT INTO eavs VALUES (1, 10), (1, 30);
SELECT add_provenance('eavs');
CREATE TABLE eavs_r AS
  SELECT round(a, 2) AS r, abs(a) AS ab, ceil(a) AS ce, floor(a) AS fl,
         round(a / b, 2) AS rd, sqrt(a / b) AS sq, ln(a) AS l, exp(b) AS e
    FROM (SELECT sum(v)::numeric AS a, count(*)::numeric AS b FROM eavs) z;
SELECT remove_provenance('eavs_r');
SELECT r::text AS r, ab::text AS ab, ce::text AS ce, fl::text AS fl,
       rd::text AS rd, sq::text AS sq, round(l::numeric,6)::text AS l,
       round(e::numeric,6)::text AS e FROM eavs_r;
DROP TABLE eavs_r;
-- The same read with the rewriting off: the values have to be identical.
SET provsql.active = off;
SELECT round(a, 2)::text AS r, abs(a)::text AS ab, ceil(a)::text AS ce,
       floor(a)::text AS fl, round(a / b, 2)::text AS rd, sqrt(a / b)::text AS sq,
       round(ln(a)::numeric,6)::text AS l, round(exp(b)::numeric,6)::text AS e
  FROM (SELECT sum(v)::numeric AS a, count(*)::numeric AS b FROM eavs) z;
SET provsql.active = on;
-- `power` over an aggregate that arrives as a column is carried by the same
-- swap (the POW gate, as the `^` operator), while `trunc` is still frozen, for
-- want of a counterpart rather than of the swap: it would need an arithmetic
-- op of its own.  The warning is trunc's.
SELECT power(a, 2) AS p, trunc(a, 1) AS t
  FROM (SELECT sum(v)::numeric AS a FROM eavs) z;
SELECT remove_provenance('eavs');
DROP TABLE eavs;
