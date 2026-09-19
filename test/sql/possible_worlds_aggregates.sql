\set ECHO none
\pset format unaligned

-- The probability of a comparison of aggregate results (here the rank of a
-- row under an ORDER BY ... LIMIT) over few input tuples: the worlds of the
-- inputs are enumerated on the circuit as it is, the comparison read from the
-- value it takes in each.  The Boolean form of such a circuit has one term
-- per subset of the rows the comparison aggregates, which is the more
-- expensive of the two as soon as they outnumber the input tuples.

CREATE TABLE pwc_u(id int, rep int);
CREATE TABLE pwc_p(id int);
INSERT INTO pwc_u VALUES (1, 40), (2, 30), (3, 20), (4, 10);
INSERT INTO pwc_p VALUES (1), (2), (3), (4);
SELECT add_provenance('pwc_u');
SELECT add_provenance('pwc_p');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.5) FROM pwc_u;
  PERFORM set_prob(provenance(), 0.5) FROM pwc_p;
END $$;

-- The cross product ranked on rep: each rank compares a count over up to 15
-- of its rows with the limit, more than the eight input tuples, so the worlds
-- of the inputs are enumerated on the circuit.  Only the rows of the two
-- lowest ranks are shown, the others being certain to be kept.
CREATE TABLE pwc_r AS
  SELECT u.id AS uid, p.id AS pid,
         round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM pwc_u u, pwc_p p ORDER BY u.rep DESC LIMIT 4;
SELECT remove_provenance('pwc_r');
SELECT * FROM pwc_r WHERE uid >= 4 ORDER BY uid, pid;
SHOW provsql.last_eval_method;
DROP TABLE pwc_r;

-- A HAVING comparison over as many rows as there are inputs keeps the
-- resolution and its closed forms (here the scan over a sum).
CREATE TABLE pwc_h AS
  SELECT id, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM pwc_u GROUP BY id HAVING sum(rep) > 15;
SELECT remove_provenance('pwc_h');
SELECT * FROM pwc_h ORDER BY id;
SHOW provsql.last_eval_method;
DROP TABLE pwc_h;

DROP TABLE pwc_u, pwc_p;
