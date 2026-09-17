\set ECHO none
\pset format unaligned

-- FILTER on aggregates, and NULL inputs of aggregates that see them.
--
-- The circuit of an aggregate must range over exactly the inputs the
-- aggregate reads:
--  * a NULL-skipping aggregate (sum, min, avg, string_agg, ...) with
--    FILTER (WHERE f) is agg(CASE WHEN f THEN x END): a row failing f is no
--    child of the gate;
--  * count keeps such a row as a child of value 0, since the row still
--    witnesses the group (count ... = 0 must not hold in the empty world of a
--    grouped query);
--  * a NULL-keeping aggregate (array_agg, json_agg, ...) keeps its NULL
--    inputs as children, and its FILTER removes children.
--
-- Rows are independent, with distinct probabilities so that every expected
-- value below pins down which rows the gate ranges over:
--   g=1: a (v=5,  w='p',  0.5), b (v=6, w=NULL, 0.4), c (v=7, w='q', 0.2)
--   g=2: e (v=5,  w='r',  0.5)
--   g=3: f (v=NULL, w=NULL, 0.5)
--   g=4: h (v=1,  w='NULL' the string, 0.5), i (v=2, w=NULL, 0.5)
--   g=5: j (v=1,  w='-null', 0.5): a string that must not be read as NULL
--        whatever the seed of the NULL value gate

CREATE TABLE af(id text, g int, v int, w text, p float);
INSERT INTO af VALUES
  ('a',1,5,'p',0.5), ('b',1,6,NULL,0.4), ('c',1,7,'q',0.2),
  ('e',2,5,'r',0.5), ('f',3,NULL,NULL,0.5),
  ('h',4,1,'NULL',0.5), ('i',4,2,NULL,0.5), ('j',5,1,'-null',0.5);
SELECT add_provenance('af');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM af; END $$;

CREATE FUNCTION af_p(u uuid) RETURNS numeric LANGUAGE sql AS
  $$ SELECT round(provsql.probability_evaluate(u)::numeric, 4) $$;

-- ---------------------------------------------------------------------------
-- 1. count with FILTER
-- ---------------------------------------------------------------------------

-- >= 1: b or c present.              1: 1-0.6*0.8 = 0.52   2,3,4: 0
CREATE TABLE af_t1 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING count(*) FILTER (WHERE v>5) >= 1;
SELECT remove_provenance('af_t1'); SELECT 'count(*) f >= 1' AS q, g, p FROM af_t1 ORDER BY g;

-- = 0: the group exists (through a row failing the filter) and no row passes
-- it.  1: a, not b, not c = 0.5*0.6*0.8 = 0.24   2,3: 0.5   4: 0.75
CREATE TABLE af_t2 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING count(*) FILTER (WHERE v>5) = 0;
SELECT remove_provenance('af_t2'); SELECT 'count(*) f = 0' AS q, g, p FROM af_t2 ORDER BY g;

-- count(expr) with FILTER counts rows passing the filter with a non-NULL
-- expr.  count(w) FILTER (v<7) = 1 on g=1: only a qualifies (b has w NULL, c
-- fails the filter), the group may exist through any row: 0.5.
-- 2: 0.5   3: 0 (f never counts)   4: h present = 0.5
CREATE TABLE af_t3 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING count(w) FILTER (WHERE v<7) = 1;
SELECT remove_provenance('af_t3'); SELECT 'count(w) f = 1' AS q, g, p FROM af_t3 ORDER BY g;

-- The same predicate on the aggregate column of a subquery.
CREATE TABLE af_t4 AS SELECT g, af_p(provenance()) AS p FROM
  (SELECT g, count(*) FILTER (WHERE v>5) AS c FROM af GROUP BY g) s WHERE c >= 1;
SELECT remove_provenance('af_t4'); SELECT 'subquery c >= 1' AS q, g, p FROM af_t4 ORDER BY g;

-- Scalar aggregation: the result row exists in every world, the empty one
-- included.  = 0 with an unsatisfiable filter: 1.   < 2: not (b and c) = 0.92
CREATE TABLE af_t5 AS SELECT af_p(provenance()) AS p FROM af
  HAVING count(*) FILTER (WHERE v>100) = 0;
