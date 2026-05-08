\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

/* The capability semiring: ⊕ = bitwise OR (alternatives combine
   permissively), ⊗ = bitwise AND (joins require both flags),
   ⊖ = a AND NOT b (Boolean difference), δ = identity (preserves the
   capability of the supporting rows). Carrier is bit(2) over the
   diamond lattice {00, 01, 10, 11} with 00 as zero and 11 as one. */

CREATE FUNCTION cap_or(state bit(2), v bit(2))
  RETURNS bit(2) IMMUTABLE LANGUAGE SQL AS $$ SELECT state | v $$;
CREATE FUNCTION cap_and(state bit(2), v bit(2))
  RETURNS bit(2) IMMUTABLE LANGUAGE SQL AS $$ SELECT state & v $$;
CREATE FUNCTION cap_minus(a bit(2), b bit(2))
  RETURNS bit(2) IMMUTABLE LANGUAGE SQL AS $$ SELECT a & ~b $$;
CREATE FUNCTION cap_delta(v bit(2))
  RETURNS bit(2) IMMUTABLE LANGUAGE SQL AS $$ SELECT v $$;

CREATE AGGREGATE cap_plus(bit(2)) (
  sfunc = cap_or, stype = bit(2), initcond = '00'
);
CREATE AGGREGATE cap_times(bit(2)) (
  sfunc = cap_and, stype = bit(2), initcond = '11'
);

CREATE FUNCTION capability(token UUID, token2value regclass)
  RETURNS bit(2) AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    B'11'::bit(2),
    'cap_plus', 'cap_times', 'cap_minus', 'cap_delta');
END
$$ LANGUAGE plpgsql PARALLEL SAFE;

/* Map personnel.id to a bit(2) capability value. */
SELECT create_provenance_mapping('personnel_caps', 'personnel', $$
  CASE id
    WHEN 1 THEN B'11'::bit(2)  -- John,     NY
    WHEN 2 THEN B'10'::bit(2)  -- Paul,     NY
    WHEN 3 THEN B'11'::bit(2)  -- Dave,     Paris
    WHEN 4 THEN B'10'::bit(2)  -- Ellen,    Berlin
    WHEN 5 THEN B'01'::bit(2)  -- Magdalen, Paris
    WHEN 6 THEN B'11'::bit(2)  -- Nancy,    Paris
    WHEN 7 THEN B'01'::bit(2)  -- Susan,    Berlin
  END
$$);

/* Self-join exercises ⊗ within each pair and ⊕ across pairs. */
CREATE TABLE result_capability AS SELECT
  p1.city,
  capability(provenance(),'personnel_caps') AS cap
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_capability');
SELECT * FROM result_capability;

DROP TABLE result_capability;

/* EXCEPT exercises ⊖. The deduped 'Berlin' tuple has provenance
   ellen ⊕ susan; subtracting the 'Berlin' tuple from the WHERE-id=7
   side (provenance = susan) yields (ellen ⊕ susan) ⊖ susan. */
CREATE TABLE result_capability_except AS
SELECT label, capability(provenance(),'personnel_caps') AS cap FROM (
  SELECT city AS label FROM personnel WHERE city='Berlin'
  EXCEPT
  SELECT city AS label FROM personnel WHERE id=7
) p;

SELECT remove_provenance('result_capability_except');
SELECT * FROM result_capability_except;

DROP TABLE result_capability_except;
