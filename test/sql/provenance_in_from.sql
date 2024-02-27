\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

select * from provenance(), personnel;
