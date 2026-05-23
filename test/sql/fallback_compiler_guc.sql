\set ECHO none
\pset format unaligned

-- provsql.fallback_compiler is the compiler invoked by
-- BooleanCircuit::makeDD's final fallback (after interpretAsDD and
-- tree-decomposition both fail). The fallback path is hard to trigger
-- on a small fixture (it needs a non-independent circuit with
-- treewidth above the supported bound), so this smoke pins the GUC
-- contract itself: registration, default, SET round-trip via
-- pg_settings, and RESET.

-- The GUC is registered in pg_settings with the documented default.
SELECT name, setting, vartype, context
  FROM pg_settings WHERE name = 'provsql.fallback_compiler';

-- SET / SHOW round-trip.
SET provsql.fallback_compiler = 'c2d';
SHOW provsql.fallback_compiler;

-- RESET restores the default.
RESET provsql.fallback_compiler;
SHOW provsql.fallback_compiler;

-- Empty string is accepted (parser); makeDD falls back to "d4" via
-- its own safety net at evaluation time.
SET provsql.fallback_compiler = '';
SHOW provsql.fallback_compiler;
RESET provsql.fallback_compiler;
