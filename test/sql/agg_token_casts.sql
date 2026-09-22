\set ECHO none
\pset format unaligned

-- agg_token value extraction: the provenance-losing casts to each numeric
-- type (with their WARNING), the internal value accessor, agg_token
-- literals, and the degenerate-value convention: an empty or literal-NULL
-- value string casts to SQL NULL rather than raising a type-input error.

CREATE TABLE atc(g int, v int);
INSERT INTO atc VALUES (1,10),(1,32);
SELECT add_provenance('atc');

CREATE TABLE atr AS SELECT g, sum(v) AS s, count(*) AS c FROM atc GROUP BY g;
SELECT remove_provenance('atr');

SELECT s::numeric AS s_num, s::double precision AS s_f8, s::integer AS s_i4,
       s::bigint AS s_i8, s::text AS s_txt, c::integer AS c_i4
FROM atr;
SELECT provsql.agg_token_value(s) AS s_value FROM atr;
DROP TABLE atr;

-- An aggregate result read from a subquery (here over VALUES): cast to
-- its value there, like anywhere it is read as a value.
CREATE TABLE atr AS
  SELECT (SELECT max(x) FROM (VALUES (100 - c), (0)) v(x)) AS m,
         (SELECT max(x) FROM (VALUES (s)) v(x)) AS m1,
         (SELECT c::numeric + y FROM (VALUES (1)) v(y)) AS m2
  FROM (SELECT count(*) AS c, sum(v) AS s FROM atc) t;
SELECT remove_provenance('atr');
SELECT * FROM atr;
DROP TABLE atr;

-- An "any" parameter, declared or VARIADIC, reads the value too.
CREATE TABLE atr AS
  SELECT json_build_object('n', count(*), 'vs', array_agg(v ORDER BY v)) AS j,
         concat(count(*), '!') AS c, format('%s', sum(v)) AS f
  FROM atc;
SELECT remove_provenance('atr');
SELECT * FROM atr;
DROP TABLE atr;
CREATE TABLE atr AS
  SELECT json_build_object('n', c) AS j, concat(c, '!') AS cc
  FROM (SELECT count(*) AS c FROM atc) t;
SELECT remove_provenance('atr');
SELECT * FROM atr;
DROP TABLE atr;
-- The agg_token counterparts of round, abs, the casts and the rest carry names
-- of their own -- provsql_abs, provsql_round -- and not the names of the
-- pg_catalog functions they stand for.  Beside those functions they made an
-- untyped literal ambiguous for anyone with provsql in their search_path: an
-- unknown argument is resolved by type category, pg_catalog's candidates are
-- numeric and an agg_token one is not, and PostgreSQL then refuses to choose
-- ("function abs(unknown) is not unique").  The rewriting looks the
-- counterparts up under their own names, so this resolves and the carrying
-- below still happens.
SELECT abs('0.20') AS untyped_abs;
SELECT round('0.25') AS untyped_round;
CREATE TABLE atc_carry AS
  SELECT abs(sum(v)) AS a, round(avg(v)) AS r, floor(sum(v)) AS f,
         sqrt(sum(v)) AS q
  FROM atc;
SELECT remove_provenance('atc_carry');
SELECT a::text AS a, r::text AS r, f::text AS f, q::text AS q FROM atc_carry;
DROP TABLE atc_carry;

DROP TABLE atc;

-- Literals: '( <uuid> , <value> )'.  The casts read only the value part,
-- so a placeholder UUID is fine.
SELECT '( 00000000-0000-0000-0000-000000000000 , 42 )'::provsql.agg_token::numeric AS lit_num;
SELECT '( 00000000-0000-0000-0000-000000000000 , 42 )'::provsql.agg_token::integer AS lit_i4;

-- Degenerate value strings: empty and literal NULL.
SELECT ('( 00000000-0000-0000-0000-000000000000 ,  )'::provsql.agg_token)::numeric IS NULL AS empty_num_null,
       ('( 00000000-0000-0000-0000-000000000000 ,  )'::provsql.agg_token)::double precision IS NULL AS empty_f8_null,
       ('( 00000000-0000-0000-0000-000000000000 ,  )'::provsql.agg_token)::integer IS NULL AS empty_i4_null,
       ('( 00000000-0000-0000-0000-000000000000 ,  )'::provsql.agg_token)::bigint IS NULL AS empty_i8_null,
       provsql.agg_token_value('( 00000000-0000-0000-0000-000000000000 ,  )'::provsql.agg_token) IS NULL AS empty_value_null;
SELECT ('( 00000000-0000-0000-0000-000000000000 , NULL )'::provsql.agg_token)::numeric IS NULL AS nullword_num_null;

-- Malformed literal: a clean type-input error.
SELECT 'garbage'::provsql.agg_token;

-- The warning a conversion gives is one for the statement and for each target
-- type, not one per row: it says nothing the first does not, and a query over
-- a large relation used to write a line for every row (a round of the
-- differential testing once filled a server log with them).  Three rows here,
-- two target types, so two warnings.
CREATE TABLE atc_many(g int, v int);
INSERT INTO atc_many SELECT i % 3, i FROM generate_series(1, 9) i;
SELECT add_provenance('atc_many');
-- Three groups, so three conversions of the sum to numeric: ONE warning, where
-- there was one per row.  The cast to bigint is the sum's own type, so it
-- converts nothing and the column stays tracked.
CREATE TABLE atc_conv AS
  SELECT total::numeric AS a, total::bigint AS b
    FROM (SELECT g, sum(v) AS total FROM atc_many GROUP BY g) s;
