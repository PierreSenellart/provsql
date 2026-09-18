\set ECHO none
\pset format unaligned

-- The provsql column that * expands to comes last, as the rewriting shows
-- it: a view with a column list over SELECT * names the columns the user
-- sees, and a position in ORDER BY counts them as shown.
CREATE TABLE star_t(a int, b int);
INSERT INTO star_t VALUES (1,10),(2,20);
CREATE TABLE star_u(a int, c int);
INSERT INTO star_u VALUES (1,5),(2,4);
SELECT add_provenance('star_t');
SELECT add_provenance('star_u');

-- Stored columns of views over *, provsql last
CREATE VIEW star_v AS SELECT *, -a AS e FROM star_t;
SELECT string_agg(column_name, ',' ORDER BY ordinal_position) AS cols
FROM information_schema.columns WHERE table_name = 'star_v';
CREATE VIEW star_v2(x, y, z) AS SELECT *, -a FROM star_t;
SELECT string_agg(column_name, ',' ORDER BY ordinal_position) AS cols
FROM information_schema.columns WHERE table_name = 'star_v2';
CREATE TABLE star_r AS SELECT x, y, z FROM star_v2;
SELECT remove_provenance('star_r');
SELECT * FROM star_r ORDER BY x;
DROP TABLE star_r;

-- Positions in ORDER BY count the columns as shown (3 is e, then c)
CREATE TABLE star_r AS SELECT *, -a AS e FROM star_t ORDER BY 3;
SELECT remove_provenance('star_r');
SELECT * FROM star_r;
DROP TABLE star_r;
CREATE VIEW star_o AS SELECT *, -a AS e FROM star_t ORDER BY 3 DESC;
CREATE TABLE star_r AS SELECT a FROM star_o;
SELECT remove_provenance('star_r');
SELECT * FROM star_r;
DROP TABLE star_r;
CREATE VIEW star_j AS SELECT * FROM star_t JOIN star_u USING (a) ORDER BY 3;
CREATE TABLE star_r AS SELECT a, c FROM star_j;
SELECT remove_provenance('star_r');
SELECT * FROM star_r;
DROP TABLE star_r;

-- Whole-row values read as any row leave provsql out: the output, the json
-- functions, a conversion to text, an anonymous ROW(t.*); a null-padded row
-- of an outer join stays NULL; a row of the table's own type keeps it all.
CREATE TABLE star_r AS
  SELECT a, row_to_json(t)::text AS j, t::text AS s,
         ROW(t.*)::text AS r, (ROW(t.*)::star_t).b AS rb
  FROM star_t t;
SELECT remove_provenance('star_r');
SELECT a, j, s, r, rb FROM star_r ORDER BY a;
DROP TABLE star_r;
-- A table created from a whole row gets the table's row type, as in SQL
CREATE TABLE star_r AS SELECT t AS w FROM star_t t;
SELECT format_type(atttypid, NULL) FROM pg_attribute
WHERE attrelid = 'star_r'::regclass AND attname = 'w';
DROP TABLE star_r;
CREATE TABLE star_r AS SELECT json_agg(t ORDER BY a)::text AS j FROM star_t t;
SELECT remove_provenance('star_r');
SELECT j FROM star_r;
DROP TABLE star_r;
CREATE TABLE star_r AS
  SELECT t.a, row_to_json(u)::text AS j, sr_boolean(provenance()) AS holds
  FROM star_t t LEFT JOIN star_u u ON u.a = t.a AND u.c = 5;
SELECT remove_provenance('star_r');
SELECT a, j FROM star_r WHERE holds ORDER BY a;
DROP TABLE star_r;

-- Whole rows of a subquery and of a view over a tracked table: provsql,
-- which the rewriting adds to the subquery, is not among the fields; an
-- aggregate of the subquery is a value.
CREATE VIEW star_w AS SELECT a, b FROM star_t;
CREATE TABLE star_r AS
  SELECT row_to_json(x)::text AS jx, row_to_json(w)::text AS jw
  FROM (SELECT a, b FROM star_t) x JOIN star_w w ON w.a = x.a;
SELECT remove_provenance('star_r');
SELECT jx, jw FROM star_r ORDER BY jx;
DROP TABLE star_r;
CREATE TABLE star_r AS
  SELECT row_to_json(g)::text AS j
  FROM (SELECT a, count(*) AS n FROM star_t GROUP BY a) g;
SELECT remove_provenance('star_r');
SELECT j FROM star_r ORDER BY j;
DROP TABLE star_r;
DROP VIEW star_w;

DROP VIEW star_v, star_v2, star_o, star_j;
DROP TABLE star_t, star_u;
