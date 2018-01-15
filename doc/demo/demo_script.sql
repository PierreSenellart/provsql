SET search_path TO public, provsql;
select * from personnel;
select add_provenance('personnel');
select * from personnel;
select * from provenance_circuit_gate;
select * from provenance_circuit_wire;
select distinct position from personnel;
select create_provenance_mapping('personnel','name','personnel_name');
select *,formula(provenance(),'personnel_name') from (select distinct position from personnel) t;
select distinct p1.city from personnel p1, personnel p2 where p1.city=p2.city and p1.id<p2.id;
select *,formula(provenance(),'personnel_name') from (select distinct p1.city from personnel p1, personnel p2 where p1.city=p2.city and p1.id<p2.id) t;
select * from personnel;
select create_provenance_mapping('personnel_level','personnel','classification');
select *,security(provenance(),'personnel_level') from (select distinct p1.city from personnel p1, personnel p2 where p1.city=p2.city and p1.id<p2.id) t;

SELECT
  *, formula(provenance(),'personnel_name') FROM (
    (SELECT DISTINCT city FROM personnel)
  EXCEPT
    (SELECT p1.city                               
     FROM personnel p1, personnel p2
     WHERE p1.city = p2.city AND p1.id < p2.id
     GROUP BY p1.city 
     ORDER BY p1.city)
  ) t;


ALTER TABLE personnel ADD COLUMN probability DOUBLE PRECISION;
UPDATE personnel SET probability=id*1./10;
SELECT create_provenance_mapping('p', 'personnel', 'probability');

SELECT create_provenance_mapping('pr', 'r', 'prob');
SELECT pr1.x,pr2.y,probability_evaluate(provenance(),'pr','compilation','dsharp')
FROM r AS pr1, r AS pr2
WHERE pr2.x=pr1.y AND pr1.x>90 AND pr2.x>90 AND pr2.y>90 
GROUP BY pr1.x,pr2.y
ORDER BY x,y;
