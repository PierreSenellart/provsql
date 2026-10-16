\set ECHO none
\pset format unaligned

CREATE TABLE result_choose AS SELECT
  city, choose(position) AS position, sr_formula(choose(position), 'personnel_name') AS formula
FROM personnel
GROUP BY city;

SELECT remove_provenance('result_choose');
SELECT city, position, formula FROM result_choose ORDER BY city;

DROP TABLE result_choose;

-- choose() reads its rows in order: where two rows tie on its ORDER BY (or
-- there is none) with different values, in any world, a warning says so, as
-- for array_agg.  So does the choose() a scalar subquery or a LATERAL join with
-- ORDER BY ... LIMIT 1 is read as.  Ordered by a key, or with values that
-- agree, nothing is said.
CREATE TABLE ch_k(id int);
INSERT INTO ch_k VALUES (1);
CREATE TABLE ch_r(id int, t int, v int);
INSERT INTO ch_r VALUES (1, 5, 10), (1, 5, 20), (1, 3, 30);
SELECT add_provenance('ch_r');
CREATE TABLE ch_res AS
  SELECT ch_k.id, (SELECT v FROM ch_r WHERE ch_r.id = ch_k.id
                   ORDER BY t DESC LIMIT 1) AS v
  FROM ch_k;
DROP TABLE ch_res;
CREATE TABLE ch_res AS
  SELECT ch_k.id, s.v FROM ch_k LEFT JOIN LATERAL
    (SELECT v FROM ch_r WHERE ch_r.id = ch_k.id ORDER BY t DESC LIMIT 1) s
    ON true;
DROP TABLE ch_res;
CREATE TABLE ch_res AS SELECT id, choose(v) FROM ch_r GROUP BY id;
DROP TABLE ch_res;
\echo -- no warning: ordered by a key, values that agree, a tie broken
CREATE TABLE ch_res AS SELECT id, choose(v ORDER BY v) FROM ch_r GROUP BY id;
DROP TABLE ch_res;
CREATE TABLE ch_res AS SELECT id, choose(id) FROM ch_r GROUP BY id;
DROP TABLE ch_res;
UPDATE ch_r SET t = 4 WHERE v = 20;
CREATE TABLE ch_res AS
  SELECT ch_k.id, (SELECT v FROM ch_r WHERE ch_r.id = ch_k.id
                   ORDER BY t DESC LIMIT 1) AS v
  FROM ch_k;
SELECT remove_provenance('ch_res');
SELECT id, v FROM ch_res;
DROP TABLE ch_res;
SELECT remove_provenance('ch_r');
DROP TABLE ch_k, ch_r;
