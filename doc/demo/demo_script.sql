SET search_path TO public, provsql;
SELECT * FROM personnel;
SELECT add_provenance('personnel');
SELECT * FROM personnel;

SELECT DISTINCT position FROM personnel;
SELECT create_provenance_mapping('name','personnel','name');
SELECT *,formula(provenance(),'name') FROM (
  SELECT distinct position FROM personnel
) t;

SELECT DISTINCT P1.city
FROM Personnel P1 JOIN Personnel P2
ON P1.city = P2.city
WHERE P1.id < P2.id;
SELECT view_circuit(/* Replace this with provenance token of Paris */,'name');
SELECT create_provenance_mapping('personnel_level','personnel','classification');
SELECT *,security(provenance(),'personnel_level')
FROM (
  SELECT DISTINCT P1.city
  FROM Personnel P1 JOIN Personnel P2
  ON P1.city = P2.city
  WHERE P1.id < P2.id
) t;
SELECT * FROM personnel;

  SELECT DISTINCT city FROM Personnel
EXCEPT
  SELECT DISTINCT P1.city
  FROM Personnel P1 JOIN Personnel P2
  ON P1.city = P2.city
  WHERE P1.city = P2.city AND P1.id < P2.id;
SELECT *, formula(provenance(),'name') FROM (
    SELECT DISTINCT city FROM Personnel
  EXCEPT
    SELECT DISTINCT P1.city
    FROM Personnel P1 JOIN Personnel P2
    ON P1.city = P2.city
    WHERE P1.city = P2.city AND P1.id < P2.id
) t;
SELECT DISTINCT P1.city
FROM Personnel P1 JOIN Personnel P2
ON P1.city = P2.city
WHERE P1.id < P2.id;
SELECT *, where_provenance(provenance()) FROM (
  SELECT DISTINCT P1.city
  FROM Personnel P1 JOIN Personnel P2
  ON P1.city = P2.city
  WHERE P1.id < P2.id
) t;

ALTER TABLE personnel ADD COLUMN probability DOUBLE PRECISION;
UPDATE personnel SET probability=id*1./10;
SELECT * FROM personnel;
SELECT create_provenance_mapping('p', 'personnel', 'probability');

SELECT *, probability_evaluate(provenance(),'p','possible-worlds') FROM (
    SELECT DISTINCT city FROM Personnel
  EXCEPT
    SELECT DISTINCT P1.city
    FROM Personnel P1 JOIN Personnel P2
    ON P1.city = P2.city
    WHERE P1.city = P2.city AND P1.id < P2.id
) t;
SELECT *, probability_evaluate(provenance(),'p','monte-carlo','1000') FROM (
    SELECT DISTINCT city FROM Personnel
  EXCEPT
  SELECT DISTINCT P1.city
    FROM Personnel P1 JOIN Personnel P2
    ON P1.city = P2.city
    WHERE P1.city = P2.city AND P1.id < P2.id
) t;
SELECT *, probability_evaluate(provenance(),'p','compilation','c2d') FROM (
    SELECT DISTINCT city FROM Personnel
  EXCEPT 
    SELECT DISTINCT P1.city
    FROM Personnel P1 JOIN Personnel P2
    ON P1.city = P2.city
    WHERE P1.city = P2.city AND P1.id < P2.id
) t;

SELECT * FROM r;
SELECT add_provenance('r');
SELECT create_provenance_mapping('pr', 'r', 'prob');
\timing
SELECT pr1.x,pr2.y,probability_evaluate(provenance(),'pr','possible-worlds')
FROM r AS pr1, r AS pr2
WHERE pr2.x=pr1.y AND pr1.x>90 AND pr2.x>90 AND pr2.y>90 
GROUP BY pr1.x,pr2.y
ORDER BY x,y;
/* CTRL+C */
/* Roughly 1% additive error with 95% probability */
SELECT pr1.x,pr2.y,probability_evaluate(provenance(),'pr','monte-carlo','9604')
FROM r AS pr1, r AS pr2
WHERE pr2.x=pr1.y AND pr1.x>90 AND pr2.x>90 AND pr2.y>90 
GROUP BY pr1.x,pr2.y
ORDER BY x,y;
SELECT pr1.x,pr2.y,probability_evaluate(provenance(),'pr','compilation','dsharp')
FROM r AS pr1, r AS pr2
WHERE pr2.x=pr1.y AND pr1.x>90 AND pr2.x>90 AND pr2.y>90 
GROUP BY pr1.x,pr2.y
ORDER BY x,y;
