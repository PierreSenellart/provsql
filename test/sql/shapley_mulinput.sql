\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Computing Shapley/Banzhaf values is currently ill-defined for circuits
-- containing mulinput gates: the d-DNNF builder silently drops MULINs
-- (see shapley.cpp), so the result is wrong.  shapley/banzhaf must
-- therefore reject such circuits explicitly until the semantics are
-- defined.

CREATE TABLE shap_mulin_t (id integer, grp text, p double precision);
INSERT INTO shap_mulin_t VALUES
  (1, 'A', 0.4), (2, 'A', 0.3),
  (3, 'B', 0.5);
SELECT repair_key('shap_mulin_t', 'grp');
DO $$ BEGIN
  PERFORM set_prob(provenance(), p) FROM shap_mulin_t;
END $$;

-- shapley(token, variable) on a mulinput-rooted circuit must error.
SELECT shapley(provsql, (get_children(provsql))[1])
FROM shap_mulin_t WHERE id = 1;

-- banzhaf delegates to shapley with banzhaf=true; same guard.
SELECT banzhaf(provsql, (get_children(provsql))[1])
FROM shap_mulin_t WHERE id = 1;

-- shapley_all_vars has its own makeDD call; same guard.
SELECT * FROM shapley_all_vars(
  (SELECT provsql FROM shap_mulin_t WHERE id = 1));

DROP TABLE shap_mulin_t;