SELECT remove_provenance('af_t5'); SELECT 'scalar f = 0' AS q, p FROM af_t5;
CREATE TABLE af_t6 AS SELECT af_p(provenance()) AS p FROM af
  HAVING count(*) FILTER (WHERE v>5) < 2;
SELECT remove_provenance('af_t6'); SELECT 'scalar f < 2' AS q, p FROM af_t6;

-- ---------------------------------------------------------------------------
-- 2. NULL-skipping aggregates with FILTER
-- ---------------------------------------------------------------------------

-- sum over b (6), c (7).  >= 7: c present = 0.2
CREATE TABLE af_t7 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING sum(v) FILTER (WHERE v>5) >= 7;
SELECT remove_provenance('af_t7'); SELECT 'sum f >= 7' AS q, g, p FROM af_t7 ORDER BY g;

-- min over b, c.  = 6: b present = 0.4
CREATE TABLE af_t8 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING min(v) FILTER (WHERE v>5) = 6;
SELECT remove_provenance('af_t8'); SELECT 'min f = 6' AS q, g, p FROM af_t8 ORDER BY g;

-- avg over a (5), c (7).  > 5: c present = 0.2
CREATE TABLE af_t9 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING avg(v) FILTER (WHERE v<>6) > 5;
SELECT remove_provenance('af_t9'); SELECT 'avg f > 5' AS q, g, p FROM af_t9 ORDER BY g;

-- IS NULL: the group exists and no row passing the filter is present.
-- 1: a, not b, not c = 0.24   2,3: 0.5   4: 0.75
CREATE TABLE af_t10 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING sum(v) FILTER (WHERE v>5) IS NULL;
SELECT remove_provenance('af_t10'); SELECT 'sum f IS NULL' AS q, g, p FROM af_t10 ORDER BY g;

-- IS NOT NULL: b or c present.  1: 0.52   others: 0
CREATE TABLE af_t11 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING sum(v) FILTER (WHERE v>5) IS NOT NULL;
SELECT remove_provenance('af_t11'); SELECT 'sum f IS NOT NULL' AS q, g, p FROM af_t11 ORDER BY g;

-- bool_or over a, b only (c filtered out): never true.  Unfiltered: c = 0.2
CREATE TABLE af_t12 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=1
  GROUP BY g HAVING bool_or(v>6) FILTER (WHERE v<>7);
SELECT remove_provenance('af_t12'); SELECT 'bool_or f' AS q, g, p FROM af_t12 ORDER BY g;
CREATE TABLE af_t13 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=1
  GROUP BY g HAVING bool_or(v>6);
SELECT remove_provenance('af_t13'); SELECT 'bool_or' AS q, g, p FROM af_t13 ORDER BY g;

-- Expected values.  count: 0.4+0.2 = 0.6   sum: 6*0.4+7*0.2 = 3.8
CREATE TABLE af_t14 AS SELECT g,
    round(expected(count(*) FILTER (WHERE v>5))::numeric, 4) AS ec,
    round(expected(sum(v) FILTER (WHERE v>5))::numeric, 4) AS es
  FROM af WHERE g=1 GROUP BY g;
SELECT remove_provenance('af_t14'); SELECT 'expected' AS q, g, ec, es FROM af_t14;

-- ---------------------------------------------------------------------------
-- 3. Gates: a filtered and an unfiltered aggregate over one group are
--    different gates, with the children each one reads
-- ---------------------------------------------------------------------------

CREATE TABLE af_g AS SELECT g,
    count(*) AS c, count(*) FILTER (WHERE v>5) AS cf,
    sum(v) AS s, sum(v) FILTER (WHERE v>5) AS sf,
    string_agg(w, ',') FILTER (WHERE v<>5) AS st,
    array_agg(w) AS aa, array_agg(w) FILTER (WHERE v<>6) AS aaf,
    json_agg(w) AS ja
  FROM af WHERE g=1 GROUP BY g;
