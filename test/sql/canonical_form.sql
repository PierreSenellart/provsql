\set ECHO none
\pset format unaligned
SET client_min_messages = warning;

-- Canonical forms and island keys of circuits built gate by gate, so that
-- each shape is exactly the one meant.  In a canonical form, V is an input
-- met for the first time and #k is the k-th node of the form, met again.

-- An input gate, and a gate over children: plus is OR, times is AND, and
-- cf_not(x), the monus of the one gate and x, is NOT x.
CREATE FUNCTION cf_in(name text, p float8 = 0.5) RETURNS uuid
LANGUAGE plpgsql AS $$
DECLARE u uuid := md5('canonical_form:' || name)::uuid;
BEGIN
  PERFORM create_gate(u, 'input');
  PERFORM set_prob(u, p);
  RETURN u;
END $$;
CREATE FUNCTION cf_gate(name text, t provenance_gate, VARIADIC children uuid[])
RETURNS uuid LANGUAGE plpgsql AS $$
DECLARE u uuid := md5('canonical_form:' || name)::uuid;
BEGIN
  PERFORM create_gate(u, t, children);
  RETURN u;
END $$;
CREATE FUNCTION cf_not(name text, child uuid) RETURNS uuid
LANGUAGE sql AS $$ SELECT cf_gate(name, 'monus', gate_one(), child) $$;

-- (a AND b) OR (a AND c), then the same shape with another input shared,
-- and with the two conjunctions exchanged: one form.
SELECT circuit_canonical_form(cf_gate('e1', 'plus',
  cf_gate('e1_1', 'times', cf_in('a'), cf_in('b')),
  cf_gate('e1_2', 'times', cf_in('a'), cf_in('c')))) AS shared_a;
SELECT circuit_canonical_form(cf_gate('e2', 'plus',
  cf_gate('e2_1', 'times', cf_in('a'), cf_in('b')),
  cf_gate('e2_2', 'times', cf_in('c'), cf_in('b')))) AS shared_b;
SELECT circuit_canonical_form(cf_gate('e3', 'plus',
  cf_gate('e3_1', 'times', cf_in('a'), cf_in('c')),
  cf_gate('e3_2', 'times', cf_in('a'), cf_in('b')))) AS exchanged;

-- Renaming the inputs does not change the form.
SELECT circuit_canonical_form(cf_gate('r1', 'plus',
  cf_gate('r1_and', 'times', cf_in('orig_x', 0.3),
          cf_not('r1_not', cf_in('orig_y', 0.7))),
  cf_in('orig_x', 0.3))) AS original;
SELECT circuit_canonical_form(cf_gate('r2', 'plus',
  cf_gate('r2_and', 'times', cf_in('renamed_x', 0.3),
          cf_not('r2_not', cf_in('renamed_y', 0.7))),
  cf_in('renamed_x', 0.3))) AS renamed;

-- A cycle of four conjunctions, whose children all look alike: the order
-- the conjunctions are given in decides the form.
SELECT circuit_canonical_form(cf_gate('tie_a', 'plus',
  cf_gate('tie_a1', 'times', cf_in('ta'), cf_in('tb')),
  cf_gate('tie_a2', 'times', cf_in('tb'), cf_in('tc')),
  cf_gate('tie_a3', 'times', cf_in('tc'), cf_in('td')),
  cf_gate('tie_a4', 'times', cf_in('td'), cf_in('ta')))) AS cycle_in_order;
SELECT circuit_canonical_form(cf_gate('tie_b', 'plus',
  cf_gate('tie_b1', 'times', cf_in('ta'), cf_in('tb')),
  cf_gate('tie_b2', 'times', cf_in('td'), cf_in('ta')),
  cf_gate('tie_b3', 'times', cf_in('tc'), cf_in('td')),
  cf_gate('tie_b4', 'times', cf_in('tb'), cf_in('tc')))) AS cycle_reordered;

-- Islands.  Two conjunctions sharing an input are no islands; two with
-- inputs of their own are one island each, and the residual sees them as
-- inputs.
SELECT circuit_island_key(cf_gate('dom1', 'plus',
  cf_gate('dom1_1', 'times', cf_in('x'), cf_in('y')),
  cf_gate('dom1_2', 'times', cf_in('y'), cf_in('z')))) AS sharing;
