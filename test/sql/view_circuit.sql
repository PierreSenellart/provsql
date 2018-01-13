SET search_path TO public, provsql;

CREATE TABLE vc_result AS
SELECT city, view_circuit(provenance(),'d',1) AS prob
FROM (
  SELECT DISTINCT city
  FROM personal
EXCEPT 
  SELECT p1.city
  FROM personal p1,personal p2
  WHERE p1.id<p2.id AND p1.city=p2.city
  GROUP BY p1.city
) t;

select * from vc_result;
