\set ECHO none
SET search_path TO public,provsql;

CREATE TABLE complex_set_op_result AS
SELECT *,formula(provenance(),'personnel_name') FROM (
  SELECT city FROM personnel
  EXCEPT
  (SELECT city FROM personnel
  UNION
  SELECT city FROM personnel)
) t;

SELECT remove_provenance('complex_set_op_result');
SELECT * FROM complex_set_op_result ORDER BY city;
DROP TABLE complex_set_op_result;
