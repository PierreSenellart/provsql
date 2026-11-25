\set ECHO none
\pset format unaligned

-- provenance() in a FROM clause is a function call, not the provenance of
-- the row: it is not rewritten, and gives the semiring one, while the row
-- keeps its own provenance in the provsql column.
CREATE TABLE provenance_in_from_result AS
SELECT name, p = gate_one() AS from_one, provsql <> gate_one() AS row_tracked
FROM provenance() p, personnel;

SELECT remove_provenance('provenance_in_from_result');

SELECT * FROM provenance_in_from_result ORDER BY name;
DROP TABLE provenance_in_from_result;