SELECT remove_provenance('af_g');
SELECT 'values' AS q, c, cf, s, sf, st, aa, aaf FROM af_g;
SELECT 'distinct gates' AS q,
       agg_token_uuid(c) <> agg_token_uuid(cf) AS count_differ,
       agg_token_uuid(s) <> agg_token_uuid(sf) AS sum_differ,
       agg_token_uuid(aa) <> agg_token_uuid(aaf) AS array_differ FROM af_g;

-- The values the children carry; the value gate of a NULL input is gate_null().
CREATE FUNCTION af_children(u uuid) RETURNS text LANGUAGE sql AS $$
  SELECT string_agg(
           CASE WHEN (provsql.get_children(ch))[2] = provsql.gate_null()
                THEN '<null>'
                ELSE provsql.get_extra((provsql.get_children(ch))[2]) END,
           ' ' ORDER BY (provsql.get_children(ch))[2] <> provsql.gate_null(),
                        provsql.get_extra((provsql.get_children(ch))[2]))
  FROM unnest(provsql.get_children(u)) AS ch $$;
SELECT 'children' AS q,
       af_children(agg_token_uuid(c))   AS count_all,    -- 1 1 1
       af_children(agg_token_uuid(cf))  AS count_filter, -- 0 1 1
       af_children(agg_token_uuid(sf))  AS sum_filter,   -- 6 7
       af_children(agg_token_uuid(st))  AS string_agg_f, -- q  (a filtered, b NULL)
       af_children(agg_token_uuid(aa))  AS array_all,    -- <null> p q
       af_children(agg_token_uuid(aaf)) AS array_filter, -- p q
       af_children(agg_token_uuid(ja))  AS json_all      -- <null> p q
  FROM af_g;

-- User-defined aggregates: a strict transition function never sees a NULL, so
-- the aggregate skips them; a non-strict one does, and its NULL inputs are
-- kept.  choose is non-strict but known to skip NULLs.  Inputs: v of g=1 (5,
-- 6, 7) with the value of b turned into NULL.
CREATE AGGREGATE af_strict_sum(int) (SFUNC = int4pl, STYPE = int);
CREATE AGGREGATE af_collect(int) (SFUNC = array_append, STYPE = int[], INITCOND = '{}');
CREATE TABLE af_u AS SELECT g,
    af_strict_sum(nullif(v,6)) AS us, af_collect(nullif(v,6)) AS uc,
    choose(nullif(v,6)) AS ch,
    af_collect(nullif(v,6)) FILTER (WHERE v<>5) AS ucf
  FROM af WHERE g=1 GROUP BY g;
SELECT remove_provenance('af_u');
SELECT 'user-defined' AS q,
       af_children(agg_token_uuid(us))  AS strict,          -- 5 7
       af_children(agg_token_uuid(uc))  AS nonstrict,       -- <null> 5 7
       af_children(agg_token_uuid(ch))  AS choose,          -- 5 7
       af_children(agg_token_uuid(ucf)) AS nonstrict_filter -- <null> 7
  FROM af_u;

-- Aggregates over random_variable: the result is built from the rows the
-- aggregate reads, so FILTER selects rows there too.  Certain rows (no
-- set_prob), x ~ N(10,1), N(20,1), N(30,1) and y ~ N(1,1), N(2,1), N(3,1) on
-- v = 5, 6, 7.  sum: 20+30   avg: 25   max (v<7): 20   min (v>5): 20
-- product of y: 2*3   stddev_pop of {20,30}: 5   median of {20,30}: 25
-- An empty selection is NULL.
CREATE TABLE af_rv(g int, v int, x random_variable, y random_variable);
INSERT INTO af_rv VALUES (1,5,normal(10,1),normal(1,1)),
  (1,6,normal(20,1),normal(2,1)), (1,7,normal(30,1),normal(3,1));
