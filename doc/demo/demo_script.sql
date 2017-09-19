select * from personal;
select add_provenance('personal');
select * from personal;
select * from provenance_circuit_gate;
select * from provenance_circuit_wire;
select distinct position from personal;
select *,formula(provenance(),'personal_name') from (select distinct position from personal) t;
select distinct p1.city from personal p1, personal p2 where p1.city=p2.city and p1.id<p2.id;
select *,formula(provenance(),'personal_name') from (select distinct p1.city from personal p1, personal p2 where p1.city=p2.city and p1.id<p2.id) t;
select * from personal;
select create_provenance_mapping('personal_level','personal','classification');
select *,security(provenance(),'personal_level') from (select distinct p1.city from personal p1, personal p2 where p1.city=p2.city and p1.id<p2.id) t;

SELECT
  *, formula(provenance(),'personal_name') FROM (
    (SELECT DISTINCT city FROM personal)
  EXCEPT
    (SELECT p1.city                               
     FROM personal p1, personal p2
     WHERE p1.city = p2.city AND p1.id < p2.id
     GROUP BY p1.city 
     ORDER BY p1.city)
  ) t;


ALTER TABLE personal ADD COLUMN probability DOUBLE PRECISION;
UPDATE personal SET probability=id*1./10;
SELECT create_provenance_mapping('p', 'personal', 'probability');

SELECT pr1.x,pr2.y,probability_evaluate(provenance(),'pr','compilation','dsharp')
FROM r AS pr1, r AS pr2
WHERE pr2.x=pr1.y AND pr1.x>90 AND pr2.x>90 AND pr2.y>90 
GROUP BY pr1.x,pr2.y
ORDER BY x,y;
