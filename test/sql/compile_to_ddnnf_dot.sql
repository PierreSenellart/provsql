\set ECHO none
\if `which d4 > /dev/null 2>&1 && echo true || echo false`
\pset format unaligned

-- A 3-way times circuit; the compiled d-DNNF should contain at least
-- the three IN leaves (recognisable by their @c "p=0.<n>" label).
CREATE TABLE compile_to_ddnnf_dot_q AS
SELECT compile_to_ddnnf_dot(provenance(), 'd4') AS dot
FROM personnel p1, personnel p2, personnel p3
WHERE p1.id=5 AND p2.id=6 AND p3.id=7;

SELECT remove_provenance('compile_to_ddnnf_dot_q');

-- Structural assertions (the d-DNNF shape depends on d4's choices).
SELECT
  (dot ~ '^digraph dDNNF \{')                                      AS has_digraph_header,
  (SELECT count(*) FROM regexp_matches(dot, 'p=0\.\d', 'g'))       AS in_leaf_count,
  (dot ~ 'label="∧"' OR dot ~ 'label="∨"')                         AS has_internal,
  (dot ~ 'penwidth=2')                                             AS marks_root
FROM compile_to_ddnnf_dot_q;

DROP TABLE compile_to_ddnnf_dot_q;
\else
\echo 'SKIPPING: d4 not available'
\endif
