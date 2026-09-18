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

DROP VIEW star_v, star_v2, star_o, star_j;
DROP TABLE star_t, star_u;
