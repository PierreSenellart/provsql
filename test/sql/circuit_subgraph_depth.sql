\set ECHO none
-- Pin the longest-path semantics of circuit_subgraph's depth column
-- (the change introduced when the d-DNNF inspector started reporting
-- the canonical circuit-depth notion). Build a circuit where a leaf
-- is reached from the root via two paths of different lengths:
--
--   root = times(inner, leaf)        -- direct edge: depth 1
--   inner = times(leaf)              -- inner→leaf: depth 2
--
-- Shortest path from root to leaf has length 1; longest has length 2.
-- Longest-path semantics report depth=2 for leaf.
\pset format unaligned

CREATE TABLE cs_t(id int);
INSERT INTO cs_t VALUES (1);
SELECT add_provenance('cs_t');

DO $$
DECLARE leaf uuid; inner_g uuid; root uuid;
BEGIN
  SELECT provsql INTO leaf FROM cs_t WHERE id = 1;
  inner_g := public.uuid_generate_v5(uuid_ns_provsql(),
                              concat('cs-depth-inner', leaf));
  root    := public.uuid_generate_v5(uuid_ns_provsql(),
                              concat('cs-depth-root', leaf));
  PERFORM create_gate(inner_g, 'times', ARRAY[leaf]);
  PERFORM create_gate(root,    'times', ARRAY[inner_g, leaf]);
  PERFORM set_config('cs.leaf', leaf::text,    false);
  PERFORM set_config('cs.root', root::text,    false);
END $$;

-- Depth of the leaf reached from the root: 2 (longest path) under the
-- canonical circuit-depth notion. (Shortest path would report 1.)
SELECT depth AS leaf_depth
FROM circuit_subgraph(current_setting('cs.root')::uuid)
WHERE node = current_setting('cs.leaf')::uuid
LIMIT 1;

DROP TABLE cs_t;
