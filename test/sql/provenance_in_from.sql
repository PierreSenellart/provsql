\set ECHO none
SET search_path TO provsql_test,provsql;

select * from provenance(), personnel;
