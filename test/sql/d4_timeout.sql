\set ECHO none
\if `which d4 > /dev/null 2>&1 && echo true || echo false`
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- 100x100 random-probability matrix (matches the casestudy1 documentation).
-- The self-join below produces a circuit whose per-group d4 compilations
-- collectively exceed the 200ms statement_timeout, reliably triggering a
-- cancel during the system(d4) wait inside BooleanCircuit::compilation.
CREATE TABLE matrix AS
SELECT ones.n + 10 * tens.n  AS x,
       other.n + 10 * tens2.n AS y,
       random() AS prob
FROM (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)) ones(n),
     (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)) tens(n),
     (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)) other(n),
     (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)) tens2(n);

SELECT add_provenance('matrix');
DO $$ BEGIN
    PERFORM set_prob(provenance(), prob) FROM matrix;
END $$;

-- Without the CHECK_FOR_INTERRUPTS poll added after system() in
-- BooleanCircuit::compilation, the cancel raised by statement_timeout is
-- consumed by the throw of "Unreadable d-DNNF" (XX000) that follows when
-- the d4 subprocess returns a partial .nnf, masking the timeout. The
-- DO/EXCEPTION wrapper normalises the surface error so the comparison is
-- sqlstate-only rather than depending on how many groups had completed
-- before the cancel fired (which is timing-dependent).
SET statement_timeout = '200ms';

DO $$
BEGIN
    PERFORM m1.x, m2.y,
            probability_evaluate(provenance(), 'compilation', 'd4') AS prob
    FROM matrix m1, matrix m2
    WHERE m2.x = m1.y AND m1.x > 90 AND m2.x > 90 AND m2.y > 90
    GROUP BY m1.x, m2.y
    ORDER BY m1.x, m2.y;
    RAISE NOTICE 'unexpected: query finished within timeout';
EXCEPTION
    WHEN query_canceled THEN
        RAISE NOTICE 'timeout raised as query_canceled';
END $$;

RESET statement_timeout;

SELECT remove_provenance('matrix');
DROP TABLE matrix;
\else
\echo 'SKIPPING: d4 not available'
\endif
