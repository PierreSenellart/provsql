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