SELECT remove_provenance('atc_conv');
SELECT a, b::text AS b FROM atc_conv ORDER BY a;
DROP TABLE atc_conv;
SELECT remove_provenance('atc_many');
DROP TABLE atc_many;

-- A cast of an aggregate result to a number is the function of its value that
-- it is, and is carried rather than frozen: the rewriting swaps the
-- pg_catalog cast over an agg_token for the provsql counterpart of the same
-- name, as it does for round() and floor().  A widening keeps the value, a
-- narrowing to an integer rounds as PostgreSQL's own cast rounds, and a cast
-- to text (or to a boolean, or a date) has no arithmetic behind it and stays a
-- reading of the plain value, which the freezing names.
CREATE TABLE atc_cast(v int);
INSERT INTO atc_cast VALUES (1), (2), (4);
SELECT add_provenance('atc_cast');
CREATE TABLE atc_cast_r AS
  SELECT sum(v)::numeric AS to_numeric, avg(v)::bigint AS to_bigint,
         sum(v)::float8 AS to_float
  FROM atc_cast;
SELECT remove_provenance('atc_cast_r');
SELECT to_numeric::text AS to_numeric, to_bigint::text AS to_bigint,
       to_float::text AS to_float
FROM atc_cast_r;
DROP TABLE atc_cast_r;
-- Cast the same three ways with the rewriting off, which the values must equal
-- (avg is 2.33, and the cast to bigint rounds it to 2).
SET provsql.active = off;
SELECT sum(v)::numeric AS to_numeric, avg(v)::bigint AS to_bigint,
       sum(v)::float8 AS to_float
FROM atc_cast;
SET provsql.active = on;
-- A cast to text is a reading of the plain value, so it is frozen and named.
SELECT sum(v)::text AS t FROM atc_cast;
-- A cast FROM something that is not a number cannot be carried the same way:
-- the counterparts compute in numeric over the value the aggregate carries,
-- and the value of a bool_or is the text "true", which int4(boolean) -- what
-- "bool_or(x)::int" writes -- would read as a numeric.  That one cast is the
-- indicator it means instead, CASE WHEN agg = true THEN 1 WHEN agg = false
-- THEN 0 ELSE NULL END, so it is carried: 1/1 for the member of both groups
-- and 1/0 for the one in A alone, as plain SQL answers, and tracked.
-- Checked per world over two rows at one half: for the first member,
-- bool_or(gn='A') is true, false and true over the three worlds where the
-- group exists, so the indicator is 1, 0, 1 and E = 2/3, and the same for
-- group B the other way round; the second member holds one row of A, so 1
-- and 0 with certainty.  (Boolean to integer is the only cast PostgreSQL has
-- on a Boolean: it rejects Boolean to numeric or to bigint itself.)
CREATE TABLE atc_bool(m int, gn text);
INSERT INTO atc_bool VALUES (1,'A'),(1,'B'),(2,'A');
SELECT add_provenance('atc_bool');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM atc_bool; END $$;
CREATE TABLE atc_bool_r AS
  SELECT m, bool_or(gn = 'A')::int AS a, bool_or(gn = 'B')::int AS b
  FROM atc_bool GROUP BY m;
SELECT remove_provenance('atc_bool_r');
SELECT m, a::text AS a, round(expected(a)::numeric, 6) AS e_a,
       b::text AS b, round(expected(b)::numeric, 6) AS e_b
FROM atc_bool_r ORDER BY m;
DROP TABLE atc_bool_r;
-- A text-valued aggregate cast to a number, and one read through a text
-- operator, have no such indicator behind them and stay a reading of the
-- plain value.
SELECT min(gn)::int AS n FROM atc_bool WHERE gn ~ '^[0-9]+$';
SELECT max(gn) || '!' AS shout FROM atc_bool;
SELECT remove_provenance('atc_bool');
DROP TABLE atc_bool;

-- An agg_token column has an ordering of its own, on the value each token
-- carries: numbers as numbers (50 before 30, not the other way as the text of
-- the values would have it), a token without a value first, and the statement
-- told once that this is the order of the data as it is.
CREATE TABLE atc_ord(g int, v int);
INSERT INTO atc_ord VALUES (1, 30), (2, 5), (3, 50);
SELECT add_provenance('atc_ord');
CREATE TABLE atc_ord_r AS
  SELECT g, sum(v)::numeric AS s FROM atc_ord GROUP BY g;
SELECT remove_provenance('atc_ord_r');
-- Aliased apart from the column: "s::text AS s" would order by the text.
SELECT g, s::text AS shown FROM atc_ord_r ORDER BY s DESC;
DROP TABLE atc_ord_r;
SELECT remove_provenance('atc_ord');
DROP TABLE atc_ord;
SELECT remove_provenance('atc_cast');
DROP TABLE atc_cast;
