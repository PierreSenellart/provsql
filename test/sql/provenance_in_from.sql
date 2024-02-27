\set ECHO none
\pset format csv
SET search_path TO provsql_test,provsql;

select * from provenance(), personnel;
