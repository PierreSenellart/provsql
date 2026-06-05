-- Comparative benchmark for the probability chooser: a spread of circuit shapes,
-- each timed under every applicable method BY NAME (the actual cost in ms) next
-- to what the cost chooser auto-selects for exact / additive(delta=0.05) /
-- additive(delta=0, deterministic).  A well-calibrated chooser auto-picks (close
-- to) the cheapest by-name method.  Run against a provsql database:
--   psql -d <db> -f test/bench/dtree_bench.sql
-- Runs in ~30-60s.  The adaptive samplers are capped at max_samples in the
-- by-name timing (their true cost on rare/high-clause circuits is unbounded);
-- the auto-chooser columns are the point -- it should pick a fast method every
-- time and never one of the catastrophic by-name cells.
--
-- Reference auto-chooser picks (this machine, d4/c2d/dsharp available).  Every
-- request lands on a fast method; together they exercise the WHOLE portfolio.
-- Refreshed after (a) the d-tree was generalised off monotone DNF to arbitrary
-- circuits -- so it is now a candidate on the CNF / cycle shapes and wins many
-- approximate cells it previously could not enter -- and (b) speculative
-- execution: the chooser budgets the d-tree (and tree-decomposition) at the
-- next-best method's cost and escalates on overrun.
--
--  circuit       exact           rel eps=.1      rel eps=.3      additive      det delta=0
--  readonce      independent     independent     independent     independent   independent
--  triangle      possible-worlds possible-worlds possible-worlds possible-worlds possible-worlds
--  cycle_common  tree-decomp     d-tree          d-tree          monte-carlo   d-tree
--  cycle_rare    tree-decomp     d-tree          karp-luby       monte-carlo   d-tree
--  clique8       possible-worlds possible-worlds possible-worlds possible-worlds possible-worlds
--  clique14      d-tree          d-tree          d-tree          monte-carlo   d-tree
--  big_cycle     tree-decomp     d-tree          d-tree          monte-carlo   d-tree
--  big_rare      tree-decomp     compilation:d4  tree-decomp     monte-carlo   d-tree   [*]
--  cnf           tree-decomp     d-tree          d-tree          monte-carlo   d-tree
--  ladder        tree-decomp     tree-decomp     d-tree          tree-decomp   tree-decomp
--  nested/monus/cnf12 independent independent    independent     independent   independent
--  cliqueCNF14   possible-worlds d-tree          stopping-rule   monte-carlo   d-tree
--  cliqueCNF18   compilation:d4  d-tree          stopping-rule   monte-carlo   d-tree
--  sieve_fav     sieve           sieve           karp-luby       sieve         sieve
--  kl_fav        d-tree          d-tree          karp-luby       monte-carlo   d-tree
--  invfree       inversion-free  inversion-free  inversion-free  inversion-free inversion-free
--
-- [*] big_rare rel eps=.1 escalates d-tree -> compilation because the d-tree's
-- *approximate* DNF path does not memoise (only the exact path writes the memo,
-- since an early-stopped interval is width-dependent), so on this 160-cycle the
-- approximate run does tens of thousands of subproblems where the exact run does
-- a few hundred; the speculative budget rightly bails it.  This is deterministic
-- (the whole auto-chooser table is bit-identical across runs): the degeneracy
-- peel and the min-fill heap break ties by node id.  Memoising the exact
-- sub-results encountered during an approximate recursion would let the d-tree
-- handle this cell directly -- a noted follow-up, not a budget bug.
--
-- Reachability of every method: independent / possible-worlds / tree-decomp /
-- d-tree / monte-carlo are common; sieve (sieve_fav), karp-luby + stopping-rule
-- (loose eps), compilation (cliqueCNF18 exact), inversion-free (invfree).  The
-- stopping-rule picks at loose eps are the documented 1/p corner (it is cheaper
-- in the model but slow at runtime); at tight eps the chooser avoids it.
\timing off
\set ECHO none
SET search_path TO provsql_test, provsql;
SET provsql.boolean_provenance = off;

