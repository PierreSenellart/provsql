\set ECHO none
\pset format unaligned

-- INSERT ... SELECT propagates provenance
CREATE TABLE insert_prov_result (name varchar, city varchar);
SELECT add_provenance('insert_prov_result');
INSERT INTO insert_prov_result SELECT name, city FROM personnel WHERE city='Paris';

CREATE TABLE insert_prov_eval AS
  SELECT name, city, sr_formula(provenance(), 'personnel_name') AS formula
  FROM insert_prov_result;
SELECT remove_provenance('insert_prov_eval');
SELECT * FROM insert_prov_eval ORDER BY name;

DROP TABLE insert_prov_eval;
DROP TABLE insert_prov_result;

-- INSERT ... SELECT with join propagates combined provenance
CREATE TABLE insert_prov_join (name1 varchar, name2 varchar);
SELECT add_provenance('insert_prov_join');
INSERT INTO insert_prov_join
  SELECT p1.name, p2.name FROM personnel p1 JOIN personnel p2
  ON p1.city = p2.city AND p1.id < p2.id WHERE p1.city='Paris';

CREATE TABLE insert_prov_join_eval AS
  SELECT name1, name2, sr_formula(provenance(), 'personnel_name') AS formula
  FROM insert_prov_join;
SELECT remove_provenance('insert_prov_join_eval');
SELECT * FROM insert_prov_join_eval ORDER BY name1, name2;

DROP TABLE insert_prov_join_eval;
DROP TABLE insert_prov_join;

-- INSERT ... SELECT into table without provsql column warns
CREATE TABLE insert_no_prov (name varchar, city varchar);
INSERT INTO insert_no_prov SELECT name, city FROM personnel WHERE city='Paris';
DROP TABLE insert_no_prov;

-- An aggregate over a tracked relation is retyped to agg_token by the planner
-- hook, long after parse analysis fixed the INSERT's target row type.  The
-- retyped columns are cast back to the declared types (the agg_token
-- assignment casts, which yield the running value), so the ordinary
-- "INSERT INTO summary SELECT count(*) ..." shape works whatever the target
-- column type is -- with a warning that the aggregate's provenance is dropped.
CREATE TABLE insert_agg_plain (c bigint, s numeric);
INSERT INTO insert_agg_plain SELECT count(*), sum(id) FROM personnel;
SELECT * FROM insert_agg_plain;
DROP TABLE insert_agg_plain;

-- Same with a GROUP BY, and with an explicit cast the hook rewrites underneath.
CREATE TABLE insert_agg_group (city varchar, n bigint);
INSERT INTO insert_agg_group
  SELECT city, count(*)::bigint FROM personnel GROUP BY city;
SELECT * FROM insert_agg_group ORDER BY city;
DROP TABLE insert_agg_group;

-- With a provenance-tracked target the row still gets its existence
-- provenance; only the aggregate value's own provenance is what the bigint
-- column cannot hold.
CREATE TABLE insert_agg_tracked (n bigint);
SELECT add_provenance('insert_agg_tracked');
INSERT INTO insert_agg_tracked SELECT count(*) FROM personnel WHERE city='Paris';
CREATE TABLE insert_agg_tracked_eval AS
  SELECT n, get_gate_type(provenance()) AS token_kind FROM insert_agg_tracked;
SELECT remove_provenance('insert_agg_tracked_eval');
SELECT * FROM insert_agg_tracked_eval;
DROP TABLE insert_agg_tracked_eval;
DROP TABLE insert_agg_tracked;

-- An INSERT ... SELECT over a chain of outer joins: the lowering of the chain
-- shares its inputs through CTEs, which go to the WITH clause of the source
-- SELECT, the statement's top level, as they go to a SELECT's.  Into a
-- tracked target, the rows of every world, each with its provenance: those
-- of the data as it is, every input row being certain, are plain SQL's 7.
CREATE TABLE insert_oj (id int, city text, other int);
SELECT add_provenance('insert_oj');
INSERT INTO insert_oj
  SELECT p.id, p.city, q.id
  FROM personnel p LEFT JOIN personnel q ON q.id = p.id + 1
                   LEFT JOIN personnel r ON r.id = q.id + 1;
CREATE TABLE insert_oj_r AS
  SELECT count(*) FILTER (WHERE probability_evaluate(provenance()) > 0) AS n
  FROM insert_oj;
SELECT remove_provenance('insert_oj_r');
SELECT n::text AS n FROM insert_oj_r;
DROP TABLE insert_oj_r;
SELECT remove_provenance('insert_oj');
DROP TABLE insert_oj;
