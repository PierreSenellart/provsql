\set ECHO none
\pset format unaligned

-- An error caught in a subtransaction nested deeper than 64 levels must not
-- silence the warnings of the rest of the transaction.  ProvSQL tells the
-- statements a user runs from those its own functions run by the depth of
-- nested executors; an aborted subtransaction puts back the depth it started
-- at, which was kept for 64 levels only, so that an error past them left the
-- depth raised, and the warnings that only the user's statements give were
-- given no more.
CREATE TABLE sd_t(g int, x int, y int);
INSERT INTO sd_t VALUES (1, 10, 1), (1, 20, 2);
SELECT add_provenance('sd_t');

-- n nested exception blocks, the innermost catching an error raised while a
-- query runs.
CREATE FUNCTION sd_deep(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  IF n = 0 THEN
    BEGIN
      PERFORM 1 / (SELECT 0);
    EXCEPTION WHEN division_by_zero THEN
      NULL;
    END;
  ELSE
    -- A block with an EXCEPTION clause runs in a subtransaction of its own.
    BEGIN
      PERFORM sd_deep(n - 1);
    EXCEPTION WHEN raise_exception THEN
      RAISE;
    END;
  END IF;
END $$;

BEGIN;
SELECT sd_deep(70);
-- An aggregate read as a plain value: ProvSQL warns that it does not track it.
CREATE TABLE sd_r AS SELECT g, (array_agg(x ORDER BY y))[1] AS f FROM sd_t GROUP BY g;
SELECT remove_provenance('sd_r');
SELECT g, f FROM sd_r;
ROLLBACK;

DROP FUNCTION sd_deep(int);
DROP TABLE sd_t;
