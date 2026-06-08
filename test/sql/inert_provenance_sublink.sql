\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- provenance() inside a scalar SubLink is a scope-local, INERT token
-- fetch: it resolves to the subquery's own provenance token and returns
-- it as a plain uuid value, WITHOUT coupling that relation into the
-- outer row's lineage.  This is the basis of the conditioning operator's
-- evidence.  Only a simple `SELECT provenance() FROM R WHERE ...` shape
-- is inert; aggregated / EXISTS / IN forms stay rejected.

CREATE TABLE isub(id int, v text);
INSERT INTO isub VALUES (1,'a'),(2,'b'),(3,'c');
SELECT add_provenance('isub');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM isub; END $$;

-- No coupling: the outer row's provenance stays the BARE row token, so
-- its probability is .5 (= P(id=1)), not .25 (= P(id=1 AND id=2)); the
-- fetched evidence token is id=2's own token (probability .5) and is
-- distinct from the row's.
CREATE TABLE r1 AS
  SELECT t1.id,
         provenance() AS row_tok,
         (SELECT provenance() FROM isub t2 WHERE t2.id = 2) AS ev_tok
  FROM isub t1 WHERE t1.id = 1;
SELECT remove_provenance('r1');
SELECT round(probability_evaluate(row_tok)::numeric, 3) AS p_row,
       round(probability_evaluate(ev_tok)::numeric, 3)  AS p_ev,
       row_tok <> ev_tok AS distinct_tokens
FROM r1;
DROP TABLE r1;

-- Correlated per-row evidence (the conditioning shape): every row keeps
-- its bare token and fetches its own neighbour's token inertly.
CREATE TABLE r2 AS
  SELECT t1.id,
         provenance() AS row_tok,
         (SELECT provenance() FROM isub t2 WHERE t2.id = t1.id + 1) AS ev_tok
  FROM isub t1;
SELECT remove_provenance('r2');
SELECT id,
       round(probability_evaluate(row_tok)::numeric, 3) AS p_row,
       CASE WHEN ev_tok IS NULL THEN NULL
            ELSE round(probability_evaluate(ev_tok)::numeric, 3) END AS p_ev
FROM r2 ORDER BY id;
DROP TABLE r2;

-- Inert fetch in WHERE position: still no coupling (row stays bare, p .5).
CREATE TABLE r3 AS
  SELECT t1.id, provenance() AS row_tok
  FROM isub t1
  WHERE t1.id = 1
    AND (SELECT provenance() FROM isub t2 WHERE t2.id = 2) IS NOT NULL;
SELECT remove_provenance('r3');
SELECT round(probability_evaluate(row_tok)::numeric, 3) AS p_row FROM r3;
DROP TABLE r3;

-- Still rejected: provenance() in EXISTS, and in an aggregated subselect
-- (those carry probabilistic-provenance semantics an inert fetch cannot
-- honour).
SELECT 1 AS bad WHERE EXISTS (SELECT provenance() FROM isub WHERE id = 2);
SELECT (SELECT provenance() FROM isub GROUP BY v) AS bad;

SELECT remove_provenance('isub');
DROP TABLE isub;
