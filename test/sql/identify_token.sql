\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

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
