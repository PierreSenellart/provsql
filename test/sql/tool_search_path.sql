\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- 1. The GUC is registered and defaults to the empty string.
SHOW provsql.tool_search_path;

-- 2. Stage a stub `graph-easy` in a private directory.  The stub ignores
--    its input file and writes a fixed marker to whatever path it received
--    via --output=, then exits 0.  Permissions allow the postgres backend
--    OS user (which may differ from the test user) to read and execute.
\! rm -rf /tmp/provsql_tsp_test && mkdir -p /tmp/provsql_tsp_test
\! printf '#!/bin/sh\nfor a in "$@"; do case "$a" in --output=*) printf STUB-MARKER > "${a#--output=}";; esac; done\n' > /tmp/provsql_tsp_test/graph-easy
\! chmod 755 /tmp/provsql_tsp_test /tmp/provsql_tsp_test/graph-easy

-- 3. Point the GUC at the stub directory; SHOW must reflect it.
SET provsql.tool_search_path = '/tmp/provsql_tsp_test';
SHOW provsql.tool_search_path;

-- 4. Trigger the dot renderer.  With the stub directory prepended to PATH
--    inside the backend, view_circuit's call to `graph-easy` resolves to
--    the stub regardless of whether the real graph-easy is installed, so
--    the rendered output equals the marker the stub wrote.  This is what
--    proves the GUC value is actually applied around system() calls.
CREATE TABLE tsp_result AS
SELECT view_circuit(provenance(), 'd') AS rendered
FROM (
  SELECT p1.name FROM personnel p1, personnel p2
  WHERE p1.id=1 AND p2.id=1
) t;
SELECT remove_provenance('tsp_result');
SELECT rendered FROM tsp_result;
DROP TABLE tsp_result;

-- 5. Replace the stub with one that exits 42.  format_external_tool_status
--    should decode the WEXITSTATUS into "exited with status 42" rather
--    than the old generic "Error executing graph-easy".  The DO/EXCEPTION
--    wrapper turns the substring match into a stable, sqlstate-independent
--    NOTICE so the test does not depend on PG locale or error-prefix
--    formatting.
\! printf '#!/bin/sh\nexit 42\n' > /tmp/provsql_tsp_test/graph-easy
\! chmod 755 /tmp/provsql_tsp_test/graph-easy

DO $$
BEGIN
  PERFORM view_circuit(provenance(), 'd')
  FROM (
    SELECT p1.name FROM personnel p1, personnel p2
    WHERE p1.id=1 AND p2.id=1
  ) t;
  RAISE NOTICE 'unexpected: view_circuit succeeded';
EXCEPTION WHEN OTHERS THEN
  IF SQLERRM LIKE '%exited with status 42%' THEN
    RAISE NOTICE 'graph-easy exit 42 surfaces as structured error';
  ELSE
    RAISE NOTICE 'unexpected error: %', SQLERRM;
  END IF;
END $$;

-- 6. RESET restores the empty default.
RESET provsql.tool_search_path;
SHOW provsql.tool_search_path;

\! rm -rf /tmp/provsql_tsp_test
