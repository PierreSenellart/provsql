\set ECHO none
\pset format unaligned

-- Non-existent tool: deterministic FALSE regardless of host setup.
SELECT tool_available('definitely-not-a-real-tool-xyz123');

-- Empty / NULL behaviour.
SELECT tool_available('');
SELECT tool_available(NULL) IS NULL AS null_in_null_out;

-- Path-form with leading slash: tested via access(X_OK). A non-existent
-- absolute path returns FALSE; /bin/sh is universally executable.
SELECT tool_available('/this/path/does/not/exist');
SELECT tool_available('/bin/sh');
