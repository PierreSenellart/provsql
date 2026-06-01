\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Self-contained fixture: group A has the non-mutually-exclusive values
-- (x, y, x), group B the single value z.  The id-based provenance mapping
-- lets sr_formula print stable token names (1..4).
--
-- choose() is PICKFIRST, so its value depends on the order of the group's
-- occurrences.  We always write choose(val ORDER BY id) so "first" is the
-- smallest id (occurrences t1, t2, t3 in order), making the result
-- independent of the physical scan order and hence deterministic.
CREATE TABLE att(id int, grp text, val text);
INSERT INTO att VALUES (1,'A','x'),(2,'A','y'),(3,'A','x'),(4,'B','z');
SELECT add_provenance('att');
SELECT create_provenance_mapping('att_map','att','id');

-- The aggregate over a text column has the internal agg_token type.
CREATE TABLE att_typ AS SELECT grp, choose(val ORDER BY id) AS pick FROM att GROUP BY grp;
SELECT remove_provenance('att_typ');
SELECT DISTINCT pg_typeof(pick)::text FROM att_typ;
DROP TABLE att_typ;

-- HAVING choose(val ORDER BY id) = text.  choose is PICKFIRST, so the result holds in a
-- world iff the first surviving occurrence equals the constant.  Group A:
-- t1 present, or (t1,t2 absent and t3 present); group B: never (no x).
CREATE TABLE r_eq AS
SELECT grp, sr_formula(provenance(),'att_map') AS formula
FROM att GROUP BY grp HAVING choose(val ORDER BY id) = 'x';
SELECT remove_provenance('r_eq');
SELECT grp, formula FROM r_eq ORDER BY grp;
DROP TABLE r_eq;

-- HAVING choose(val ORDER BY id) <> text.  Group A: first surviving occurrence is the y
-- (t2 present, t1 absent); group B: always (z <> x).
CREATE TABLE r_ne AS
SELECT grp, sr_formula(provenance(),'att_map') AS formula
FROM att GROUP BY grp HAVING choose(val ORDER BY id) <> 'x';
SELECT remove_provenance('r_ne');
SELECT grp, formula FROM r_ne ORDER BY grp;
DROP TABLE r_ne;

-- Probability under independent contributors with Pr=1/2 each.
-- P(A) = Pr(t1) + Pr(not t1)Pr(not t2)Pr(t3) = 1/2 + 1/8 = 0.625; P(B) = 0.
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM att; END $$;
CREATE TABLE r_prob AS
SELECT grp, probability_evaluate(provenance()) AS prob
FROM att GROUP BY grp HAVING choose(val ORDER BY id) = 'x';
SELECT remove_provenance('r_prob');
SELECT grp, round(prob::numeric, 6) AS prob FROM r_prob ORDER BY grp;
DROP TABLE r_prob;

-- Only choose() is supported for an aggregate-vs-text comparison; any other
-- aggregate (here max) must raise a clear error rather than a wrong answer.
SELECT grp, sr_formula(provenance(),'att_map')
FROM att GROUP BY grp HAVING max(val) = 'x';

-- explode_table: expand an agg_token column back into one row per child,
-- recombining each child's value and provenance.  The table is rebuilt in
-- place (same schema), so its provsql column survives for readout.
CREATE TABLE att_grp AS SELECT grp, choose(val ORDER BY id) AS pick FROM att GROUP BY grp;
SELECT explode_table('att_grp', 'pick');
-- Materialise the per-row provenance formula, then strip the auto-added
-- provsql column (a non-deterministic UUID) before printing.
CREATE TABLE att_exp AS
SELECT grp, pick, sr_formula(provsql,'att_map') AS formula FROM att_grp;
SELECT remove_provenance('att_exp');
SELECT grp, pick, formula FROM att_exp ORDER BY grp, pick, formula;
DROP TABLE att_exp;
DROP TABLE att_grp;

-- JOIN on agg_token = text: the aggregated relation is exploded automatically
-- at plan time and the join runs as text = text with provenance propagated.
CREATE TABLE att_agg AS SELECT grp, choose(val ORDER BY id) AS pick FROM att GROUP BY grp;
CREATE TABLE att_lookup(who text);
INSERT INTO att_lookup VALUES ('x'),('z');
CREATE TABLE r_join AS
SELECT att_agg.grp, l.who, sr_formula(provenance(),'att_map') AS formula
FROM att_agg JOIN att_lookup l ON att_agg.pick = l.who;
SELECT remove_provenance('r_join');
SELECT grp, who, formula FROM r_join ORDER BY grp, who, formula;
DROP TABLE r_join;
DROP TABLE att_agg;
DROP TABLE att_lookup;

DROP TABLE att_map;
SELECT remove_provenance('att');
DROP TABLE att;
