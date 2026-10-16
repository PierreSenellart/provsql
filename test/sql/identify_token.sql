\set ECHO none
\pset format unaligned

CREATE TABLE identify_result AS
SELECT identify_token(provenance())::text FROM personnel;

SELECT remove_provenance('identify_result');
SELECT * FROM identify_result;
DROP TABLE identify_result;

/* A provenance-tracked relation with a NULL-valued data column must still
 * be identified. identify_token previously tested "result IS NOT NULL" on
 * the whole matched record, but "RECORD IS NOT NULL" is true only when
 * every field is non-null; a row with any NULL data column (here c) was
 * therefore never matched, yielding (,-1) instead of the real relation. */
CREATE TABLE identify_null (a int, b int, c int);
INSERT INTO identify_null VALUES (1,10,NULL),(2,20,NULL);
SELECT add_provenance('identify_null');

CREATE TABLE identify_null_result AS
SELECT identify_token(provenance())::text FROM identify_null;

SELECT remove_provenance('identify_null_result');
SELECT * FROM identify_null_result;
DROP TABLE identify_null_result;
DROP TABLE identify_null;

/* A tracked table in a schema that is not on the search_path is identified
 * too, and named with its schema: identify_token queried each table by its
 * bare name, which resolved to nothing (or to a namesake elsewhere). */
CREATE SCHEMA identify_elsewhere;
CREATE TABLE identify_elsewhere.hidden (a int);
INSERT INTO identify_elsewhere.hidden VALUES (1);
SELECT add_provenance('identify_elsewhere.hidden');
CREATE TABLE identify_hidden_tok AS
  SELECT provsql AS tok FROM identify_elsewhere.hidden;
SELECT remove_provenance('identify_hidden_tok');
SELECT (identify_token(tok)).table_name::text AS table_name,
       (identify_token(tok)).nb_columns
FROM identify_hidden_tok;
DROP TABLE identify_hidden_tok;
DROP SCHEMA identify_elsewhere CASCADE;
