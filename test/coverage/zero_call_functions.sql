-- Function-call coverage: provsql functions in the regression database that
-- the test suite never invoked, from PostgreSQL's own per-function call counts
-- (pg_stat_user_functions). Run against the database left
-- behind by `make installcheck` (contrib_regression) with track_functions=all
-- enabled server-side; `make coverage` sets both up.
--
-- Caveat: a plain LANGUAGE SQL function the planner inlines is folded into the
-- calling query and never counted as a call, so it can appear here even when
-- exercised. The signal is reliable for LANGUAGE C and LANGUAGE plpgsql; treat
-- inlinable SQL functions as "unknown" rather than "untested".
SELECT n.nspname || '.' || p.proname AS never_called,
       pg_get_function_identity_arguments(p.oid) AS args,
       l.lanname AS lang
FROM pg_proc p
JOIN pg_namespace n ON n.oid = p.pronamespace
JOIN pg_language  l ON l.oid = p.prolang
LEFT JOIN pg_stat_user_functions s ON s.funcid = p.oid
WHERE n.nspname = 'provsql'
  AND COALESCE(s.calls, 0) = 0
ORDER BY l.lanname, 1;
