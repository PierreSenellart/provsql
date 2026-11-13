\set ECHO none
\pset format unaligned
SET client_min_messages = warning;

-- Tree decomposition by name must not fail for having split the circuit.
-- With the cache of compiled d-DNNFs extended to methods called by name,
-- the circuit is built island by island; min-fill, a heuristic, may find
-- no decomposition of the maximum width for an island although it finds
-- one for the whole circuit.  The method then builds the circuit whole.
--
-- A ladder is the disjunction of the conjunctions of the neighbours of a
-- grid of w by len inputs.  The circuits below hang a ladder of width 6
-- and length 16, and a short one, under a shared input: each ladder is an
-- island.  On some of them (which ones depends on the order the gates are
-- met in, hence on the platform) the long ladder cannot be built alone.
-- Whatever the platform, the split must succeed wherever the whole does,
-- with the same probability.
CREATE FUNCTION tpf_ladder(tag text, w int, len int) RETURNS uuid
LANGUAGE plpgsql AS $$
DECLARE ands uuid[] := '{}'; u uuid; v uuid; x uuid;
BEGIN
  FOR i IN 1..len LOOP FOR r IN 1..w LOOP
    u := md5('isl:'||tag||':'||i||':'||r)::uuid;
    PERFORM create_gate(u, 'input'); PERFORM set_prob(u, 0.5);
  END LOOP; END LOOP;
  FOR i IN 1..len LOOP FOR r IN 1..w LOOP
    u := md5('isl:'||tag||':'||i||':'||r)::uuid;
    IF r < w THEN
      v := md5('isl:'||tag||':'||i||':'||(r+1))::uuid;
      x := md5('isl:a:'||tag||':'||i||':'||r)::uuid;
      PERFORM create_gate(x, 'times', ARRAY[u,v]); ands := ands || x;
    END IF;
    IF i < len THEN
      v := md5('isl:'||tag||':'||(i+1)||':'||r)::uuid;
      x := md5('isl:b:'||tag||':'||i||':'||r)::uuid;
      PERFORM create_gate(x, 'times', ARRAY[u,v]); ands := ands || x;
    END IF;
  END LOOP; END LOOP;
  u := md5('isl:root:'||tag)::uuid;
  PERFORM create_gate(u, 'plus', ands);
  RETURN u;
END $$;
CREATE FUNCTION tpf_circuit(tag text) RETURNS uuid LANGUAGE plpgsql AS $$
DECLARE
  a uuid := md5('w:a'||tag)::uuid; x1 uuid := md5('w:1'||tag)::uuid;
  x2 uuid := md5('w:2'||tag)::uuid; r uuid := md5('w:r'||tag)::uuid;
BEGIN
  PERFORM create_gate(a, 'input'); PERFORM set_prob(a, 0.5);
  PERFORM create_gate(x1, 'times', ARRAY[a, tpf_ladder(tag||'L', 6, 16)]);
  PERFORM create_gate(x2, 'times', ARRAY[a, tpf_ladder(tag||'M', 6, 3)]);
  PERFORM create_gate(r, 'plus', ARRAY[x1, x2]);
  RETURN r;
END $$;
-- The probability by tree decomposition, or NULL if the method fails.
CREATE FUNCTION tpf_try(token uuid, cache text) RETURNS float8
LANGUAGE plpgsql AS $$
BEGIN
  PERFORM set_config('provsql.prob_cache', cache, false);
  PERFORM prob_cache_reset();
  RETURN probability_evaluate(token, 'tree-decomposition');
EXCEPTION WHEN OTHERS THEN
  RETURN NULL;
END $$;

SET provsql.idp_fold = off;
CREATE TABLE tpf AS
  SELECT seed, tpf_try(token, 'off') AS whole, tpf_try(token, 'on') AS split
  FROM (SELECT seed, tpf_circuit('q'||seed||'_6_16') AS token
        FROM generate_series(1, 6) seed) c;
SELECT count(*) FILTER (WHERE whole IS NOT NULL) > 0 AS some_built_whole,
       count(*) FILTER (WHERE whole IS NOT NULL
                          AND (split IS NULL OR abs(split - whole) > 1e-9))
         AS split_fails_or_differs
FROM tpf;

RESET provsql.idp_fold;
RESET provsql.prob_cache;
SELECT prob_cache_reset();
DROP TABLE tpf;
DROP FUNCTION tpf_try(uuid, text);
DROP FUNCTION tpf_circuit(text);
DROP FUNCTION tpf_ladder(text, int, int);
