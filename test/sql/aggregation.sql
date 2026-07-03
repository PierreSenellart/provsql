\set ECHO none
\pset format unaligned

--aggregation formula, read via the compiled sr_formula on the agg_token

CREATE TABLE agg_result AS
    SELECT position, count(*), regexp_replace(sr_formula(provenance(),'personnel_name'),'Susan ⊕ Dave','Dave ⊕ Susan') AS formula FROM personnel
    GROUP BY position;

SELECT remove_provenance('agg_result');

SELECT * FROM agg_result ORDER BY position;

SELECT position, regexp_replace(sr_formula(count,'personnel_name'),'Dave\*1\+Susan\*1','Susan*1+Dave*1') AS aggregation_formula FROM agg_result ORDER BY position;

CREATE TABLE agg_result2 AS
  SELECT position, sr_formula(count,'personnel_name') AS aggregation_formula FROM (
    SELECT position, count(*)
    FROM personnel
    GROUP BY position
  ) subquery;
SELECT remove_provenance('agg_result2');

SElECT * FROM agg_result2 WHERE position <> 'Analyst' ORDER BY position;

CREATE TABLE agg_result3 AS
  SELECT position, sr_formula(count(*),'personnel_name') AS aggregation_formula
  FROM personnel
  GROUP BY position;
SELECT remove_provenance('agg_result3');

SElECT * FROM agg_result3 WHERE position <> 'Analyst' ORDER BY position;

DROP TABLE agg_result;
DROP TABLE agg_result2;
DROP TABLE agg_result3;
