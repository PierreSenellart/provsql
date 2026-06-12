-- Function-call coverage: provsql functions the test suite never invoked, from
-- PostgreSQL's own per-function call counts (pg_stat_user_functions). Run
-- against the regression database (contrib_regression) with track_functions=all
-- enabled server-side; `make coverage` sets both up, and excludes the
-- extension_upgrade test (its DROP EXTENSION would purge these counters).
--
-- LANGUAGE sql / internal functions are excluded: a SQL function simple enough
-- to be inlined into the calling query is never counted as a call, so it would
-- show up here even when heavily exercised. The list is therefore plpgsql + C.
--
-- Residual caveat for the LANGUAGE C rows: functions reached only through paths
-- that bypass the executor's function-call accounting -- type input/output and
-- cast functions (called by the type cache) and functions invoked internally
-- via DirectFunctionCall / SPI from the C core -- can still appear despite being
-- exercised. Cross-check a C entry against the gcovr line report before trusting
-- it. plpgsql rows are reliable.
SELECT n.nspname || '.' || p.proname AS never_called,
       pg_get_function_identity_arguments(p.oid) AS args,
       l.lanname AS lang
FROM pg_proc p
JOIN pg_namespace n ON n.oid = p.pronamespace
JOIN pg_language  l ON l.oid = p.prolang
LEFT JOIN pg_stat_user_functions s ON s.funcid = p.oid
WHERE n.nspname = 'provsql'
  AND l.lanname IN ('plpgsql', 'c')
  AND COALESCE(s.calls, 0) = 0
ORDER BY l.lanname, 1;
