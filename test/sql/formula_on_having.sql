\set ECHO none
\pset format unaligned

-- formula() does not support semimod gates created by HAVING;
-- this should produce a clear error message
SELECT city, COUNT(*), formula(provenance(), 'personnel_name')
FROM personnel
GROUP BY city
HAVING COUNT(*) > 2;

-- One comparison gate the possible-world enumeration cannot handle must not
-- cost the ones it can.  fh_big carries an agg-vs-agg comparison over more
-- contributors than the enumeration's cap, so it stays an opaque cmp that
-- sr_formula renders literally; fh_small's comparison is well within reach
-- and has to come out fully expanded even when both live in one circuit.
CREATE TABLE fh_small(g int, v int);
INSERT INTO fh_small VALUES (1,10),(1,20),(1,30);
CREATE TABLE fh_big(g int, v bigint);
INSERT INTO fh_big SELECT 1, (i * 1000000)::bigint FROM generate_series(1,25) i;
SELECT add_provenance('fh_small');
SELECT add_provenance('fh_big');
SELECT create_provenance_mapping('fh_small_v', 'fh_small', 'v');

-- Alone, the small comparison expands into its satisfying worlds.
CREATE TABLE fh_alone AS
  SELECT position('(10 ⊗ 20 ⊗ 30)' in sr_formula(provenance(), 'fh_small_v')) > 0
           AS expanded
  FROM fh_small GROUP BY g HAVING count(*) >= 2;
SELECT remove_provenance('fh_alone');
SELECT 'small comparison alone' AS circuit, expanded FROM fh_alone;
DROP TABLE fh_alone;

-- Together with the unresolvable one, it must still expand.
CREATE TABLE fh_mixed AS
  SELECT position('(10 ⊗ 20 ⊗ 30)' in sr_formula(provenance(), 'fh_small_v')) > 0
           AS expanded
  FROM (SELECT g, provenance() AS ps FROM fh_small GROUP BY g HAVING count(*) >= 2) s,
       (SELECT g, provenance() AS pb FROM fh_big GROUP BY g HAVING sum(v) > count(*)) b;
SELECT remove_provenance('fh_mixed');
SELECT 'both in one circuit' AS circuit, expanded FROM fh_mixed;
DROP TABLE fh_mixed;

DROP TABLE fh_small_v;
DROP TABLE fh_small;
DROP TABLE fh_big;
