-- Function-call coverage: provsql functions the test suite never invoked, from
-- PostgreSQL's own per-function call counts (pg_stat_user_functions). Run
-- against the regression database (contrib_regression) with track_functions=all
-- enabled server-side; `make coverage` sets both up, and excludes the
-- extension_upgrade test (its DROP EXTENSION would purge these counters).
--
-- pg_stat_user_functions only counts functions entered through the executor's
-- function-call path. Several categories of provsql function are never counted
-- even when heavily exercised, so they are filtered out below to leave a list
-- of *genuine* coverage gaps:
--
--   (a) LANGUAGE sql functions  -- inlined into the calling query;
--   (b) type input/output/send/receive functions  -- called by the type cache;
--   (c) cast functions  -- called by coercion;
--   (d) aggregate support functions (transition/final/combine/...)  -- called
--       by the aggregate executor;
--   (e) ProvSQL planner-rewritten placeholders and markers  -- declared so the
--       parser accepts the syntax (provenance(), the comparison operators, the
--       "| (predicate)" conditioning forms), but the planner hook rewrites them
--       away, so their bodies never execute by design. (a)-(d) are detected
--       from the catalogs; (e) has no catalog signal and is listed explicitly.
--       Keep it in sync with the source: grep for "Never executes" /
--       "Never actually called" / "Placeholder".
--
-- A few LANGUAGE C rows can still slip through (e.g. functions reached only via
-- DirectFunctionCall / SPI from the C core); cross-check a C entry against the
-- gcovr line report before trusting it. plpgsql rows are reliable.
SELECT n.nspname || '.' || p.proname AS never_called,
       pg_get_function_identity_arguments(p.oid) AS args,
       l.lanname AS lang
FROM pg_proc p
JOIN pg_namespace n ON n.oid = p.pronamespace
JOIN pg_language  l ON l.oid = p.prolang
LEFT JOIN pg_stat_user_functions s ON s.funcid = p.oid
WHERE n.nspname = 'provsql'
  AND l.lanname IN ('plpgsql', 'c')                                 -- (a)
  AND COALESCE(s.calls, 0) = 0
  AND p.oid NOT IN (                                                 -- (b)
        SELECT unnest(ARRAY[typinput, typoutput, typreceive, typsend,
                            typmodin, typmodout, typanalyze])
        FROM pg_type)
  AND p.oid NOT IN (SELECT castfunc FROM pg_cast WHERE castfunc <> 0) -- (c)
  AND p.oid NOT IN (                                                 -- (d)
        SELECT unnest(ARRAY[aggtransfn, aggfinalfn, aggcombinefn, aggserialfn,
                            aggdeserialfn, aggmtransfn, aggminvtransfn,
                            aggmfinalfn])
        FROM pg_aggregate)
  AND p.proname NOT IN (                                             -- (e)
        'provenance',
        'agg_token_comp_agg_token', 'agg_token_comp_numeric',
        'agg_token_comp_text', 'numeric_comp_agg_token', 'text_comp_agg_token',
        'cond_predicate', 'given', 'agg_token_cond_predicate',
        'random_variable_cond_predicate', 'random_variable_cmp_placeholder')
ORDER BY l.lanname, 1;
