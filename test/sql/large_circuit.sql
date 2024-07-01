\set ECHO none
\pset format unaligned

CREATE table t(x INT);
INSERT INTO t VALUES(generate_series(0,9999));
SELECT provsql.add_provenance('t');
DO $$ BEGIN
    PERFORM provsql.set_prob(provsql.provenance(),0.5) from t;
END $$;
CREATE TABLE tresult AS
SELECT provsql.probability_evaluate(provsql.provenance()) from (select distinct 1 from t) temp;
SELECT provsql.remove_provenance('tresult');
SELECT * FROM tresult;
DROP table t;
DROP table tresult;
