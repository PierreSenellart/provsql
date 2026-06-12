\set ECHO none
\pset format unaligned

-- probability_bounds(token) returns a cheap CERTIFIED interval [lower,upper]
-- with lower <= probability_evaluate(token) <= upper, computed without
-- compiling the circuit (the Olteanu-Huang-Koch d-tree leaf bound, ICDE 2010
-- Fig. 3).  It is deterministic (no sampling), so the expected values are
-- exact.  boolean_provenance off so the load-time fold leaves the DNF intact.
SET provsql.provenance = 'semiring';

CREATE TABLE pb(id int);
INSERT INTO pb SELECT generate_series(1,6);
SELECT add_provenance('pb');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.1*id) FROM pb; END $$;

-- Build the witness circuits with the rewriter off (we manipulate the provsql
-- column directly, which the planner hook would otherwise intercept).
SET provsql.active = off;
DO $$
DECLARE x uuid[];
BEGIN
  SELECT array_agg(provsql::uuid ORDER BY id) INTO x FROM pb;
  -- (a) read-once DNF: (x1 x2) OR (x3 x4) OR (x5 x6).  The clauses have disjoint
  --     supports, so the bound's single bucket gives lower = upper = exact.
  PERFORM set_config('pb.ro', provenance_plus(ARRAY[
     provenance_times(x[1],x[2]), provenance_times(x[3],x[4]),
     provenance_times(x[5],x[6])])::text, false);
  -- (b) shared-variable DNF: (x1 x2) OR (x1 x3).  x1 is in both clauses, so they
  --     fall in different buckets and the interval is non-trivial but sound.
  PERFORM set_config('pb.sh', provenance_plus(ARRAY[
     provenance_times(x[1],x[2]), provenance_times(x[1],x[3])])::text, false);
  -- (c) non-DNF: AND-of-ORs (x1 OR x2) AND (x3 OR x4) -- a times over plus gates,
  --     which is not the monotone OR-of-ANDs shape the leaf bound requires.
  PERFORM set_config('pb.cnf', provenance_times(
     provenance_plus(ARRAY[x[1],x[2]]),
     provenance_plus(ARRAY[x[3],x[4]]))::text, false);
END $$;
RESET provsql.active;

-- (a) Read-once: the interval collapses to the exact probability.
SELECT round(lower::numeric,6) AS ro_lo,
       round(upper::numeric,6) AS ro_hi,
       round(probability_evaluate(current_setting('pb.ro')::uuid)::numeric,6) AS ro_exact
FROM probability_bounds(current_setting('pb.ro')::uuid);

-- (b) Shared x1: bounds bracket the exact value and the interval is non-trivial.
SELECT round(lower::numeric,6) AS sh_lo,
       round(upper::numeric,6) AS sh_hi,
       round(probability_evaluate(current_setting('pb.sh')::uuid)::numeric,6) AS sh_exact,
       lower <= probability_evaluate(current_setting('pb.sh')::uuid)
         AND probability_evaluate(current_setting('pb.sh')::uuid) <= upper AS sound,
       lower < upper AS nontrivial
FROM probability_bounds(current_setting('pb.sh')::uuid);

-- (c) Non-DNF circuit: errors cleanly (the leaf bound is DNF-specific).
SELECT lower FROM probability_bounds(current_setting('pb.cnf')::uuid);

SELECT remove_provenance('pb');
DROP TABLE pb;
RESET provsql.provenance;
