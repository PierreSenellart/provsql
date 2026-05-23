\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Same 3-way times circuit as tseytin_cnf.sql (personnel 5/6/7). The
-- tree-decomposition route needs no external tool, so its d-DNNF shape
-- (and hence the structural counts) is deterministic. compile_ms is
-- wall-clock, so strip it before comparing.
CREATE TABLE ddnnf_stats_q AS
SELECT ddnnf_stats(provenance(), 'tree-decomposition') AS s
FROM personnel p1, personnel p2, personnel p3
WHERE p1.id=5 AND p2.id=6 AND p3.id=7;

SELECT remove_provenance('ddnnf_stats_q');

-- Structural stats (compile_ms dropped: it is non-deterministic).
SELECT s - 'compile_ms' AS stats FROM ddnnf_stats_q;

-- compile_ms is present and a non-negative number.
SELECT (s ? 'compile_ms') AS has_compile_ms,
       (s->>'compile_ms')::float8 >= 0 AS compile_ms_nonneg
FROM ddnnf_stats_q;

DROP TABLE ddnnf_stats_q;
