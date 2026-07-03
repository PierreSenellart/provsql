\set ECHO none
\pset format unaligned

-- provsql.nonzero / provsql.present: explicit zero-filtering.
-- Default mode: false only on a structural proof of universal zero-ness
-- (zero in every m-semiring under every leaf valuation), with comparison
-- gates resolved through their possible-worlds expansion; true otherwise.
-- Named modes evaluate on this instance: 'boolean' (presence in the
-- vanilla answer; present() is its shorthand) and 'counting' (bag
-- multiplicity), with an absent mapping reading every leaf as 1.

CREATE TABLE nz(a int, name text);
INSERT INTO nz VALUES (1,'x'),(1,'y');
SELECT add_provenance('nz');

-- Hand-built (x ⊕ y) ⊖ x: not universally zero (counting gives 1), zero
-- in the Boolean semiring with all leaves true.
CREATE TABLE hb AS SELECT provsql AS t, name FROM nz;
DO $$
DECLARE tx uuid; ty uuid;
        p uuid := public.uuid_generate_v4();
        m uuid := public.uuid_generate_v4();
BEGIN
  SELECT t INTO tx FROM hb WHERE name='x';
  SELECT t INTO ty FROM hb WHERE name='y';
  PERFORM create_gate(p, 'plus', ARRAY[tx, ty]);
  PERFORM create_gate(m, 'monus', ARRAY[p, tx]);
  CREATE TEMP TABLE hbres AS
    SELECT nonzero(m) AS dflt, nonzero(m,'boolean') AS bool,
           nonzero(m,'counting') AS cnt, present(m) AS prs;
END $$;
SELECT * FROM hbres;
DROP TABLE hb;

-- The EXCEPT-dedup row (x ⊖ x) ⊕ (y ⊖ x): the per-copy bag difference is
-- 0 in counting too (each left copy is cancelled by the right x), unlike
-- the abstract (x ⊕ y) ⊖ x above -- only the default mode keeps it.
CREATE TABLE nz1 AS
  SELECT a, nonzero(provenance()) AS dflt,
         nonzero(provenance(),'boolean') AS bool,
         nonzero(provenance(),'counting') AS cnt,
         present(provenance()) AS prs
  FROM (SELECT a FROM nz EXCEPT SELECT a FROM nz WHERE name='x') t;
SELECT remove_provenance('nz1');
SELECT * FROM nz1;
DROP TABLE nz1;

-- IN with no match: a structural zero, dropped even by the default mode.
CREATE TABLE nz2 AS
  SELECT a, nonzero(provenance()) AS dflt, present(provenance()) AS prs
  FROM nz WHERE a IN (SELECT a FROM nz WHERE a = 99);
SELECT remove_provenance('nz2');
SELECT * FROM nz2 ORDER BY a;
DROP TABLE nz2;

-- LEFT JOIN arms: the matched arm is present; the null-padded antijoin
-- arm is zero on this instance ('boolean' false) but not universally
-- zero (default true).
CREATE TABLE nzr(k int, name text); INSERT INTO nzr VALUES (1,'r');
CREATE TABLE nzs(k int, name text); INSERT INTO nzs VALUES (1,'s');
SELECT add_provenance('nzr');
SELECT add_provenance('nzs');
CREATE TABLE nz3 AS
  SELECT nzr.k AS rk, nzs.k AS sk,
         nonzero(provenance()) AS dflt, present(provenance()) AS prs
  FROM nzr LEFT JOIN nzs ON nzr.k = nzs.k;
SELECT remove_provenance('nz3');
SELECT * FROM nz3 ORDER BY sk NULLS LAST;
DROP TABLE nz3;
DROP TABLE nzr;
DROP TABLE nzs;

-- HAVING comparison gates are expanded: an empty satisfying-world set is
-- universally zero (default false), a satisfiable one is kept.
CREATE TABLE hv(g int, b int, name text);
INSERT INTO hv VALUES (1, 10, 'h1');
SELECT add_provenance('hv');
CREATE TABLE hv1 AS
  SELECT g, nonzero(provenance()) AS dflt, present(provenance()) AS prs
  FROM hv GROUP BY g HAVING sum(b) < 5;
