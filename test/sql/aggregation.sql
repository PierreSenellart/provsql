\set ECHO none
SET search_path TO public,provsql;

--aggregation formula

CREATE FUNCTION public.formula_semimod(formula1 text, formula2 text) RETURNS text
    LANGUAGE sql IMMUTABLE STRICT
    AS $$
  SELECT concat('(',formula1,' * ',formula2,')')
$$;

CREATE FUNCTION public.formula_agg_state(state public.formula_state, value text) RETURNS public.formula_state
    LANGUAGE plpgsql IMMUTABLE
    AS $$
BEGIN
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,' , ',value),state.nbargs+1);
  END IF;
END
$$;

CREATE AGGREGATE public.formula_agg(text) (
    SFUNC = public.formula_agg_state,
    STYPE = public.formula_state,
    INITCOND = '(1,0)'
);

CREATE FUNCTION public.formula_agg_final(state public.formula_state, fname varchar) RETURNS text
  LANGUAGE sql IMMUTABLE STRICT
  AS
  $$
    SELECT concat(fname,'{ ',state.formula,' }');
  $$;

CREATE FUNCTION public.aggregation_formula(token anyelement, token2value regclass) RETURNS text
    LANGUAGE plpgsql
    AS $$
BEGIN
  RETURN provsql.aggregation_evaluate(
    token::provsql.provenance_token,
    token2value,
    'formula_agg_final',
    'formula_agg',
    'formula_semimod',
    '𝟙'::text,
    'formula_plus',
    'formula_times',
    'formula_monus',
    'formula_delta');
END
$$;

CREATE TABLE agg_result AS
    SELECT position, count(*), formula(provenance(),'personnel_name') FROM personnel
    GROUP BY position;

SELECT remove_provenance('agg_result');

SELECT * FROM agg_result ORDER BY position;

SELECT position, regexp_replace(aggregation_formula(count,'personnel_name'),'\(1 \* Dave\) , \(1 \* Susan\)','(1 * Susan) , (1 * Dave)') AS aggregation_formula FROM agg_result ORDER BY position;

CREATE TABLE agg_result2 AS 
  SELECT position, aggregation_formula(count,'personnel_name') FROM (
    SELECT position, count(*)
    FROM personnel
    GROUP BY position
  ) subquery;
SELECT remove_provenance('agg_result2'); 

SElECT * FROM agg_result2 WHERE position <> 'Analyst' ORDER BY position;  

CREATE TABLE agg_result3 AS 
  SELECT position, aggregation_formula(count(*),'personnel_name')
  FROM personnel
  GROUP BY position;
SELECT remove_provenance('agg_result3');

SElECT * FROM agg_result3 WHERE position <> 'Analyst' ORDER BY position;  

DROP TABLE agg_result;
DROP TABLE agg_result2;
DROP TABLE agg_result3;
