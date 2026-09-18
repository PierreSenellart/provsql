\set ECHO none
\pset format unaligned

-- The gate-building functions are written in C; they must build the gates the
-- former PL/pgSQL versions built, at the same addresses: a circuit stored
-- before and a query run after have to meet.  The former bodies are kept here,
-- in schema gb_ref, as the reference; each case compares the token returned
-- and, through get_gate_type / get_children, the gate it designates.
--
-- The reference runs first in each comparison, so a gate it creates exists
-- when the C function runs: what is compared is the address and the shape.

CREATE SCHEMA gb_ref;

CREATE FUNCTION gb_ref.provenance_times(VARIADIC tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  times_token uuid;
  filtered_tokens uuid[];
  canonical uuid;
BEGIN
  -- A NULL element reads as the ⊗-neutral 1: it is the token slot of an
  -- untracked source (a join against an untracked table), which is
  -- certain.  Contrast provenance_plus / provenance_monus, where NULL
  -- reads as the ⊕- / ⊖-right-neutral 0: each combinator maps NULL to
  -- its own neutral element.  Nothing may therefore hand a NULL to ⊗
  -- meaning "false"; a comparison with a NULL operand goes through
  -- provenance_cmp, which returns gate_zero for it.
  SELECT array_agg(t) FROM unnest(tokens) t WHERE t IS NOT NULL AND t <> gate_one() INTO filtered_tokens;

  -- Dispatch on the FILTERED count: a single survivor short-circuits
  -- to that token directly (no useless single-child times gate); zero
  -- survivors collapse to the identity. Using array_length(tokens, 1)
  -- here would miss the [one, cmp] → [cmp] case, leaving the cmp wrapped
  -- in a one-child times when its only sibling was gate_one().
  CASE coalesce(array_length(filtered_tokens, 1), 0)
    WHEN 0 THEN
      times_token:=gate_one();
    WHEN 1 THEN
      times_token:=filtered_tokens[1];
    ELSE
      -- Computed separately from the filtering aggregate above: an
      -- ORDER BY aggregate there would make the planner feed *both*
      -- aggregates sorted input, scrambling the stored children order.
      SELECT uuid_generate_v5(uuid_ns_provsql(),
                              concat('times-canonical', array_agg(t ORDER BY t)))
      FROM unnest(filtered_tokens) t
      INTO canonical;
      IF get_gate_type(canonical) = 'times' THEN
        -- A deliberate pre-creation at the canonical address: same
        -- children, same product.
        times_token := canonical;
      ELSE
        times_token := uuid_generate_v5(uuid_ns_provsql(),concat('times',filtered_tokens));

        PERFORM create_gate(times_token, 'times', ARRAY_AGG(t)) FROM UNNEST(filtered_tokens) AS t WHERE t IS NOT NULL;
      END IF;
  END CASE;

  RETURN times_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

CREATE FUNCTION gb_ref.provenance_plus(tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  c INTEGER;
  plus_token uuid;
  filtered_tokens uuid[];
  canonical uuid;
BEGIN
  -- A NULL element reads as the ⊕-neutral 0: it stands for a row absent
  -- from the disjunction (a null-padded antijoin row whose token array
  -- slot is NULL), not for an untracked source.  Contrast provenance_times,
  -- where NULL reads as the ⊗-neutral 1 (untracked source): each
  -- combinator maps NULL to its own neutral element.
  SELECT array_agg(t) FROM unnest(tokens) t
  WHERE t IS NOT NULL AND t <> gate_zero()
  INTO filtered_tokens;

  c:=array_length(filtered_tokens, 1);

  IF c = 0 THEN
    plus_token := gate_zero();
  ELSIF c = 1 THEN
    plus_token := filtered_tokens[1];
  ELSE
    -- Computed separately from the filtering aggregate above: an ORDER
    -- BY aggregate there would make the planner feed *both* aggregates
    -- sorted input, scrambling the stored (aggregation-order) children.
    SELECT uuid_generate_v5(uuid_ns_provsql(),
                            concat('plus-canonical', array_agg(t ORDER BY t)))
    FROM unnest(filtered_tokens) t
    INTO canonical;
    IF get_gate_type(canonical) = 'plus' THEN
      -- A deliberate pre-creation at the canonical address: same
      -- children, same sum.
      plus_token := canonical;
    ELSE
      plus_token := uuid_generate_v5(
        uuid_ns_provsql(),
        concat('plus', filtered_tokens));

      PERFORM create_gate(plus_token, 'plus', filtered_tokens);
    END IF;
  END IF;

  RETURN plus_token;
END
$$ LANGUAGE plpgsql STRICT SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

CREATE FUNCTION gb_ref.provenance_monus(token1 UUID, token2 UUID)
  RETURNS UUID AS
$$
DECLARE
  monus_token uuid;
BEGIN
  IF token1 IS NULL THEN
    RAISE EXCEPTION USING MESSAGE='provenance_monus is called with first argument NULL';
  END IF;

  IF token2 IS NULL THEN
    -- The ⊖-right-neutral 0: a NULL second argument is the no-match case
    -- of the difference operator's LEFT OUTER JOIN (nothing to subtract),
    -- so X ⊖ NULL = X ⊖ 0 = X.  Note this is NOT the NULL ≡ 1 reading of
    -- provenance_times; each combinator maps NULL to its own neutral.
    RETURN token1;
  END IF;

  IF token1 = token2 THEN
    -- X-X=0
    monus_token:=gate_zero();
  ELSIF token1 = gate_zero() THEN
    -- 0-X=0
    monus_token:=gate_zero();
  ELSIF token2 = gate_zero() THEN
    -- X-0=X
    monus_token:=token1;
  ELSE
    monus_token:=uuid_generate_v5(uuid_ns_provsql(),concat('monus',token1,token2));
    PERFORM create_gate(monus_token, 'monus', ARRAY[token1::uuid, token2::uuid]);
  END IF;

  RETURN monus_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

CREATE FUNCTION gb_ref.provenance_delta
  (token UUID)
  RETURNS UUID AS
$$
DECLARE
  delta_token uuid;
BEGIN
  -- NULL token ≡ 1 (untracked source), and δ(1) = 1.  Tested first: the
  -- equality comparisons below are not NULL-safe.
  IF token IS NULL THEN
    return gate_one();
  END IF;

  IF token = gate_zero() OR token = gate_one() THEN
    return token;
  END IF;

  delta_token:=uuid_generate_v5(uuid_ns_provsql(),concat('delta',token));

  PERFORM create_gate(delta_token,'delta',ARRAY[token::uuid]);

  RETURN delta_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

CREATE FUNCTION gb_ref.provenance_cmp(
  left_token  UUID,
  comparison_op OID,
  right_token UUID
)
RETURNS UUID AS
$$
DECLARE
  cmp_token UUID;
BEGIN
  -- A comparison with a NULL operand (a NULL random_variable cell, or an
  -- aggregate that is NULL on the instance) is unknown under SQL's 3VL in
  -- every possible world: the row is annotated zero.  The function must
  -- not be STRICT: a NULL result would read as the neutral token
  -- (provenance_times drops it), silently turning "unknown" into
  -- "certainly true".
  IF left_token IS NULL OR right_token IS NULL OR comparison_op IS NULL THEN
    RETURN gate_zero();
  END IF;
  -- deterministic v5 namespace id
  cmp_token := public.uuid_generate_v5(
    uuid_ns_provsql(),
    concat('cmp', left_token::text, comparison_op::text, right_token::text)
  );
  -- wire it up in the circuit
  PERFORM create_gate(cmp_token, 'cmp', ARRAY[left_token, right_token]);
  PERFORM set_infos(cmp_token, comparison_op::integer);
  RETURN cmp_token;
END
$$ LANGUAGE plpgsql
  SET search_path=provsql,pg_temp,public
  SECURITY DEFINER
  IMMUTABLE
  PARALLEL SAFE;

CREATE FUNCTION gb_ref.annotate(token UUID, extra TEXT) RETURNS UUID AS
$$
DECLARE
  annotated uuid;
BEGIN
  IF token IS NULL THEN
    RETURN NULL;
  END IF;
  annotated := public.uuid_generate_v5(uuid_ns_provsql(),
                                       concat('annotation', token, extra));
  PERFORM create_gate(annotated, 'annotation', ARRAY[token]);
  PERFORM set_extra(annotated, extra);
  RETURN annotated;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

CREATE FUNCTION gb_ref.inversion_free_key(root TEXT, sec TEXT, factor INT)
  RETURNS TEXT AS
$$ SELECT 'K' || factor::text || ' '
       || octet_length(root) || ':' || root
       || octet_length(sec)  || ':' || sec $$
  LANGUAGE sql IMMUTABLE PARALLEL SAFE;

CREATE TABLE gb_tok(i int, t uuid);
INSERT INTO gb_tok SELECT i, public.uuid_generate_v5(provsql.uuid_ns_provsql(), 'gb-leaf-' || i)
  FROM generate_series(1, 60) i;

CREATE FUNCTION gb_tokens(VARIADIC idx int[]) RETURNS uuid[] LANGUAGE sql AS $$
  SELECT array_agg(CASE WHEN k = 0 THEN NULL
                        WHEN k = -1 THEN provsql.gate_one()
                        WHEN k = -2 THEN provsql.gate_zero()
                        ELSE (SELECT t FROM gb_tok WHERE i = k) END ORDER BY ord)
  FROM unnest(idx) WITH ORDINALITY AS u(k, ord) $$;

CREATE FUNCTION gb_same(a uuid, b uuid) RETURNS text LANGUAGE sql AS $$
  SELECT CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT TOKEN'
              WHEN provsql.get_gate_type(a) IS DISTINCT FROM provsql.get_gate_type(b) THEN 'different type'
              ELSE 'same ' || provsql.get_gate_type(a) || ' / '
                   || coalesce(array_length(provsql.get_children(a), 1), 0) || ' children' END $$;

-- 0 stands for NULL, -1 for gate_one(), -2 for gate_zero().
CREATE TABLE gb_cases(label text, idx int[]);
INSERT INTO gb_cases VALUES
  ('two', '{1,2}'), ('two, other order', '{2,1}'), ('three', '{3,1,2}'),
  ('duplicate child', '{1,1}'), ('single', '{4}'),
  ('NULL first', '{0,1,2}'), ('NULL last', '{1,2,0}'), ('NULL only', '{0}'),
  ('NULL and one survivor', '{0,5,0}'),
  ('one gate inside', '{1,-1,2}'), ('one gate alone', '{-1}'), ('one gates and a survivor', '{-1,6,-1}'),
  ('zero gate inside', '{1,-2,2}'), ('zero gate alone', '{-2}'), ('zero gates and a survivor', '{-2,6,-2}'),
  ('zero and one', '{-2,-1}'),
  ('fifty', (SELECT array_agg(i ORDER BY i DESC) FROM generate_series(1, 50) i));

SELECT 'times' AS f, label,
       gb_same(gb_ref.provenance_times(VARIADIC gb_tokens(VARIADIC idx)),
               provsql.provenance_times(VARIADIC gb_tokens(VARIADIC idx))) AS result
  FROM gb_cases ORDER BY label;
-- One deliberate difference: a ⊕ with no survivor is gate_zero; the former
-- version returned a plus gate without children, which evaluates to the same.
CREATE FUNCTION gb_no_survivor(idx int[]) RETURNS boolean LANGUAGE sql AS $$
  SELECT NOT EXISTS (SELECT 1 FROM unnest(idx) k WHERE k > 0 OR k = -1) $$;
SELECT 'plus' AS f, label,
       gb_same(gb_ref.provenance_plus(gb_tokens(VARIADIC idx)),
               provsql.provenance_plus(gb_tokens(VARIADIC idx))) AS result
  FROM gb_cases WHERE NOT gb_no_survivor(idx) ORDER BY label;
SELECT 'plus, no survivor' AS f, label,
       provsql.provenance_plus(gb_tokens(VARIADIC idx)) = provsql.gate_zero() AS is_zero,
       provsql.get_gate_type(gb_ref.provenance_plus(gb_tokens(VARIADIC idx))) AS former_type,
       coalesce(array_length(provsql.get_children(gb_ref.provenance_plus(gb_tokens(VARIADIC idx))), 1), 0) AS former_children
  FROM gb_cases WHERE gb_no_survivor(idx) ORDER BY label;

-- Children are kept in the order given, not sorted.
SELECT 'children order' AS f,
       provsql.get_children(provsql.provenance_times(VARIADIC gb_tokens(3,1,2))) = gb_tokens(3,1,2) AS times_kept,
       provsql.get_children(provsql.provenance_plus(gb_tokens(3,1,2))) = gb_tokens(3,1,2) AS plus_kept;

-- NULL and empty arrays.  plus is strict; times reads NULL as no factor.
SELECT 'null array' AS f,
       provsql.provenance_times(VARIADIC NULL::uuid[]) = provsql.gate_one() AS times_is_one,
       gb_ref.provenance_times(VARIADIC NULL::uuid[]) = provsql.gate_one() AS ref_times_is_one,
       provsql.provenance_plus(NULL::uuid[]) IS NULL AS plus_is_null;
SELECT 'empty array' AS f,
       gb_same(gb_ref.provenance_times(VARIADIC '{}'::uuid[]), provsql.provenance_times(VARIADIC '{}'::uuid[])) AS times,
       provsql.provenance_plus('{}'::uuid[]) = provsql.gate_zero() AS plus_is_zero;

-- provenance_monus, provenance_delta, provenance_cmp.  The C function runs
-- first here, so that it is the one that creates the gate.
CREATE FUNCTION gb_tok(k int) RETURNS uuid LANGUAGE sql AS $$ SELECT (gb_tokens(k))[1] $$;
SELECT 'monus' AS f, label, gb_same(c, r) AS result,
       CASE WHEN provsql.get_gate_type(c) = 'monus' THEN provsql.get_children(c) = ARRAY[gb_tok(a), gb_tok(b)] END AS children_in_order
FROM (SELECT label, a, b, provsql.provenance_monus(gb_tok(a), gb_tok(b)) AS c,
             gb_ref.provenance_monus(gb_tok(a), gb_tok(b)) AS r
      FROM (VALUES ('two tokens', 11, 12), ('the other way', 12, 11), ('X - X', 13, 13),
                   ('0 - X', -2, 14), ('X - 0', 14, -2), ('X - NULL', 15, 0),
                   ('1 - X', -1, 16), ('X - 1', 16, -1), ('0 - NULL', -2, 0)) v(label, a, b)) t
ORDER BY label;
SELECT provsql.provenance_monus(NULL, gb_tok(11));
SELECT 'delta' AS f, label, gb_same(c, r) AS result
FROM (SELECT label, provsql.provenance_delta(gb_tok(a)) AS c, gb_ref.provenance_delta(gb_tok(a)) AS r
      FROM (VALUES ('a token', 17), ('0', -2), ('1', -1), ('NULL', 0)) v(label, a)) t
ORDER BY label;
SELECT 'cmp' AS f, label, gb_same(c, r) AS result,
       (provsql.get_infos(c)).info1 AS info1, (provsql.get_infos(c)).info2 AS info2
FROM (SELECT label, provsql.provenance_cmp(gb_tok(a), op::oid, gb_tok(b)) AS c,
             gb_ref.provenance_cmp(gb_tok(a), op::oid, gb_tok(b)) AS r
      FROM (VALUES ('>', 18, 521, 19), ('the other way', 19, 521, 18), ('another operator', 18, 97, 19),
                   ('NULL left', 0, 521, 19), ('NULL right', 18, 521, 0), ('NULL operator', 18, NULL, 19)) v(label, a, op, b)) t
ORDER BY label;

-- annotate and the order keys it carries on the inversion-free route.
SELECT 'annotate' AS f, label, gb_same(c, r) AS result, provsql.get_extra(c) AS extra,
       provsql.get_children(c) = ARRAY[gb_tok(a)] AS over_the_token
FROM (SELECT label, a, provsql.annotate(gb_tok(a), x) AS c, gb_ref.annotate(gb_tok(a), x) AS r
      FROM (VALUES ('a text', 21, 'gb note'), ('another text, same token', 21, 'gb other note'),
                   ('same text, another token', 22, 'gb note'), ('empty text', 23, ''),
                   ('NULL text', 24, NULL), ('NULL token', 0, 'gb note'),
                   ('not ASCII', 25, 'gb clé – ⊗')) v(label, a, x)) t
ORDER BY label;
SELECT 'key' AS f, provsql.inversion_free_key(root, sec, factor) AS key,
       provsql.inversion_free_key(root, sec, factor) IS NOT DISTINCT FROM gb_ref.inversion_free_key(root, sec, factor) AS same
FROM (VALUES ('12', '7', 0), ('a b:c', '', 3), ('clé', '⊗', -1), (NULL, 'x', 1), ('x', NULL, 1), ('x', 'y', NULL)) v(root, sec, factor)
ORDER BY root, sec;

-- A gate planted for a multiset of tokens of a working table is returned in
-- place of an ordinary gate, whatever the order of the children, by the
-- session that planted it and for as long as the working table exists.  The
-- address is the one the former code probed in the store.
CREATE TEMP TABLE gb_work(v int, provsql uuid);
SELECT provsql.planted_scope('gb_work');
CREATE TABLE gb_planted AS
  SELECT provsql.plant_canonical('gb_work', 'times', gb_tokens(7,8,9), (gb_tokens(7))[1], 1) AS c;
SELECT 'planted' AS f,
       c = public.uuid_generate_v5(provsql.uuid_ns_provsql(),
             concat('times-canonical', (SELECT array_agg(t ORDER BY t) FROM unnest(gb_tokens(7,8,9)) t))) AS same_address,
       provsql.get_gate_type(c) AS type, (provsql.get_infos(c)).info1 AS info1,
       provsql.provenance_times(VARIADIC gb_tokens(9,7,8)) = c AS found_in_any_order,
       provsql.provenance_times(VARIADIC gb_tokens(7,8)) <> c AS other_multiset_is_not,
       provsql.provenance_plus(gb_tokens(7,8,9)) <> c AS nor_the_sum
  FROM gb_planted;

-- A gate that merely sits in the store at a canonical address was planted by
-- no one in this session: the store is not consulted.
SELECT provsql.create_gate(
         public.uuid_generate_v5(provsql.uuid_ns_provsql(),
           concat('plus-canonical', (SELECT array_agg(t ORDER BY t) FROM unnest(gb_tokens(7,8)) t))),
         'plus', gb_tokens(7));
SELECT 'store only' AS f,
       provsql.provenance_plus(gb_tokens(7,8)) =
         public.uuid_generate_v5(provsql.uuid_ns_provsql(), concat('plus', gb_tokens(7,8))) AS ordinary_gate;

-- Once the working table is gone, the next lowering forgets what was planted
-- for it; the planted gate is still in the store, and no longer returned.
DROP TABLE gb_work;
CREATE TEMP TABLE gb_work2(v int, provsql uuid);
SELECT provsql.planted_scope('gb_work2');
SELECT 'forgotten' AS f,
       provsql.get_gate_type(c) AS still_stored,
       provsql.provenance_times(VARIADIC gb_tokens(7,8,9)) <> c AS no_longer_returned
  FROM gb_planted;
-- A working table recreated under the same name starts afresh too.
SELECT provsql.plant_canonical('gb_work2', 'plus', gb_tokens(1,2), (gb_tokens(1))[1], 1) = provsql.provenance_plus(gb_tokens(2,1)) AS planted_again;
DROP TABLE gb_work2;
CREATE TEMP TABLE gb_work2(v int, provsql uuid);
SELECT provsql.planted_scope('gb_work2');
SELECT provsql.get_gate_type(provsql.provenance_plus(gb_tokens(2,1))) AS type,
       provsql.provenance_plus(gb_tokens(2,1)) =
         public.uuid_generate_v5(provsql.uuid_ns_provsql(), concat('plus', gb_tokens(2,1))) AS ordinary_gate;
DROP TABLE gb_work2;

DROP TABLE gb_tok, gb_cases, gb_planted;
DROP FUNCTION gb_tok(int); DROP FUNCTION gb_tokens(int[]); DROP FUNCTION gb_same(uuid, uuid); DROP FUNCTION gb_no_survivor(int[]);
SET client_min_messages = warning;
DROP SCHEMA gb_ref CASCADE;