DROP TABLE IF EXISTS bench_v CASCADE;
CREATE TABLE bench_v(id int);
INSERT INTO bench_v SELECT generate_series(1, 600);
SELECT add_provenance('bench_v');
-- Two probability regimes: low per-variable prob -> rare results (exercises the
-- stopping rule's 1/p), moderate -> common results.
DO $$ BEGIN PERFORM set_prob(provsql, CASE WHEN id <= 300 THEN 0.3 ELSE 0.02 END)
            FROM bench_v; END $$;

DROP TABLE IF EXISTS bench_tok CASCADE;
CREATE TABLE bench_tok(seq serial, name text, descr text, tok uuid);

SET provsql.active = off;
DO $$
DECLARE c uuid[]; r uuid[]; cl uuid[]; ors uuid[]; cv uuid[]; acc uuid;
        i int; j int; k int; n int;
BEGIN
  SELECT array_agg(provsql::uuid ORDER BY id) INTO c FROM bench_v WHERE id <= 300; -- p=0.3
  SELECT array_agg(provsql::uuid ORDER BY id) INTO r FROM bench_v WHERE id  > 300; -- p=0.02

  -- 1. read-once DNF (10 disjoint pairs)
  cl := ARRAY[]::uuid[];
  FOR i IN 1..10 LOOP cl := cl || provenance_times(c[2*i-1], c[2*i]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('readonce','10 disjoint pairs', provenance_plus(cl));

  -- 2. tiny shared DNF (triangle)
  INSERT INTO bench_tok(name,descr,tok) VALUES ('triangle','(x1x2)|(x1x3)|(x2x3)',
    provenance_plus(ARRAY[provenance_times(c[1],c[2]),provenance_times(c[1],c[3]),provenance_times(c[2],c[3])]));

  -- 3. low-treewidth cycle, common result (p=0.3)
  cl := ARRAY[]::uuid[];
  FOR i IN 1..40 LOOP cl := cl || provenance_times(c[i], c[1+(i%40)]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('cycle_common','40-cycle, p=0.3', provenance_plus(cl));

  -- 4. low-treewidth cycle, rare result (p=0.02)
  cl := ARRAY[]::uuid[];
  FOR i IN 1..40 LOOP cl := cl || provenance_times(r[i], r[1+(i%40)]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('cycle_rare','40-cycle, p=0.02', provenance_plus(cl));

  -- 5. clique on 8 vars (treewidth 7, collapses fast)
  cl := ARRAY[]::uuid[];
  FOR i IN 1..8 LOOP FOR j IN i+1..8 LOOP cl := cl || provenance_times(c[i],c[j]); END LOOP; END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('clique8','all pairs of 8 (w=7)', provenance_plus(cl));

  -- 6. clique on 14 vars (treewidth 13 > tree-decomposition cap)
  cl := ARRAY[]::uuid[];
  FOR i IN 1..14 LOOP FOR j IN i+1..14 LOOP cl := cl || provenance_times(c[i],c[j]); END LOOP; END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('clique14','all pairs of 14 (w=13)', provenance_plus(cl));

  -- 7. large cycle (160 clauses), common
  cl := ARRAY[]::uuid[];
  FOR i IN 1..160 LOOP cl := cl || provenance_times(c[i], c[1+(i%160)]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('big_cycle','160-cycle, p=0.3', provenance_plus(cl));

  -- 8. large cycle, rare (exercises stopping-rule 1/p)
  cl := ARRAY[]::uuid[];
  FOR i IN 1..160 LOOP cl := cl || provenance_times(r[i], r[1+(i%160)]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('big_rare','160-cycle, p=0.02', provenance_plus(cl));

  -- 9. non-DNF: AND-of-ORs over 20 vars (CNF-shaped, d-tree/karp-luby N/A)
  ors := ARRAY[]::uuid[];
  FOR i IN 1..20 LOOP ors := ors || provenance_plus(ARRAY[c[i], c[1+(i%20)]]); END LOOP;
  cl := ors[1:1];
  DECLARE acc uuid := ors[1]; BEGIN
    FOR i IN 2..20 LOOP acc := provenance_times(acc, ors[i]); END LOOP;
    INSERT INTO bench_tok(name,descr,tok) VALUES ('cnf','AND of 20 ORs (non-DNF)', acc);
  END;

  -- 10. overlapping "ladder" DNF: (x_i x_{i+1}) over 60 vars, denser sharing
  cl := ARRAY[]::uuid[];
  FOR i IN 1..59 LOOP cl := cl || provenance_times(c[i], c[i+1]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('ladder','path of 59 pairwise, p=0.3', provenance_plus(cl));

  -- NON-DNF shapes: d-tree / sieve / karp-luby decline (DnfShape), so these
  -- exercise tree-decomposition / compilation / possible-worlds / monte-carlo /
  -- stopping-rule.
  -- 11. three-level alternation OR( AND( OR(leaves) ) ) -- not OR-of-ANDs-of-leaves
  ors := ARRAY[]::uuid[];
  FOR i IN 1..12 LOOP ors := ors || provenance_plus(ARRAY[c[i], c[i+12]]); END LOOP;
  cl := ARRAY[]::uuid[];
  FOR i IN 1..6 LOOP cl := cl || provenance_times(ors[2*i-1], ors[2*i]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('nested','OR of 6 (AND of 2 ORs) -- non-DNF', provenance_plus(cl));

  -- 12. NON-MONOTONE: (x1..x10 OR) minus (x11 x12) -- EXCEPT-style, carries negation
  INSERT INTO bench_tok(name,descr,tok) VALUES ('monus','(OR of 10) minus (x11 x12) -- non-monotone',
    provenance_monus(provenance_plus(c[1:10]), provenance_times(c[11], c[12])));

  -- 13. CNF: AND of 12 ORs over 24 vars (a conjunction of disjunctions)
  ors := ARRAY[]::uuid[];
  FOR i IN 1..12 LOOP ors := ors || provenance_plus(ARRAY[c[2*i-1], c[2*i]]); END LOOP;
  acc := ors[1];
  FOR i IN 2..12 LOOP acc := provenance_times(acc, ors[i]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('cnf12','AND of 12 ORs (non-DNF)', acc);

  -- HIGH-TREEWIDTH NON-DNF: clique CNF -- AND over ALL pairwise (x_i OR x_j).
  -- The primal graph is a complete graph, so treewidth = n-1 (> td's cap), and
  -- it is non-DNF, so d-tree / sieve / karp-luby decline too: the hard corner
  -- where only possible-worlds (2^N) / compilation / the samplers remain.
  -- 14. 14 vars (tw=13; possible-worlds 2^14 still cheap)
  ors := ARRAY[]::uuid[];
  FOR i IN 1..14 LOOP FOR j IN i+1..14 LOOP ors := ors || provenance_plus(ARRAY[c[i],c[j]]); END LOOP; END LOOP;
  acc := ors[1];
  FOR i IN 2..array_length(ors,1) LOOP acc := provenance_times(acc, ors[i]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('cliqueCNF14','AND of all pairwise ORs of 14 (high-tw non-DNF)', acc);
  -- 15. 18 vars (tw=17; possible-worlds 2^18 dearer than the compiler -> compilation)
  ors := ARRAY[]::uuid[];
  FOR i IN 1..18 LOOP FOR j IN i+1..18 LOOP ors := ors || provenance_plus(ARRAY[c[i],c[j]]); END LOOP; END LOOP;
  acc := ors[1];
  FOR i IN 2..array_length(ors,1) LOOP acc := provenance_times(acc, ors[i]); END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('cliqueCNF18','AND of all pairwise ORs of 18 (high-tw non-DNF)', acc);

  -- 16. sieve's niche: FEW clauses (m=6) over MANY entangled vars -> 2^m beats
  -- both possible-worlds (2^N) and tree-decomposition, and undercuts d-tree's S*m.
  cl := ARRAY[]::uuid[];
  FOR i IN 1..6 LOOP
    cv := ARRAY[]::uuid[];
    FOR k IN 0..6 LOOP cv := cv || c[1 + ((i+k) % 12)]; END LOOP;
    cl := cl || provenance_times(VARIADIC cv);
  END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('sieve_fav','6 clauses x 7 vars over 12 (few-clause DNF)', provenance_plus(cl));

  -- 17. karp-luby's niche: an entangled DNF (m=12) where exact is dear; under a
  -- LOOSE relative tolerance its m*ln(1/delta)/eps^2 finally undercuts the d-tree.
  cl := ARRAY[]::uuid[];
  FOR i IN 1..12 LOOP
    cv := ARRAY[]::uuid[];
    FOR k IN 0..5 LOOP cv := cv || c[1 + ((2*i+3*k) % 16)]; END LOOP;
    cl := cl || provenance_times(VARIADIC cv);
  END LOOP;
  INSERT INTO bench_tok(name,descr,tok) VALUES ('kl_fav','12 clauses x 6 vars over 16 (entangled DNF)', provenance_plus(cl));
END $$;
RESET provsql.active;

-- 18. inversion-free's niche: a REAL planner-built lineage (the certificate is
-- attached by the query rewriter, so this cannot be hand-built).  The self-join
-- S(x,y),A(x,y),S(x,z),B(x,z) with many (y,z) derivations for x=1 yields a
-- non-read-once, N=42 inversion-free circuit: independent throws and possible-
-- worlds is 2^42, so the chooser takes the inversion-free structured d-DNNF.
SET provsql.boolean_provenance = on;
DROP TABLE IF EXISTS ifr_s, ifr_a, ifr_b CASCADE;
CREATE TABLE ifr_s(x int, c2 int); INSERT INTO ifr_s SELECT 1, g FROM generate_series(1,14) g; SELECT add_provenance('ifr_s');
CREATE TABLE ifr_a(x int, c2 int); INSERT INTO ifr_a SELECT 1, g FROM generate_series(1,14) g; SELECT add_provenance('ifr_a');
CREATE TABLE ifr_b(x int, c2 int); INSERT INTO ifr_b SELECT 1, g FROM generate_series(1,14) g; SELECT add_provenance('ifr_b');
DO $$ BEGIN PERFORM set_prob(provsql,0.3) FROM ifr_s; PERFORM set_prob(provsql,0.3) FROM ifr_a; PERFORM set_prob(provsql,0.3) FROM ifr_b; END $$;
CREATE TEMP TABLE ifr_block AS
  SELECT s1.x AS x, provenance() AS p FROM ifr_s s1, ifr_a a, ifr_s s2, ifr_b b
   WHERE s1.x=a.x AND s1.c2=a.c2 AND s1.x=s2.x AND s2.x=b.x AND s2.c2=b.c2 GROUP BY s1.x;
SELECT remove_provenance('ifr_block');
INSERT INTO bench_tok(name,descr,tok)
  SELECT 'invfree','self-join S,A,S,B, N=42 (inversion-free, real query)', p FROM ifr_block;
SET provsql.boolean_provenance = off;

-- Timing harness: ms per (circuit, method); inapplicable methods recorded as NULL.
DROP TABLE IF EXISTS bench_res CASCADE;
CREATE TABLE bench_res(seq int, circuit text, descr text, method text, ms numeric, resolved text);

DO $$
DECLARE
  rec record; t0 timestamptz; v double precision; meth text; lab text;
  methods text[] := ARRAY['independent','sieve','tree-decomposition','d-tree',
                          'karp-luby','stopping-rule','monte-carlo'];
  args text;
BEGIN
  FOR rec IN SELECT * FROM bench_tok ORDER BY seq LOOP
    -- by-name methods (actual cost)
    FOREACH meth IN ARRAY methods LOOP
      -- Cap the adaptive samplers' by-name budget so the bench stays runnable:
      -- their true 1/p (stopping-rule) / m (karp-luby) cost is unbounded on rare
      -- or high-clause circuits (it is exactly why the chooser avoids them), but
      -- here we only need to show they are far dearer than the chosen method.
      args := CASE meth
                WHEN 'karp-luby' THEN 'epsilon=0.1,delta=0.05,max_samples=100000'
                WHEN 'stopping-rule' THEN 'epsilon=0.1,delta=0.05,max_samples=100000'
                WHEN 'monte-carlo' THEN 'epsilon=0.1,delta=0.05'
                WHEN 'd-tree' THEN 'epsilon=0.1' ELSE NULL END;
      BEGIN
        t0 := clock_timestamp();
        v := probability_evaluate(rec.tok, meth, args);
        INSERT INTO bench_res VALUES (rec.seq, rec.name, rec.descr, meth,
          round((extract(epoch FROM clock_timestamp()-t0)*1000)::numeric,2), NULL);
      EXCEPTION WHEN OTHERS THEN
        INSERT INTO bench_res VALUES (rec.seq, rec.name, rec.descr, meth, NULL, 'N/A');
      END;
    END LOOP;
    -- auto-chooser: exact, relative(delta>0), additive(delta>0), deterministic(delta=0)
    FOR meth, args, lab IN VALUES
        ('exact',NULL,'AUTO exact'),
        ('relative','epsilon=0.1,delta=0.05','AUTO rel d=.05'),
        ('relative','epsilon=0.3,delta=0.2','AUTO rel loose'),
        ('additive','epsilon=0.1,delta=0.05','AUTO add d=.05'),
        ('additive','epsilon=0.1,delta=0','AUTO det d=0') LOOP
      BEGIN
        PERFORM set_config('provsql.last_eval_method','',false);
        t0 := clock_timestamp();
        v := probability_evaluate(rec.tok, meth, args);
        INSERT INTO bench_res VALUES (rec.seq, rec.name, rec.descr, lab,
          round((extract(epoch FROM clock_timestamp()-t0)*1000)::numeric,2),
          current_setting('provsql.last_eval_method'));
      EXCEPTION WHEN OTHERS THEN
        INSERT INTO bench_res VALUES (rec.seq, rec.name, rec.descr, lab, NULL, 'ERROR');
      END;
    END LOOP;
  END LOOP;
END $$;

\set ECHO all
\pset format aligned
-- By-name actual cost per method (ms), pivoted per circuit.  (possible-worlds and
-- inversion-free are omitted here -- 2^N enumeration is slow to time on every
-- circuit -- but they appear in the auto-chooser columns where they are chosen.)
SELECT circuit,
       max(ms) FILTER (WHERE method='independent')        AS indep,
       max(ms) FILTER (WHERE method='sieve')              AS sieve,
       max(ms) FILTER (WHERE method='tree-decomposition') AS td,
       max(ms) FILTER (WHERE method='d-tree')             AS dtree,
       max(ms) FILTER (WHERE method='karp-luby')          AS kl,
       max(ms) FILTER (WHERE method='stopping-rule')      AS sr,
       max(ms) FILTER (WHERE method='monte-carlo')        AS mc
FROM bench_res GROUP BY seq, circuit ORDER BY seq;

-- What the chooser auto-selected (resolved method) per request.
SELECT circuit,
       max(resolved) FILTER (WHERE method='AUTO exact')      AS exact,
       max(resolved) FILTER (WHERE method='AUTO rel d=.05')  AS rel_tight,
       max(resolved) FILTER (WHERE method='AUTO rel loose')  AS rel_loose,
       max(resolved) FILTER (WHERE method='AUTO add d=.05')  AS additive,
       max(resolved) FILTER (WHERE method='AUTO det d=0')    AS det_d0
FROM bench_res GROUP BY seq, circuit ORDER BY seq;

DROP TABLE bench_res; DROP TABLE bench_tok;
SELECT remove_provenance('bench_v'); DROP TABLE bench_v;
RESET provsql.boolean_provenance;
