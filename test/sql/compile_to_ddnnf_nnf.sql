\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- 3-way times circuit (personnel 5/6/7), same as tseytin_cnf.sql. The
-- tree-decomposition route needs no external tool, so the NNF is
-- deterministic. Its literals use the same variable numbering as
-- tseytin_cnf_mapping (shown below for comparison).
CREATE TABLE ddnnf_nnf_q AS
SELECT compile_to_ddnnf(provenance(), 'tree-decomposition') AS nnf,
       provenance() AS tok
FROM personnel p1, personnel p2, personnel p3
WHERE p1.id=5 AND p2.id=6 AND p3.id=7;

SELECT remove_provenance('ddnnf_nnf_q');

-- The compiled d-DNNF in c2d/d4 NNF text format.
SELECT nnf FROM ddnnf_nnf_q;

-- The NNF literals use exactly these variables (tseytin_cnf numbering).
SET provsql.active = off;
SELECT array_agg(variable ORDER BY variable) AS cnf_variables
FROM ddnnf_nnf_q, tseytin_cnf_mapping(tok);
SET provsql.active = on;

DROP TABLE ddnnf_nnf_q;