SELECT circuit_island_key(cf_gate('dom2', 'plus',
  cf_gate('dom2_1', 'times', cf_in('x'), cf_in('y')),
  cf_gate('dom2_2', 'times', cf_in('z'), cf_in('w')))) AS disjoint;

-- An island used twice, once under a negation, is one island.
SELECT circuit_island_key(cf_gate('sh', 'plus',
  cf_gate('sh_and', 'times', cf_in('a'), cf_in('b')),
  cf_not('sh_not', cf_gate('sh_and', 'times', cf_in('a'), cf_in('b')))))
  AS shared_island;

-- NOT NOT x beside NOT x: the two branches share x, so neither is an
-- island, though the children of the root are distinct gates.
SELECT circuit_island_key(cf_gate('co', 'plus',
  cf_not('co_v', cf_not('co_b', cf_in('x'))),
  cf_not('co_w', cf_in('x')))) AS children_only;

-- A chain of 20000 negations: the form is computed without recursing as
-- deep as the circuit, and the chain below the root is one island.
DO $$
DECLARE prev uuid := cf_in('deep_leaf');
BEGIN
  FOR i IN 1..20000 LOOP
    prev := cf_not('deep_' || i, prev);
  END LOOP;
END $$;
SELECT length(circuit_canonical_form(cf_not('deep_20000',
         cf_not('deep_19999', cf_in('deep_leaf'))))) AS deep_form_length;
SELECT count(*) AS deep_islands
FROM regexp_split_to_table(
       circuit_island_key(cf_not('deep_20000',
         cf_not('deep_19999', cf_in('deep_leaf')))), E'\n') line
WHERE line LIKE 'island %';

-- The islands against their definition, on 300 circuits drawn from a fixed
-- sequence.  A gate g other than the root is an island root when the gates
-- every path from the root reaches only through g are closed under
-- children; the islands are the outermost such gates.  The forms of the
-- islands of the key must be those of the gates the definition gives.
-- Each conjunction and disjunction has two to four distinct children, so
-- that the circuit evaluated is the one described here (a gate with one
-- child is dropped when a circuit is loaded).
CREATE FUNCTION cf_rand(INOUT state bigint, bound int, OUT value int)
LANGUAGE sql AS $$
  SELECT (state * 1103515245 + 12345) % 2147483648,
         (((state * 1103515245 + 12345) % 2147483648) / 65536 % bound)::int
$$;
CREATE FUNCTION cf_islands_by_definition(
  types text[], children int[][], root int) RETURNS int[]
LANGUAGE plpgsql AS $$
DECLARE
  n int := array_length(types, 1);
  reach boolean[]; avoid boolean[]; sub boolean[][] := '{}';
  stack int[]; x int; u int; closed boolean;
  roots int[] := '{}'; result int[] := '{}'; maximal boolean;
BEGIN
  -- gates reachable from the root
  reach := array_fill(false, ARRAY[n]); reach[root] := true; stack := ARRAY[root];
  WHILE cardinality(stack) > 0 LOOP
    x := stack[cardinality(stack)]; stack := stack[1:cardinality(stack)-1];
    FOREACH u IN ARRAY children[x:x] LOOP
      IF u > 0 AND NOT reach[u] THEN reach[u] := true; stack := stack || u; END IF;
    END LOOP;
  END LOOP;
  FOR g IN 1..n LOOP
    sub := sub || ARRAY[array_fill(false, ARRAY[n])];
    CONTINUE WHEN NOT reach[g] OR g = root OR types[g] = 'input';
    -- gates reachable from the root without entering g
    avoid := array_fill(false, ARRAY[n]); avoid[root] := true; stack := ARRAY[root];
    WHILE cardinality(stack) > 0 LOOP
      x := stack[cardinality(stack)]; stack := stack[1:cardinality(stack)-1];
      FOREACH u IN ARRAY children[x:x] LOOP
        IF u > 0 AND u <> g AND NOT avoid[u] THEN
          avoid[u] := true; stack := stack || u;
        END IF;
      END LOOP;
    END LOOP;
    closed := true;
    FOR w IN 1..n LOOP
      IF reach[w] AND NOT avoid[w] THEN
        sub[g][w] := true;
        FOREACH u IN ARRAY children[w:w] LOOP
          IF u > 0 AND avoid[u] THEN closed := false; END IF;
        END LOOP;
      END IF;
    END LOOP;
    IF closed THEN roots := roots || g; END IF;
  END LOOP;
  FOREACH x IN ARRAY roots LOOP
    maximal := true;
    FOREACH u IN ARRAY roots LOOP
      IF u <> x AND sub[u][x] THEN maximal := false; END IF;
    END LOOP;
    IF maximal THEN result := result || x; END IF;
  END LOOP;
  RETURN result;