SELECT add_provenance('af_rv');
CREATE TABLE af_rvr AS SELECT g,
    expected(sum(x) FILTER (WHERE v>5)) AS s,
    expected(avg(x) FILTER (WHERE v>5)) AS a,
    round(expected(max(x) FILTER (WHERE v<7))::numeric, 0) AS mx,
    round(expected(min(x) FILTER (WHERE v>5))::numeric, 0) AS mn,
    expected(product(y) FILTER (WHERE v>5)) AS pr,
    round(expected(stddev_pop(x) FILTER (WHERE v>5))::numeric, 0) AS sd,
    round(expected(percentile_cont(0.5) WITHIN GROUP (ORDER BY x)
                   FILTER (WHERE v>5))::numeric, 0) AS med,
    expected(sum(x) FILTER (WHERE v>100)) AS empty_sum
  FROM af_rv GROUP BY g;
SELECT remove_provenance('af_rvr');
SELECT 'random_variable' AS q, s, a, mx, mn, pr, sd, med, empty_sum FROM af_rvr;

-- With uncertain rows (0.5 each): E[sum] = 0.5*20 + 0.5*30 = 25
CREATE TABLE af_rv2 AS SELECT * FROM af_rv;
SELECT remove_provenance('af_rv2'); SELECT add_provenance('af_rv2');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM af_rv2; END $$;
CREATE TABLE af_rvr2 AS SELECT g, expected(sum(x) FILTER (WHERE v>5)) AS s
  FROM af_rv2 GROUP BY g;
SELECT remove_provenance('af_rvr2');
SELECT 'random_variable p=0.5' AS q, s FROM af_rvr2;

-- ---------------------------------------------------------------------------
-- 4. Constant arithmetic is folded into a filtered aggregate
-- ---------------------------------------------------------------------------

CREATE TABLE af_f AS SELECT g, sum(v) FILTER (WHERE v>5) * 2 AS s2
  FROM af WHERE g=1 GROUP BY g;
SELECT remove_provenance('af_f');
SELECT 'folded' AS q, s2, get_gate_type(agg_token_uuid(s2)) AS gate,
       af_children(agg_token_uuid(s2)) AS children FROM af_f;   -- 26, agg, 12 14

-- >= 14 after doubling: c present = 0.2
CREATE TABLE af_t15 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=1
  GROUP BY g HAVING sum(v) FILTER (WHERE v>5) * 2 >= 14;
SELECT remove_provenance('af_t15'); SELECT 'sum f *2 >= 14' AS q, g, p FROM af_t15;

-- ---------------------------------------------------------------------------
-- 5. array_agg: NULL elements and FILTER
-- ---------------------------------------------------------------------------

-- = {p,NULL,q}: a, b, c all present = 0.04
CREATE TABLE af_t16 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=1
  GROUP BY g HAVING array_agg(w ORDER BY id) = ARRAY['p',NULL,'q'];
SELECT remove_provenance('af_t16'); SELECT 'aa = {p,NULL,q}' AS q, g, p FROM af_t16;

-- = {p,q}: a, c present and b absent = 0.5*0.6*0.2 = 0.06
CREATE TABLE af_t17 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=1
  GROUP BY g HAVING array_agg(w ORDER BY id) = ARRAY['p','q'];
SELECT remove_provenance('af_t17'); SELECT 'aa = {p,q}' AS q, g, p FROM af_t17;

-- The string 'NULL' is not the NULL element.  g=4: h carries 'NULL', i NULL.
-- {"NULL"}: h only = 0.25   {NULL}: i only = 0.25   {"NULL",NULL}: both = 0.25
CREATE TABLE af_t18 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=4
  GROUP BY g HAVING array_agg(w ORDER BY id) = ARRAY['NULL'];
SELECT remove_provenance('af_t18'); SELECT 'aa = {"NULL"}' AS q, g, p FROM af_t18;
CREATE TABLE af_t19 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=4
  GROUP BY g HAVING array_agg(w ORDER BY id) = ARRAY[NULL]::text[];
SELECT remove_provenance('af_t19'); SELECT 'aa = {NULL}' AS q, g, p FROM af_t19;
CREATE TABLE af_t20 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=4
  GROUP BY g HAVING array_agg(w ORDER BY id) = ARRAY['NULL',NULL];
