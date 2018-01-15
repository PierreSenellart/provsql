\set ECHO none
set search_path to public, provsql;

CREATE TABLE deterministic_result AS
  SELECT provenance()=provenance()
  FROM personnel
  GROUP BY city;

SELECT remove_provenance('deterministic_result');
SELECT * FROM deterministic_result;
DROP TABLE deterministic_result;  