END $$;

CREATE TABLE cf_frontier(trial int, by_definition text[], by_key text[]);
DO $$
DECLARE
  state bigint := 20261103; v int;
  n_in int; n_gates int; n int; arity int; win int; kind int;
  types text[]; children int[][]; row int[]; ids uuid[]; kids uuid[];
  expected text[]; got text[]; g int;
BEGIN
  FOR trial IN 1..300 LOOP
    SELECT * INTO state, v FROM cf_rand(state, 4); n_in := 2 + v;
    SELECT * INTO state, v FROM cf_rand(state, 25); n_gates := 1 + v;
    types := '{}'; children := '{}'; ids := '{}';
    FOR i IN 1..n_in LOOP
      types := types || 'input'::text;
      children := children || ARRAY[ARRAY[0,0,0,0]];
      ids := ids || cf_in('g' || trial || '_' || i);
    END LOOP;
    FOR i IN 1..n_gates LOOP
      n := cardinality(types);
      SELECT * INTO state, kind FROM cf_rand(state, 3);
      SELECT * INTO state, v FROM cf_rand(state, 2);
      win := CASE WHEN v = 0 THEN n ELSE least(n, 3) END;
      IF kind = 2 THEN arity := 1;
      ELSE SELECT * INTO state, v FROM cf_rand(state, 3);
        arity := least(2 + v, win);
      END IF;
      row := ARRAY[0,0,0,0]; kids := '{}';
      FOR a IN 1..arity LOOP
        LOOP
          SELECT * INTO state, v FROM cf_rand(state, win);
          EXIT WHEN NOT (n - v = ANY(row));
        END LOOP;
        row[a] := n - v; kids := kids || ids[n - v];
      END LOOP;
      types := types || (ARRAY['times','plus','not'])[kind + 1];
      children := children || ARRAY[row];
      ids := ids || CASE kind
        WHEN 2 THEN cf_not('g' || trial || '_g' || i, kids[1])
        ELSE cf_gate('g' || trial || '_g' || i,
                     (ARRAY['times','plus'])[kind + 1]::provenance_gate,
                     VARIADIC kids) END;
    END LOOP;
    n := cardinality(types);
    expected := '{}';
    FOREACH g IN ARRAY cf_islands_by_definition(types, children, n) LOOP
      expected := expected || circuit_canonical_form(ids[g]);
    END LOOP;
    SELECT coalesce(array_agg(substr(line, strpos(line, ': ') + 2)), '{}')
      INTO got
      FROM regexp_split_to_table(circuit_island_key(ids[n]), E'\n') line
      WHERE line LIKE 'island %';
    INSERT INTO cf_frontier VALUES (trial,
      ARRAY(SELECT unnest(expected) ORDER BY 1),
      ARRAY(SELECT unnest(got) ORDER BY 1));
  END LOOP;
END $$;
SELECT count(*) AS circuits,
       count(*) FILTER (WHERE by_definition <> by_key) AS disagreements,
       count(*) FILTER (WHERE cardinality(by_definition) > 0) >= 60
         AS enough_with_islands
FROM cf_frontier;

DROP TABLE cf_frontier;
DROP FUNCTION cf_islands_by_definition(text[], int[][], int);
DROP FUNCTION cf_rand(bigint, int);
DROP FUNCTION cf_not(text, uuid);
DROP FUNCTION cf_gate(text, provenance_gate, uuid[]);
DROP FUNCTION cf_in(text, float8);