SELECT remove_provenance('hv1');
SELECT 'sum<5' AS q, * FROM hv1;
DROP TABLE hv1;
CREATE TABLE hv2 AS
  SELECT g, nonzero(provenance()) AS dflt, present(provenance()) AS prs
  FROM hv GROUP BY g HAVING sum(b) > 5;
SELECT remove_provenance('hv2');
SELECT 'sum>5' AS q, * FROM hv2;
DROP TABLE hv2;
DROP TABLE hv;

-- An undecided probabilistic comparison is a probabilistic event, not a
-- zero-testable one: opaque for the default mode (kept).
CREATE TABLE rv(id int, v provsql.random_variable);
INSERT INTO rv VALUES (1, provsql.normal(0,1));
SELECT add_provenance('rv');
CREATE TABLE rv1 AS
  SELECT id, nonzero(provenance()) AS dflt
  FROM rv WHERE v > provsql.as_random(0);
SELECT remove_provenance('rv1');
SELECT * FROM rv1;
DROP TABLE rv1;
DROP TABLE rv;

-- An RV comparison decided false by support analysis (RangeCheck at
-- circuit load, provsql.simplify_on_load): universally zero.
CREATE TABLE rvd(id int, v provsql.random_variable);
INSERT INTO rvd VALUES (1, provsql.uniform(0,1));
SELECT add_provenance('rvd');
CREATE TABLE rvd1 AS
  SELECT id, nonzero(provenance()) AS dflt
  FROM rvd WHERE v > provsql.as_random(2);
SELECT remove_provenance('rvd1');
SELECT * FROM rvd1;
DROP TABLE rvd1;
DROP TABLE rvd;

-- A larger HAVING group: the world enumeration is uncapped in this
-- context and still answers (the monotone shortcuts keep it fast).
CREATE TABLE big(g int, b int);
INSERT INTO big SELECT 1, 10 FROM generate_series(1,15);
SELECT add_provenance('big');
CREATE TABLE big1 AS
  SELECT g, nonzero(provenance()) AS dflt
  FROM big GROUP BY g HAVING sum(b) < 5;
SELECT remove_provenance('big1');
SELECT * FROM big1;
DROP TABLE big1;
DROP TABLE big;

-- Structural zero propagation through hand-built gates: an all-zero ⊕,
-- a δ over zero, and a semimod whose provenance side is zero are all
-- provably (universally) zero.
DO $$
DECLARE pz uuid := public.uuid_generate_v4();
        dz uuid := public.uuid_generate_v4();
        sz uuid := public.uuid_generate_v4();
BEGIN
  PERFORM create_gate(pz, 'plus',    ARRAY[gate_zero(), gate_zero()]);
  PERFORM create_gate(dz, 'delta',   ARRAY[gate_zero()]);
  PERFORM create_gate(sz, 'semimod', ARRAY[gate_zero(), gate_one()]);
  CREATE TEMP TABLE zres AS
    SELECT nonzero(pz) AS plus_zero, nonzero(dz) AS delta_zero,
           nonzero(sz) AS semimod_zero;
END $$;
SELECT * FROM zres;
DROP TABLE zres;

-- A NULL token is the neutral 1: kept in every mode.
SELECT nonzero(NULL) AS dflt, nonzero(NULL,'boolean') AS bool,
       present(NULL) AS prs;

-- Mapping honored in the named modes: x maps to false, y to true.
CREATE TABLE nzmap AS
  SELECT (name='y')::text AS value, provsql AS provenance FROM nz;
CREATE TABLE nz4 AS
  SELECT name, nonzero(provsql, 'boolean', 'nzmap') AS bool FROM nz;
SELECT remove_provenance('nz4');
SELECT * FROM nz4 ORDER BY name;
DROP TABLE nz4;
DROP TABLE nzmap;

-- Unsupported semiring: actionable error.
SELECT nonzero(public.uuid_generate_v4(), 'formula');

SELECT remove_provenance('nz');
DROP TABLE nz;
