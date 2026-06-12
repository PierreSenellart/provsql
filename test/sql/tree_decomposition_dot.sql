\set ECHO none
\pset format unaligned

-- A 3-way times circuit (AND of three personnel inputs).
CREATE TABLE tree_decomposition_dot_q AS
SELECT tree_decomposition_dot(provenance()) AS td
FROM personnel p1, personnel p2, personnel p3
WHERE p1.id=5 AND p2.id=6 AND p3.id=7;

SELECT remove_provenance('tree_decomposition_dot_q');

-- First line is the "// treewidth=N" comment we add on top of
-- TreeDecomposition::toDot().
SELECT split_part(td, E'\n', 1) AS treewidth_comment FROM tree_decomposition_dot_q;

-- Structural checks on the DOT body (the gate-id-to-bag mapping is
-- implementation-dependent, so we only assert shape).
SELECT
  (td ~ '\ndigraph circuit\{')                                       AS has_digraph_header,
  (td ~ '\}\s*$')                                                    AS has_closing_brace,
  (SELECT count(*) FROM regexp_matches(td, 'label="\{[^"]+\}"', 'g')) AS bag_count
FROM tree_decomposition_dot_q;

DROP TABLE tree_decomposition_dot_q;
