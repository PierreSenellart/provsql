\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Probes the safe-query read-once ('independent') rewrite in isolation;
-- pin the joint-width debug GUC off so its per-answer d-D does not replace
-- the read-once provenance the 'independent' method checks.
SET provsql.joint_width = off;

-- Deterministic-relation transparency (Gatterbauer & Suciu 2015,
-- dissociation framework).  A relation that is not provenance-tracked
-- (no @c provsql column and no metadata entry) carries
-- probability-1 tuples: under dissociation, factoring such a relation
-- out of the cross product does not change the query's probability.
-- The detector exploits this by marking every union-find class as
-- FD-determined inside a deterministic atom, so the atom drops from
-- each class's FD-aware atom-set.  The pairwise hierarchicality
-- check then accepts star-schema queries that the raw atom-count
-- check would refuse.
--
-- The canonical motivating shape is a fact table joined to two
-- deterministic dimensions, each filtered by an atom-local predicate:
--
--   q :- Sales(p, c), Products(p), Customers(c).
--
-- Without the transparency pass: atoms(p) = {Sales, Products},
-- atoms(c) = {Sales, Customers}.  Neither nested nor disjoint --
-- non-hierarchical, the detector falls through to the generic
-- provenance_plus(array_agg) shape.  With the pass: Products and
-- Customers transparent -> atoms_fd(p) = atoms_fd(c) = {Sales}.
-- Both classes touch only Sales; the rewrite emits Sales as the lone
-- probabilistic atom (one row per (product_id, customer_id) binding)
-- with the two dimensions as DISTINCT-wrapped filters in the outer
-- cross product.

CREATE TABLE sd_sales (product_id int, customer_id int);
-- Dimensions: no @c add_provenance call, so deterministic atoms.
CREATE TABLE sd_products (product_id int PRIMARY KEY, category text);
CREATE TABLE sd_customers (customer_id int PRIMARY KEY, region text);

INSERT INTO sd_sales VALUES (1, 10), (1, 10), (2, 11), (3, 12), (1, 11);
INSERT INTO sd_products VALUES (1, 'Electronics'), (2, 'Books'),
                               (3, 'Electronics');
INSERT INTO sd_customers VALUES (10, 'EU'), (11, 'US'), (12, 'EU');

SELECT add_provenance('sd_sales');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM sd_sales; END $$;

-- ---------------------------------------------------------------------
-- (1) Baseline: probability via the default evaluator on the
--     unrewritten circuit.
-- ---------------------------------------------------------------------
SET provsql.provenance = 'semiring';
CREATE TEMP TABLE sd_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sd_sales s, sd_products p, sd_customers c
           WHERE s.product_id = p.product_id
             AND s.customer_id = c.customer_id
             AND p.category = 'Electronics'
             AND c.region = 'EU') q
   GROUP BY q.x;
SELECT remove_provenance('sd_baseline');
SELECT x, ROUND(p::numeric, 6) AS prob_baseline FROM sd_baseline;

-- ---------------------------------------------------------------------
-- (2) Transparency path: 'independent' on the rewritten read-once
--     circuit must match the baseline within numerical noise.
-- ---------------------------------------------------------------------
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sd_rewritten AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sd_sales s, sd_products p, sd_customers c
           WHERE s.product_id = p.product_id
             AND s.customer_id = c.customer_id
             AND p.category = 'Electronics'
             AND c.region = 'EU') q
   GROUP BY q.x;
SELECT remove_provenance('sd_rewritten');
SELECT x, ROUND(p::numeric, 6) AS prob_rewritten FROM sd_rewritten;

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM sd_baseline b JOIN sd_rewritten r ON b.x = r.x;

-- ---------------------------------------------------------------------
-- (3) Dimension with duplicate join keys but no PRIMARY KEY: the
--     DISTINCT in the dimension's wrap collapses duplicates, so the
--     read-once invariant survives.  The probability must still match.
-- ---------------------------------------------------------------------
CREATE TABLE sd_products_dup (product_id int, category text);
INSERT INTO sd_products_dup VALUES (1, 'Electronics'), (1, 'Electronics'),
                                   (2, 'Books'), (3, 'Electronics');

SET provsql.provenance = 'semiring';
CREATE TEMP TABLE sd_dup_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sd_sales s, sd_products_dup p, sd_customers c
           WHERE s.product_id = p.product_id
             AND s.customer_id = c.customer_id
             AND p.category = 'Electronics'
             AND c.region = 'EU') q
   GROUP BY q.x;
SELECT remove_provenance('sd_dup_baseline');

SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sd_dup_rewritten AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (SELECT DISTINCT 1 AS x
            FROM sd_sales s, sd_products_dup p, sd_customers c
           WHERE s.product_id = p.product_id
             AND s.customer_id = c.customer_id
             AND p.category = 'Electronics'
             AND c.region = 'EU') q
   GROUP BY q.x;
SELECT remove_provenance('sd_dup_rewritten');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_dup_baseline_vs_rewritten
  FROM sd_dup_baseline b JOIN sd_dup_rewritten r ON b.x = r.x;

-- ---------------------------------------------------------------------
-- (4) The OPAQUE-tag case from the candidate gate is unchanged by
--     the transparency pass: a relation whose @c provsql column was
--     tagged OPAQUE by the @c provenance_guard trigger (e.g. after
--     a manual @c UPDATE touching @c provsql) is refused at the
--     shape gate, never reaching the transparency detection.
--     Cross-check that the gate still fires.  (Simple-view dimensions are out of scope here: the
--     rewriter inlines simple views before the planner hook runs,
--     so they reach the detector as their underlying base RTE; the
--     @c relkind @c == @c 'r' guard fires only for non-inlinable
--     view shapes that the existing candidate gate already rejects
--     via its @c RTE_RELATION @c -only check.)
-- ---------------------------------------------------------------------
CREATE TABLE sd_opaque (product_id int, category text);
SELECT add_provenance('sd_opaque');
SELECT set_table_info('sd_opaque'::regclass::oid, 'opaque');
INSERT INTO sd_opaque VALUES (1, 'Electronics');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM sd_opaque; END $$;

SET provsql.provenance = 'boolean';
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM (SELECT DISTINCT 1 AS x
              FROM sd_sales s, sd_opaque p, sd_customers c
             WHERE s.product_id = p.product_id
               AND s.customer_id = c.customer_id
               AND p.category = 'Electronics'
               AND c.region = 'EU') q
     GROUP BY q.x;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject the star query '
                    'when one dimension is OPAQUE-tagged';
  END IF;
END $$;

DROP TABLE sd_sales, sd_products, sd_customers, sd_products_dup,
           sd_opaque CASCADE;
