\set ECHO none
\pset format unaligned

-- KC-surface compilation of an inversion-free-certified query.  The three
-- knowledge-compilation surfaces -- compile_to_ddnnf (NNF text),
-- compile_to_ddnnf_dot (DOT), ddnnf_stats (measures) -- with the
-- 'inversion-free' compiler build the structured d-DNNF over the per-input
-- order markers (buildInversionFreeDDNNF), the same artefact the
-- 'inversion-free' probability method evaluates.  Until now only that method
-- was exercised; these surfaces are the other consumers of the certificate.
--
-- Witness: the canonical inversion-free self-join S(x,y),A(x,y),S(x,z),B(x,z)
-- (the lineage must come from the real planner, so the certificate is attached
-- on the planner side).  Only x = 1 qualifies, a single derivation of four
-- independent inputs, so the structured d-DNNF is one AND over four leaves:
-- 5 nodes, 4 edges, 4 input variables, smooth and inversion-free (depth 1).

SET provsql.provenance = 'boolean';

CREATE TABLE iff_s(x int, c2 int);
INSERT INTO iff_s VALUES (1,10),(1,20),(2,10);
SELECT add_provenance('iff_s');
CREATE TABLE iff_a(x int, c2 int);
INSERT INTO iff_a VALUES (1,10),(2,99);
SELECT add_provenance('iff_a');
CREATE TABLE iff_b(x int, c2 int);
INSERT INTO iff_b VALUES (1,20),(2,10);
SELECT add_provenance('iff_b');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM iff_s;
  PERFORM set_prob(provsql, 0.5) FROM iff_a;
  PERFORM set_prob(provsql, 0.5) FROM iff_b;
END $$;

CREATE TEMP TABLE iff_w AS
  SELECT s1.x AS x, provenance() AS p
    FROM iff_s s1, iff_a a, iff_s s2, iff_b b
   WHERE s1.x = a.x AND s1.c2 = a.c2     -- S(x,y), A(x,y)
     AND s1.x = s2.x                     -- both S occurrences share the root x
     AND s2.x = b.x AND s2.c2 = b.c2;    -- S(x,z), B(x,z)
SELECT remove_provenance('iff_w');

-- compile_to_ddnnf: the NNF header is the structural size "nnf <nodes> <edges>
-- <vars>" (the leaf variable numbers below it are gate ids, not asserted).
SELECT split_part(compile_to_ddnnf(p, 'inversion-free'), E'\n', 1) AS nnf_header
  FROM iff_w;

-- compile_to_ddnnf_dot: a DOT digraph for viewing.
SELECT left(compile_to_ddnnf_dot(p, 'inversion-free'), 7) AS dot_prefix
  FROM iff_w;

-- ddnnf_stats: structural measures of the same d-DNNF (drop compile_ms, which
-- is a wall-clock timing).
SELECT (ddnnf_stats(p, 'inversion-free')::jsonb - 'compile_ms') AS stats
  FROM iff_w;

DROP TABLE iff_s; DROP TABLE iff_a; DROP TABLE iff_b;
