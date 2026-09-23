\set ECHO none
\pset format unaligned

-- Arithmetic on aggregate results (issue #63)
-- These queries require casts from agg_token to the original aggregate type

-- Multiplication by constant
CREATE TABLE agg_arith1 AS
  SELECT position, count(*) * 10 AS scaled_count FROM personnel GROUP BY position;
SELECT remove_provenance('agg_arith1');
SELECT * FROM agg_arith1 ORDER BY position;
DROP TABLE agg_arith1;

-- Division by constant
CREATE TABLE agg_arith2 AS
  SELECT position, count(*) / 2.0 AS half_count FROM personnel GROUP BY position;
SELECT remove_provenance('agg_arith2');
SELECT * FROM agg_arith2 ORDER BY position;
DROP TABLE agg_arith2;

-- Addition of constant
CREATE TABLE agg_arith3 AS
  SELECT position, count(*) + 100 AS offset_count FROM personnel GROUP BY position;
SELECT remove_provenance('agg_arith3');
SELECT * FROM agg_arith3 ORDER BY position;
DROP TABLE agg_arith3;

-- AVG with multiplication
CREATE TABLE agg_arith4 AS
  SELECT AVG(id) * 2 AS doubled_avg FROM personnel;
SELECT remove_provenance('agg_arith4');
SELECT * FROM agg_arith4;
DROP TABLE agg_arith4;

-- SUM with arithmetic
CREATE TABLE agg_arith5 AS
  SELECT position, SUM(id) + 1 AS sum_plus_one FROM personnel GROUP BY position;
SELECT remove_provenance('agg_arith5');
SELECT * FROM agg_arith5 ORDER BY position;
DROP TABLE agg_arith5;

-- Explicit cast of aggregate result to numeric
CREATE TABLE agg_arith_cast AS
  SELECT city, count(*)::numeric AS cnt_numeric FROM personnel GROUP BY city;
SELECT remove_provenance('agg_arith_cast');
SELECT * FROM agg_arith_cast ORDER BY city;
DROP TABLE agg_arith_cast;

-- Arithmetic on aggregate from subquery
CREATE TABLE agg_arith_sub AS
  SELECT city, cnt + 1 AS plus_one
  FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_sub');
SELECT * FROM agg_arith_sub ORDER BY city;
DROP TABLE agg_arith_sub;

-- Window function over aggregate from subquery
CREATE TABLE agg_arith_win AS
  SELECT city, cnt, SUM(cnt) OVER () AS total
  FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_win');
SELECT * FROM agg_arith_win ORDER BY city;
DROP TABLE agg_arith_win;

-- COALESCE on aggregate from subquery
CREATE TABLE agg_arith_coalesce AS
  SELECT city, COALESCE(cnt, 0) AS cnt
  FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_coalesce');
SELECT * FROM agg_arith_coalesce ORDER BY city;
DROP TABLE agg_arith_coalesce;

-- GREATEST on aggregate from subquery
CREATE TABLE agg_arith_greatest AS
  SELECT city, GREATEST(cnt, 3) AS at_least_3
  FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_greatest');
SELECT * FROM agg_arith_greatest ORDER BY city;
DROP TABLE agg_arith_greatest;

-- A GREATEST nested in another (or in a LEAST): the inner one becomes an
-- agg_case, which the outer MinMaxExpr reads as any other agg_token argument
-- -- and it must be CAST to read it, the outer node's own operator being one
-- on numbers.  Regression for a value made of the token's bytes read as a
-- numeric (a 66 KB digit string, or "compressed lz4 data is corrupt" under
-- detoasting), which is what an uncast agg_token argument gave.  Both values
-- here are the same in every world (the overflow bounds swallow the
-- aggregate), so this checks the reading of the token and not the tracking:
-- agg_case has the nested selections whose value varies.
CREATE TABLE agg_arith_nested AS
  SELECT GREATEST(+9223372036854775807,
                  LEAST(-9223372036854775808, sum(id::numeric * id))) AS g,
         GREATEST(3, LEAST(2, count(*))) AS lo
  FROM personnel;
SELECT remove_provenance('agg_arith_nested');
SELECT * FROM agg_arith_nested;
DROP TABLE agg_arith_nested;

-- GREATEST / LEAST of two aggregates, which PostgreSQL casts to the type they
-- share: the arms carry two casts then (the aggregate pass's back to the
-- aggregate's own type, and that one), and the conditions of the CASE they
-- become must still see the aggregates themselves.  Tracked, and checked
-- against the four worlds of two rows at 1/2: the values are 3, 4 and 7, so
-- 14/3 in expectation once the empty world is excluded.
CREATE TABLE agg_arith_two(val bigint);
INSERT INTO agg_arith_two VALUES (3),(4);
SELECT add_provenance('agg_arith_two');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM agg_arith_two; END $$;
CREATE TABLE agg_arith_two_r AS
  SELECT greatest(sum(val), min(val)) AS g,
         round(expected(greatest(sum(val), min(val)))::numeric, 6) AS e,
         greatest(avg(val), min(val)) AS ga,
         greatest(min(val), max(val)) AS gm
  FROM agg_arith_two;
SELECT remove_provenance('agg_arith_two_r');
SELECT * FROM agg_arith_two_r;
DROP TABLE agg_arith_two_r;
SELECT remove_provenance('agg_arith_two');
DROP TABLE agg_arith_two;

-- Arithmetic on AVG (numeric type) from subquery
CREATE TABLE agg_arith_avg AS
  SELECT city, avg_id * 2 AS doubled
  FROM (SELECT city, AVG(id) AS avg_id FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_avg');
SELECT * FROM agg_arith_avg ORDER BY city;
DROP TABLE agg_arith_avg;

-- String aggregate with concatenation
CREATE TABLE agg_arith6 AS
  SELECT city, string_agg(name, ', ' ORDER BY name) || ' (team)' AS team
    FROM personnel GROUP BY city;
SELECT remove_provenance('agg_arith6');
SELECT * FROM agg_arith6 ORDER BY city;
DROP TABLE agg_arith6;

-- The arith gate records its computed scalar in extra (agg_arith_make),
-- so agg_token_value_text recovers the "value (*)" display from the
-- bare UUID for arithmetic results, as it does for plain aggregates
-- (this is what Studio renders in result tables).
CREATE TABLE agg_arith_disp AS
  SELECT city, 2.0 * COUNT(*) + MAX(id) AS expr
    FROM personnel GROUP BY city;
SELECT remove_provenance('agg_arith_disp');
SELECT provsql.agg_token_value_text(provsql.agg_token_uuid(expr)) AS disp
FROM agg_arith_disp ORDER BY 1;
DROP TABLE agg_arith_disp;

-- COALESCE, GREATEST and NULLIF over an aggregate of the same query: each is
-- the CASE it means and all three are tracked (see agg_case).  NULLIF(COUNT(*),
-- 1) is NULL in the worlds where the city holds exactly one row and the count
-- elsewhere, which is why the column is a tracked value rather than the count
-- read as a plain one.
CREATE TABLE agg_arith_cg AS
  SELECT city, COALESCE(SUM(id), 0) AS s, GREATEST(COUNT(*), 3) AS g,
         NULLIF(COUNT(*), 1) AS n
  FROM personnel GROUP BY city;
SELECT remove_provenance('agg_arith_cg');
SELECT city, s, g, n FROM agg_arith_cg ORDER BY city;
DROP TABLE agg_arith_cg;

-- A boolean aggregate where a boolean is read (a CASE condition, AND, OR,
-- NOT) or compared: the agg_token is cast back to boolean, not read as one.
CREATE TABLE agg_arith_bool AS
  SELECT city,
         CASE WHEN bool_or(position = 'Director') THEN 'yes' ELSE 'no' END AS has_dir,
         bool_or(position = 'Director') = true AS eq_true,
         NOT every(position = 'Director') AS not_every,
         CASE count(*) WHEN 2 THEN 'two' ELSE 'other' END AS simple_case
  FROM personnel GROUP BY city;
SELECT remove_provenance('agg_arith_bool');
SELECT city, has_dir, eq_true, not_every, simple_case FROM agg_arith_bool ORDER BY city;
DROP TABLE agg_arith_bool;

-- Division of integers truncates toward zero, in the displayed value as in
-- SQL; a numeric operand makes it a numeric division.
CREATE TABLE agg_arith_div AS
  SELECT city, COUNT(*) / 2 AS half, SUM(id) / 3 AS third,
         COUNT(*) / 2.0 AS half_num, 7 / COUNT(*) AS inv, -COUNT(*) / 2 AS neg
  FROM personnel GROUP BY city;
SELECT remove_provenance('agg_arith_div');
SELECT city, half, third, half_num, inv, neg FROM agg_arith_div ORDER BY city;
DROP TABLE agg_arith_div;

-- An aggregate of a type agg_token has no cast to (a date) is read through
-- the text of its value: casts of it, in the query and on a subquery column,
-- and a series bounded by such aggregates.
CREATE TABLE agg_arith_dates(label text, d date);
INSERT INTO agg_arith_dates VALUES
  ('a','2020-01-15'),('a','2020-03-10'),('b','2020-02-01');
CREATE TABLE agg_arith_dates_plain AS SELECT * FROM agg_arith_dates;
SELECT add_provenance('agg_arith_dates');
CREATE TABLE agg_arith_dc AS
  SELECT label, min(d)::timestamp AS ts, min(d)::text AS txt
  FROM agg_arith_dates GROUP BY label;
SELECT remove_provenance('agg_arith_dc');
SELECT label, ts, txt FROM agg_arith_dc ORDER BY label;
DROP TABLE agg_arith_dc;
CREATE TABLE agg_arith_dc AS
  SELECT t2.label, g::date AS month
  FROM (SELECT max(d) AS max FROM agg_arith_dates) t1
  CROSS JOIN (SELECT label, min(d) AS min FROM agg_arith_dates GROUP BY label) t2
  CROSS JOIN LATERAL generate_series(t2.min::timestamp, t1.max::timestamp,
                                     '1 month') g;
SELECT remove_provenance('agg_arith_dc');
SELECT label, month FROM agg_arith_dc ORDER BY label, month;
SELECT t2.label, g::date AS month
FROM (SELECT max(d) AS max FROM agg_arith_dates_plain) t1
CROSS JOIN (SELECT label, min(d) AS min FROM agg_arith_dates_plain GROUP BY label) t2
CROSS JOIN LATERAL generate_series(t2.min::timestamp, t1.max::timestamp,
                                   '1 month') g
ORDER BY label, month;
DROP TABLE agg_arith_dc, agg_arith_dates, agg_arith_dates_plain;

-- The displayed value of an aggregate reads only the rows that hold in the
-- database as it is: not the groups a HAVING of a subquery rejects.
CREATE TABLE agg_arith_hv AS
  SELECT count(*) AS n, string_agg(city, ',' ORDER BY city) AS cities
  FROM (SELECT city FROM personnel GROUP BY city HAVING count(*) <= 2) t;
SELECT remove_provenance('agg_arith_hv');
SELECT n, cities FROM agg_arith_hv;
DROP TABLE agg_arith_hv;

-- Functions over aggregate results: round, floor, ceil and abs are carried as
-- gate operations, so they stay tracked (their value is read in every world);
-- the others read the value of the aggregate as it is on the database -- an
-- array aggregate compared to an array, a series between aggregates of a
-- subquery, and a timestamp minus a subquery's min (a type agg_token has no
-- cast to).  Floating-point arithmetic stays as the query wrote it, and
-- ProvSQL's own functions still receive the agg_token.
CREATE TABLE agg_arith_fn AS
  SELECT round(s * 100.0 / 3, 2) AS r, abs(-s * 1.0) AS a,
         round((CAST(s AS real) / 3)::numeric, 6) AS f, expected(c) AS e
  FROM (SELECT sum(id) AS s, count(*) AS c FROM personnel) t;
SELECT remove_provenance('agg_arith_fn');
SELECT r, a, f, e FROM agg_arith_fn;
DROP TABLE agg_arith_fn;
CREATE TABLE agg_arith_fn AS
  SELECT array_agg(id ORDER BY id) = ARRAY[1,2,3,4,5,6,7] AS eq,
         cardinality(array_agg(id)) AS n, round((CAST(count(*) AS real) / 3)::numeric, 6) AS f
  FROM personnel;
SELECT remove_provenance('agg_arith_fn');
SELECT eq, n, f FROM agg_arith_fn;
DROP TABLE agg_arith_fn;
CREATE TABLE agg_arith_fn AS
  SELECT g FROM (SELECT min(id) AS lo, max(id) AS hi FROM personnel) m,
                generate_series(m.lo, m.hi, 3) g;
SELECT remove_provenance('agg_arith_fn');
SELECT g FROM agg_arith_fn ORDER BY g;
DROP TABLE agg_arith_fn;
CREATE TABLE agg_arith_ts(id int, t timestamp);
INSERT INTO agg_arith_ts VALUES (1,'2020-01-01'),(2,'2020-01-03');
SELECT add_provenance('agg_arith_ts');
CREATE TABLE agg_arith_fn AS
  SELECT id, t - m.first AS d
  FROM agg_arith_ts, (SELECT min(t) AS first FROM agg_arith_ts) m;
SELECT remove_provenance('agg_arith_fn');
SELECT id, d FROM agg_arith_fn ORDER BY id;
DROP TABLE agg_arith_fn, agg_arith_ts;

-- A comparison in WHERE on a subquery's aggregate filters the rows an
-- aggregation reads, not its groups (plain SQL: 1 city, Paris, with 3).
CREATE TABLE agg_arith_fn AS
  SELECT p.city, count(*) AS n
  FROM personnel p JOIN (SELECT city, count(*) AS c FROM personnel
                         GROUP BY city) t ON p.city = t.city
  WHERE t.c >= 3 GROUP BY p.city;
SELECT remove_provenance('agg_arith_fn');
SELECT city, n FROM agg_arith_fn ORDER BY city;
DROP TABLE agg_arith_fn;

-- A cast with a type modifier (a two-argument function) of arithmetic on a
-- subquery's aggregate reads its value (it read the agg_token as a numeric).
CREATE TABLE agg_arith_fn AS
  SELECT CAST(r AS DECIMAL(10,3)) AS r
  FROM (SELECT count(*) * 1.0 / 3 AS r FROM personnel) t;
SELECT remove_provenance('agg_arith_fn');
SELECT r FROM agg_arith_fn;
DROP TABLE agg_arith_fn;

-- CASE over a subquery's aggregates: the branches are read as values of the
-- CASE's type, a boolean aggregate as the condition (it read the agg_token
-- as a numeric).
CREATE TABLE agg_arith_fn AS
  SELECT city, CASE WHEN c > 2 THEN m ELSE 0 END AS v,
         CASE WHEN has_dir THEN 'yes' ELSE 'no' END AS d
  FROM (SELECT city, count(*) AS c, max(id) AS m,
               bool_or(position = 'Director') AS has_dir
        FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_fn');
SELECT city, v, d FROM agg_arith_fn ORDER BY city;
DROP TABLE agg_arith_fn;

-- A simple CASE on a window value of a CTE, and windows partitioned or
-- ordered by aggregate results: on the plain values, with a warning.
-- (Before PostgreSQL 11, rank() is not tracked and warns otherwise: the
-- warnings are left out, the values are the same.)
SET client_min_messages = error;
CREATE TABLE agg_arith_fn AS
  WITH w AS (SELECT id, rank() OVER (ORDER BY id) AS rn FROM personnel)
  SELECT id, CASE rn WHEN 1 THEN 'first' WHEN 2 THEN 'second' END AS d
  FROM w;
RESET client_min_messages;
SELECT remove_provenance('agg_arith_fn');
SELECT id, d FROM agg_arith_fn WHERE d IS NOT NULL ORDER BY id;
DROP TABLE agg_arith_fn;
CREATE TABLE agg_arith_fn AS
  SELECT city, c, rank() OVER (ORDER BY c DESC, city) AS r,
         sum(c) OVER (PARTITION BY c) AS s
  FROM (SELECT city, count(*) AS c FROM personnel GROUP BY city) t;
SELECT remove_provenance('agg_arith_fn');
SELECT city, c, r, s FROM agg_arith_fn ORDER BY city;
DROP TABLE agg_arith_fn;

-- The circuit of an integer division truncates as SQL does, in every world:
-- over five rows at probability 1/2, count(*) / 2 = 1 when 2 or 3 rows are
-- present, with probability (10 + 10) / 32 = 0.625.
CREATE TABLE agg_arith_id(id int);
INSERT INTO agg_arith_id SELECT generate_series(1, 5);
SELECT add_provenance('agg_arith_id');
SELECT set_prob(provsql, 0.5) FROM agg_arith_id \g /dev/null
CREATE TABLE agg_arith_fn AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM (SELECT count(*) / 2 AS c FROM agg_arith_id) s WHERE c = 1;
SELECT remove_provenance('agg_arith_fn');
SELECT p FROM agg_arith_fn;
DROP TABLE agg_arith_fn, agg_arith_id;

-- A parameter of type "any" (pg_typeof) takes a stored agg_token as it is.
CREATE TABLE agg_arith_fn AS
  SELECT city, count(*) AS c FROM personnel GROUP BY city;
CREATE TABLE agg_arith_ty AS SELECT pg_typeof(c)::text AS t FROM agg_arith_fn;
SELECT remove_provenance('agg_arith_ty');
SELECT DISTINCT t FROM agg_arith_ty;
DROP TABLE agg_arith_ty, agg_arith_fn;

-- A division by zero of an aggregate, on a row kept for the other worlds
-- (its WHERE fails in the database as it is), is a NULL value, not an error.
CREATE TABLE agg_arith_fn AS
  SELECT p.city, c.n, 10 / c.n AS q
  FROM (SELECT DISTINCT city FROM personnel) p
       JOIN LATERAL (SELECT count(*) AS n FROM personnel r
                     WHERE r.city = p.city AND r.id > 5) c ON true
  WHERE c.n > 0;
SELECT remove_provenance('agg_arith_fn');
SELECT city, n, q FROM agg_arith_fn ORDER BY city;
DROP TABLE agg_arith_fn;

-- Arithmetic on aggregates of another type than a number stays as written:
-- a timestamp minus a timestamp.
CREATE TABLE agg_arith_ts(g int, t timestamp);
INSERT INTO agg_arith_ts VALUES (1,'2020-01-01'),(1,'2020-01-03'),(2,'2020-02-01');
SELECT add_provenance('agg_arith_ts');
CREATE TABLE agg_arith_fn AS
  SELECT g, CAST(max(t) AS timestamp) - CAST(min(t) AS timestamp) AS d
  FROM agg_arith_ts GROUP BY g;
SELECT remove_provenance('agg_arith_fn');
SELECT g, d FROM agg_arith_fn ORDER BY g;
DROP TABLE agg_arith_fn, agg_arith_ts;

-- A correlated scalar subquery in an expression beside aggregates, a COALESCE
-- in the target list of an ORDER BY ... LIMIT, and a window ordered by a
-- COUNT(DISTINCT) and a column beside it: planned without internal errors, and
-- the rank of two keys is tracked (the subquery counting the groups before
-- each group compares them lexicographically), so the values carry (*).
SET client_min_messages = error;
CREATE TABLE agg_arith_fn AS
  SELECT p.city, count(*)::real / (SELECT count(*) FROM personnel q
                                   WHERE q.city = p.city)::real AS r
  FROM personnel p GROUP BY p.city;
RESET client_min_messages;
SELECT remove_provenance('agg_arith_fn');
SELECT city, r FROM agg_arith_fn ORDER BY city;
DROP TABLE agg_arith_fn;
CREATE TABLE agg_arith_fn AS
  SELECT id, COALESCE(position, 'none') AS pos FROM personnel ORDER BY id LIMIT 2;
SELECT remove_provenance('agg_arith_fn');
SELECT count(*) AS n FROM agg_arith_fn;
DROP TABLE agg_arith_fn;
CREATE TABLE agg_arith_fn AS
  SELECT city, rank() OVER (ORDER BY count(DISTINCT position) DESC, city) AS r
  FROM personnel GROUP BY city;
SELECT remove_provenance('agg_arith_fn');
SELECT city, r FROM agg_arith_fn ORDER BY city;
DROP TABLE agg_arith_fn;

-- The value of an arithmetic gate is READ in the type the query's expression
-- has, which the gate records beside the value it computes in numeric (an agg
-- gate already carried its aggregate's type in the same field).  Without that,
-- a double precision result printed numeric's twenty digits where SQL prints
-- sixteen -- 2.0000000000000000 for sum(x)/3 -- and a numeric multiplication's
-- scale where the expression is a float: every column below is what plain SQL
-- answers, which the second query checks by computing the same thing with the
-- rewriting off.
CREATE TABLE agg_ty(x float8, n numeric, i int);
INSERT INTO agg_ty VALUES (1,1,1), (2,2,2), (3,3,3);
SELECT add_provenance('agg_ty');
CREATE TABLE agg_ty_r AS
  SELECT sum(x) / 3 AS f8_div, sum(x) / 7 AS f8_div7, sum(x) * 2 AS f8_times,
         sum(n) / 3 AS num_div, sum(i) / 3 AS int_div,
         sum(x::float4) / 3 AS f4_div, round(sum(n), 2) AS num_round,
         (count(*) * max(x))::numeric AS float_then_numeric,
         -- The float the gates cannot tell from their children: the query's
         -- own cast, which the rewriting peels off the aggregate (the first)
         -- or coerces away on the other operand (the second), and a cast over
         -- an agg_token where no operand carries a type at all (the third).
         -- Read back through the type, which is what the AS_FLOAT8 gate says.
         100 / CAST(count(*) AS REAL) AS peeled_cast,
         sum(i) * 1.5::float8 AS float_operand,
         sum(i)::float8 / sum(i) AS cast_over_token,
         sqrt(sum(x)) AS f8_sqrt
  FROM agg_ty;
SELECT remove_provenance('agg_ty_r');
SELECT f8_div::text AS f8_div, f8_div7::text AS f8_div7, f8_times::text AS f8_times,
       num_div::text AS num_div, int_div::text AS int_div, f4_div::text AS f4_div,
       num_round::text AS num_round, float_then_numeric::text AS ftn,
       peeled_cast::text AS peeled, float_operand::text AS f_operand,
       cast_over_token::text AS cast_tok, f8_sqrt::text AS f8_sqrt
FROM agg_ty_r;
SET provsql.active = off;
SELECT sum(x) / 3 AS f8_div, sum(x) / 7 AS f8_div7, sum(x) * 2 AS f8_times,
       sum(n) / 3 AS num_div, sum(i) / 3 AS int_div,
       sum(x::float4) / 3 AS f4_div, round(sum(n), 2) AS num_round,
       (count(*) * max(x))::numeric AS ftn,
       100 / CAST(count(*) AS REAL) AS peeled, sum(i) * 1.5::float8 AS f_operand,
       sum(i)::float8 / sum(i) AS cast_tok, sqrt(sum(x)) AS f8_sqrt
FROM agg_ty;
SET provsql.active = on;
-- sqrt, ln and exp of a float aggregate are computed in that type, not in
-- numeric: numeric computes them to a fixed scale, which lost the last digit of
-- a double (sqrt of 3 came out to fifteen decimals).  Over a numeric column
-- they are still numeric's own answer, which is the one SQL gives there.  Both
-- rows below are the same query with the rewriting off.
CREATE TABLE agg_ty_t AS
  SELECT sqrt(sum(x)) AS f8_sqrt, ln(sum(x)) AS f8_ln, exp(sum(x)) AS f8_exp,
         sqrt(sum(n)) AS num_sqrt
  FROM agg_ty;
SELECT remove_provenance('agg_ty_t');
SELECT f8_sqrt::text AS f8_sqrt, f8_ln::text AS f8_ln, f8_exp::text AS f8_exp,
       num_sqrt::text AS num_sqrt
FROM agg_ty_t;
SET provsql.active = off;
SELECT sqrt(sum(x)) AS f8_sqrt, ln(sum(x)) AS f8_ln, exp(sum(x)) AS f8_exp,
       sqrt(sum(n)) AS num_sqrt
FROM agg_ty;
SET provsql.active = on;
DROP TABLE agg_ty_t;

-- The per-world reading goes through the same gate, which is the value of its
-- child: over the seven non-empty worlds of three rows at one half, sum(x)/3
-- takes 1/3, 2/3, 1, 1, 4/3, 5/3 and 2, so E = 8/7, and a comparison over a
-- cast still reaches the closed forms -- sum(i)::float8 > 3 holds in the three
-- worlds whose sum exceeds 3, 3/8.
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM agg_ty; END $$;
SELECT round(expected(sum(x) / 3)::numeric, 6) AS e_f8_div FROM agg_ty;
CREATE TABLE agg_ty_p AS
SELECT round(probability(provenance())::numeric, 6) AS p_cast_cmp
  FROM (SELECT 1 AS k FROM agg_ty GROUP BY 1 HAVING sum(i)::float8 > 3) t;
SELECT remove_provenance('agg_ty_p');
SELECT * FROM agg_ty_p;
DROP TABLE agg_ty_p;
DROP TABLE agg_ty_r;
SELECT remove_provenance('agg_ty');
DROP TABLE agg_ty;

-- ln, exp, sqrt of an aggregate result, and its power of a constant: the gate
-- carries the operation and computes it in every world, where reading the
-- value of the database as it is would leave the result untracked.  Over the
-- worlds of the two rows of the first group (1.5 and 2.5, each present with
-- probability one half), the sum takes 1.5, 2.5 and 4, so
-- E[ln(sum)] = (ln 1.5 + ln 2.5 + ln 4) / 3 = 0.902683 and
-- E[sqrt(sum)] = 1.601961, the second group being certain of its single row of 9.
CREATE TABLE agg_fn_d(g int, v numeric);
INSERT INTO agg_fn_d VALUES (1, 1.5), (1, 2.5), (2, 9.0);
SELECT add_provenance('agg_fn_d');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM agg_fn_d; END $$;
CREATE TABLE agg_fn_r AS
  SELECT g, ln(sum(v)) AS l, sqrt(sum(v)) AS s, sum(v) ^ 2 AS p,
         exp(sum(v)) AS e
  FROM agg_fn_d GROUP BY g;
SET provsql.active = off;
-- Rounded: the scale PostgreSQL gives a numeric power, and the digits it
-- prints of a float8, are not the same in every version it supports.
SELECT g, round(l::text::numeric, 6) AS ln, round(s::text::numeric, 6) AS sqrt,
       round(p::text::numeric, 6) AS pow, round(e::text::numeric, 6) AS exp,
       round(expected(l, provsql)::numeric, 6) AS e_ln,
       round(expected(s, provsql)::numeric, 6) AS e_sqrt
FROM agg_fn_r ORDER BY g;
SET provsql.active = on;
DROP TABLE agg_fn_r; DROP TABLE agg_fn_d;

-- Arithmetic whose result is a floating-point number is tracked as well: the
-- gate computes in numeric, so the value differs from what real arithmetic
-- prints in its last digits, and in exchange the division carries provenance.
-- A cast the query writes to widen an aggregate to real is subsumed by that
-- numeric arithmetic and is peeled to expose the aggregate; left in place, the
-- resolution would rather cast both operands to random_variable, a type both
-- reach implicitly, whose operators carry no aggregate.  Over the worlds of
-- the four rows of the first group, each present with probability one half,
-- count(*) is 1, 2, 3 or 4 with probability 4/16, 6/16, 4/16 and 1/16, so
-- 100 / count(*) is 50 in 0.375 of the worlds and above 30 in all but the
-- empty one and the full one, 0.875.
CREATE TABLE agg_fl_d(g int, v int);
INSERT INTO agg_fl_d VALUES (1, 1), (1, 2), (1, 3), (1, 4), (2, NULL);
SELECT add_provenance('agg_fl_d');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM agg_fl_d; END $$;
CREATE TABLE agg_fl_r AS
  SELECT g, 100 / CAST(count(v) AS REAL) AS r FROM agg_fl_d GROUP BY g;
SET provsql.active = off;
-- No row of the second group contributes to count(v), so the division has no
-- value to read: NULL, as every arithmetic on an aggregate without a value,
-- where the plain value would divide by the zero count(v) returns there.
-- Plain SQL RAISES on that division and ProvSQL answers NULL, which is
-- deliberate and not to be "fixed": the value of the database as it is is one
-- world of many, and raising there would abort the statement and take every
-- other world's answer with it -- including the probability that the row is
-- there at all, which is well defined and is the question ProvSQL is for.  The
-- same reading as a HAVING that keeps a group failing its predicate.
-- The text of an aggregate with no value is "NULL", not a number: the second
-- group reads as no value at all, where the scale of the first is rounded away
-- (it is not the same in every PostgreSQL version).
SELECT g, round(nullif(r::text, 'NULL')::numeric, 6) AS r FROM agg_fl_r ORDER BY g;
SET provsql.active = on;
DROP TABLE agg_fl_r;
CREATE TABLE agg_fl_r AS SELECT g, probability(provenance()) AS p
  FROM agg_fl_d GROUP BY g HAVING 100 / CAST(count(*) AS REAL) = 50;
SELECT remove_provenance('agg_fl_r');
SELECT g, round(p::numeric, 6) AS p FROM agg_fl_r ORDER BY g;
DROP TABLE agg_fl_r;
CREATE TABLE agg_fl_r AS SELECT g, probability(provenance()) AS p
  FROM agg_fl_d GROUP BY g HAVING 100 / CAST(count(*) AS REAL) > 30;
SELECT remove_provenance('agg_fl_r');
SELECT g, round(p::numeric, 6) AS p FROM agg_fl_r ORDER BY g;
DROP TABLE agg_fl_r;
DROP TABLE agg_fl_d;

-- Prefix @ is PostgreSQL's absolute value.  Its procedure is numeric_abs or
-- int8abs rather than abs, so the operator over agg_token is declared rather
-- than reached through the operator's function: @(2 - max(v)) carries its gate
-- as abs(2 - max(v)) does, instead of reading the plain value.
-- Two rows at one half, so three worlds carry the group: {1} reads |2-1| = 1,
-- and {5} and {1,5} both read |2-5| = 3, so the expectation of the absolute
-- value is (1+3+3)/3 = 2.333333.  The absolute value OF the expectation is
-- |(1-3-3)/3| = 1.666667, which is what reading the plain value would give:
-- the two tell the gate apart, where data of one sign could not.
CREATE TABLE agg_at_d(v int);
INSERT INTO agg_at_d VALUES (1), (5);
SELECT add_provenance('agg_at_d');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM agg_at_d; END $$;
CREATE TABLE agg_at_r AS SELECT
  round(expected(@(2 - max(v)))::numeric, 6) AS e_abs,
  round(expected(abs(2 - max(v)))::numeric, 6) AS e_absfn,
  round(expected(2 - max(v))::numeric, 6) AS e_signed
  FROM agg_at_d;
SELECT remove_provenance('agg_at_r');
SELECT * FROM agg_at_r;
DROP TABLE agg_at_r;
-- The corpus shape: sorting on it warns that the sort reads the plain value,
-- which is the ORDER BY reading and no longer the frozen-aggregate one.
CREATE TABLE agg_at_r AS
  SELECT v, @(2 - max(v)) AS a FROM agg_at_d GROUP BY v ORDER BY @(2 - max(v)), v;
SELECT remove_provenance('agg_at_r');
SELECT v, a::numeric AS a FROM agg_at_r ORDER BY v;
DROP TABLE agg_at_r;
DROP TABLE agg_at_d;

-- CAST(sum(v) AS numeric(10,1)), and the decimal(p,s) spelling of it, is the
-- length coercion numeric(numeric,int4) over a typmod rather than a function
-- of its own, so it had no counterpart to be re-resolved onto and read the
-- plain value.  It is carried as the ROUND gate over the scale the typmod
-- encodes, which is what applying the typmod does to the value.
-- Two rows at one half, so three worlds carry the group: the sums are 10.126,
-- 3.5 and 13.626, each one third.  Rounded to one digit they are 10.1, 3.5 and
-- 13.6, of expectation 27.2/3 = 9.066667, and to none 10, 4 and 14, of
-- expectation 28/3 = 9.333333 -- neither of which is the rounding of the
-- expectation of the sum, 9.084, so the numbers say the cast is applied in
-- every world and not once at the end.
CREATE TABLE agg_dec_d(v numeric);
INSERT INTO agg_dec_d VALUES (10.126), (3.5);
SELECT add_provenance('agg_dec_d');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM agg_dec_d; END $$;
CREATE TABLE agg_dec_r AS SELECT
  round(expected(sum(v))::numeric, 6) AS e_sum,
  round(expected(CAST(sum(v) AS numeric(10,1)))::numeric, 6) AS e_cast1,
  round(expected(sum(v)::decimal(10,1))::numeric, 6) AS e_dec1,
  round(expected(round(sum(v), 1))::numeric, 6) AS e_round1,
  round(expected(CAST(sum(v) AS numeric(10)))::numeric, 6) AS e_cast0,
  round(expected(round(sum(v), 0))::numeric, 6) AS e_round0
  FROM agg_dec_d;
SELECT remove_provenance('agg_dec_r');
SELECT * FROM agg_dec_r;
DROP TABLE agg_dec_r;
-- The value in the data as it is, which the cast must still give, and which
-- carries the token: 13.626 to one digit, and to none.
CREATE TABLE agg_dec_r AS
  SELECT CAST(sum(v) AS numeric(10,1)) AS c1, CAST(sum(v) AS numeric(10)) AS c0
    FROM agg_dec_d;
SELECT remove_provenance('agg_dec_r');
SELECT c1::numeric AS c1, c0::numeric AS c0 FROM agg_dec_r;
DROP TABLE agg_dec_r;
-- The PRECISION is not carried: plain SQL raises "numeric field overflow" here,
-- 1242.2 not fitting three digits, and ProvSQL gives the value.  Which worlds
-- overflow is not what the cast says -- the world holding 3.5 alone fits -- and
-- raising because the actual data overflows would lose every other world, as
-- raising on a divisor that is zero only there would (see the division above).
INSERT INTO agg_dec_d VALUES (1228.551);
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM agg_dec_d
  WHERE probability(provenance()) IS NULL; END $$;
SET provsql.active = off;
SELECT sum(v) AS plain_sum FROM agg_dec_d;
SET provsql.active = on;
CREATE TABLE agg_dec_r AS SELECT CAST(sum(v) AS numeric(3,1)) AS c FROM agg_dec_d;
SELECT remove_provenance('agg_dec_r');
SELECT c::numeric AS c FROM agg_dec_r;
DROP TABLE agg_dec_r;
SELECT remove_provenance('agg_dec_d');
DROP TABLE agg_dec_d;