SELECT remove_provenance('af_t20'); SELECT 'aa = {"NULL",NULL}' AS q, g, p FROM af_t20;
-- <>: every non-empty world but h alone = 0.75 - 0.25 = 0.5
CREATE TABLE af_t21 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=4
  GROUP BY g HAVING array_agg(w ORDER BY id) <> ARRAY['NULL'];
SELECT remove_provenance('af_t21'); SELECT 'aa <> {"NULL"}' AS q, g, p FROM af_t21;

-- Strings that resemble a seed of the NULL value gate stay strings.
-- {-null}: j present = 0.5   {NULL}: never
CREATE TABLE af_t27 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=5
  GROUP BY g HAVING array_agg(w) = ARRAY['-null'];
SELECT remove_provenance('af_t27'); SELECT 'aa = {-null}' AS q, g, p FROM af_t27;
CREATE TABLE af_t28 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=5
  GROUP BY g HAVING array_agg(w) = ARRAY[NULL]::text[];
SELECT remove_provenance('af_t28'); SELECT 'aa(-null) = {NULL}' AS q, g, p FROM af_t28;

-- FILTER removes b from the input: {p,q} needs a and c only = 0.1
CREATE TABLE af_t22 AS SELECT g, af_p(provenance()) AS p FROM af WHERE g=1
  GROUP BY g HAVING array_agg(w ORDER BY id) FILTER (WHERE v<>6) = ARRAY['p','q'];
SELECT remove_provenance('af_t22'); SELECT 'aa f = {p,q}' AS q, g, p FROM af_t22;

-- IS NULL: array_agg is NULL exactly when it reads no row, whatever the
-- values.  Without FILTER that never holds for an existing group; IS NOT NULL
-- is group existence.  1: 1-0.5*0.6*0.8 = 0.76   2,3: 0.5   4: 0.75
CREATE TABLE af_t23 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING array_agg(w) IS NULL;
SELECT remove_provenance('af_t23'); SELECT 'aa IS NULL' AS q, g, p FROM af_t23 ORDER BY g;
CREATE TABLE af_t24 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING array_agg(w) IS NOT NULL;
SELECT remove_provenance('af_t24'); SELECT 'aa IS NOT NULL' AS q, g, p FROM af_t24 ORDER BY g;

-- With FILTER (v>6): NULL when the group exists without c.
-- 1: (a or b), not c = 0.7*0.8 = 0.56   2: 0.5   3: 0.5 (NULL v fails)   4: 0.75
CREATE TABLE af_t25 AS SELECT g, af_p(provenance()) AS p FROM af
  GROUP BY g HAVING array_agg(w ORDER BY id) FILTER (WHERE v>6) IS NULL;
SELECT remove_provenance('af_t25'); SELECT 'aa f IS NULL' AS q, g, p FROM af_t25 ORDER BY g;

-- The NULL value gate: a constant the C++ evaluators know by its UUID, of type
-- value, displayed as NULL, and distinct from the value gate of the string.
CREATE TABLE af_t26 AS SELECT
    gate_null() = '417134e7-a404-57a7-86fd-2577ebe0f3ba'::uuid AS known_constant,
    get_gate_type(gate_null()) AS gate, sr_formula(gate_null()) AS shown,
    (SELECT count(*) FROM af_g, unnest(get_children(agg_token_uuid(aa))) AS ch
      WHERE (get_children(ch))[2] = gate_null()) AS null_children_g1;
SELECT remove_provenance('af_t26');
SELECT 'null value gate' AS q, known_constant, gate, shown, null_children_g1
  FROM af_t26;

DROP AGGREGATE af_strict_sum(int);
DROP AGGREGATE af_collect(int);
DROP FUNCTION af_children(uuid);
DROP FUNCTION af_p(uuid);
DROP TABLE af, af_g, af_f, af_u, af_rv, af_rvr, af_rv2, af_rvr2, af_t1, af_t2, af_t3, af_t4, af_t5, af_t6, af_t7, af_t8, af_t9, af_t10, af_t11, af_t12, af_t13,
  af_t14, af_t15, af_t16, af_t17, af_t18, af_t19, af_t20, af_t21, af_t22, af_t23, af_t24, af_t25, af_t26, af_t27, af_t28;
