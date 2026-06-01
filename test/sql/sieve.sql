\set ECHO none
-- Exact inclusion-exclusion (sieve) over a monotone DNF ('sieve' method,
-- BooleanCircuit::sieve).  A portfolio member that is exact and O(2^m) in the
-- clause count m -- cheapest when m is small.  Being exact, it must agree
-- bit-for-bit with the other exact methods (possible-worlds /
-- tree-decomposition), unlike the randomised karp-luby / stopping-rule.  It
-- shares karp-luby's DNF-shape requirement (an OR below an AND is refused).
-- Circuits are built from the public token combinators with
-- provsql.boolean_provenance off so the load-time folding leaves the shape.
\pset format unaligned
SET search_path TO provsql_test,provsql;
SET provsql.boolean_provenance = off;

CREATE TABLE sv_in(id int);
INSERT INTO sv_in VALUES (1),(2),(3),(4);
SELECT add_provenance('sv_in');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM sv_in WHERE id=1;
  PERFORM set_prob(provsql, 0.3) FROM sv_in WHERE id=2;
  PERFORM set_prob(provsql, 0.4) FROM sv_in WHERE id=3;
  PERFORM set_prob(provsql, 0.1) FROM sv_in WHERE id=4;
END $$;

DO $$
DECLARE x1 uuid; x2 uuid; x3 uuid; x4 uuid;
BEGIN
  SELECT provsql INTO x1 FROM sv_in WHERE id=1;
  SELECT provsql INTO x2 FROM sv_in WHERE id=2;
  SELECT provsql INTO x3 FROM sv_in WHERE id=3;
  SELECT provsql INTO x4 FROM sv_in WHERE id=4;
  -- (i) flat DNF, disjoint clauses: (x1 AND x2) OR (x3 AND x4) = 0.184.
  PERFORM set_config('sv.flat', provenance_plus(ARRAY[
            provenance_times(x1,x2), provenance_times(x3,x4)])::text, false);
  -- (ii) one-level sharing (overlapping clauses, where inclusion-exclusion
  --      earns its keep): (x1 AND x2) OR (x1 AND x3) = 0.29.
  PERFORM set_config('sv.shared', provenance_plus(ARRAY[
            provenance_times(x1,x2), provenance_times(x1,x3)])::text, false);
  -- (iii) clause subsumption: x1 OR (x1 AND x2) = P(x1) = 0.5.
  PERFORM set_config('sv.subsume', provenance_plus(ARRAY[
            x1, provenance_times(x1,x2)])::text, false);
  -- (iv) NON-DNF (an OR below an AND): (x1 OR x2) AND x3 -- refused.
  PERFORM set_config('sv.nondnf', provenance_times(
            provenance_plus(ARRAY[x1,x2]), x3)::text, false);
END $$;

\set flat    '(current_setting(''sv.flat'')::uuid)'
\set shared  '(current_setting(''sv.shared'')::uuid)'
\set subsume '(current_setting(''sv.subsume'')::uuid)'
\set nondnf  '(current_setting(''sv.nondnf'')::uuid)'

-- (i)-(iii): sieve is exact -- it agrees with another exact method to within
-- floating-point round-off (the inclusion-exclusion sum and the possible-world
-- sum associate the same terms differently, so bit-equality is too strict), and
-- equals the hand-computed value.
SELECT 'flat'    AS circuit,
       round(probability_evaluate(:flat,   'sieve')::numeric,3) AS sieve,
       abs(probability_evaluate(:flat,   'sieve') - probability_evaluate(:flat,   'possible-worlds'))    < 1e-12 AS eq_exact
UNION ALL
SELECT 'shared',
       round(probability_evaluate(:shared, 'sieve')::numeric,3),
       abs(probability_evaluate(:shared, 'sieve') - probability_evaluate(:shared, 'tree-decomposition')) < 1e-12
UNION ALL
SELECT 'subsume',
       round(probability_evaluate(:subsume,'sieve')::numeric,3),
       abs(probability_evaluate(:subsume,'sieve') - probability_evaluate(:subsume,'possible-worlds'))    < 1e-12
ORDER BY circuit;

-- (iv) a non-DNF circuit is refused (warning + error), like karp-luby.
SELECT probability_evaluate(:nondnf,'sieve');

DROP TABLE sv_in;
RESET provsql.boolean_provenance;
