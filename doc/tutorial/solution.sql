-- Q2
SET
  search_path TO public,
  provsql;

-- Q3
CREATE TABLE s AS
SELECT
  time,
  person.name AS person,
  p2.name AS witness,
  room.name AS room
FROM
  sightings
  JOIN person ON person = id
  JOIN person AS p2 ON witness = p2.id
  JOIN room ON room = room.id;

-- Q4
SELECT
  add_provenance ('s');

SELECT
  create_provenance_mapping ('witness_mapping', 's', 'witness');

SELECT
  *
FROM
  witness_mapping;

-- Q5
SELECT
  s1.time,
  s1.person,
  s1.room
FROM
  s AS s1,
  s AS s2
WHERE
  s1.person = s2.person
  AND s1.time = s2.time
  AND s1.room <> s2.room;

-- Q6
SELECT
  s1.time,
  s1.person,
  s1.room,
  sr_formula (provenance (), 'witness_mapping')
FROM
  s AS s1,
  s AS s2
WHERE
  s1.person = s2.person
  AND s1.time = s2.time
  AND s1.room <> s2.room;

-- Q7
CREATE TABLE consistent_s AS
SELECT
  time,
  person,
  room
FROM
  s
EXCEPT
SELECT
  s1.time,
  s1.person,
  s1.room
FROM
  s AS s1,
  s AS s2
WHERE
  s1.person = s2.person
  AND s1.time = s2.time
  AND s1.room <> s2.room;

SELECT
  *,
  sr_formula (provenance (), 'witness_mapping')
FROM
  consistent_s;

-- Q8
CREATE TABLE suspects AS
SELECT DISTINCT
  person
FROM
  consistent_s
WHERE
  room LIKE '% bedroom'
  AND time BETWEEN '00:00:00' AND '08:00:00';

SELECT
  *,
  sr_formula (provenance (), 'witness_mapping')
FROM
  suspects;

-- Q9
SELECT
  set_config ('setting.disable_update_trigger', 'on', false);

ALTER table s
ADD COLUMN count int;

UPDATE s
SET
  count = 1;

SELECT
  create_provenance_mapping ('count_mapping', 's', 'count');

SELECT
  *,
  sr_counting (provenance (), 'count_mapping') AS c
FROM
  suspects
ORDER BY
  c;

-- Q10
ALTER table s
ADD COLUMN reliability float;

UPDATE s
SET
  reliability = score
FROM
  reliability,
  person
WHERE
  reliability.person = person.id
  AND person.name = s.witness;

SELECT
  set_prob (provenance (), reliability)
FROM
  s;

-- Q11
SELECT
  *,
  sr_formula (provenance (), 'witness_mapping'),
  probability_evaluate (provenance (), 'possible-worlds')
FROM
  suspects
WHERE
  probability_evaluate (provenance (), 'possible-worlds') > 0.99
  AND person <> 'Daphine';
