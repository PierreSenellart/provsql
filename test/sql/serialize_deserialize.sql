\set ECHO none
SET search_path TO provsql_test,provsql;

select dump_data();
select read_data_dump();