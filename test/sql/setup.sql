\set ECHO none
\pset format unaligned
CREATE EXTENSION provsql CASCADE;

DROP EXTENSION provsql;

CREATE EXTENSION provsql;

CREATE SCHEMA provsql_test;

-- Establish the database-level search_path for the whole regression run: set it
-- to the test schema, then let provsql.setup_search_path() append provsql (this
-- also exercises that function). Every subsequent test runs in a fresh session
-- that inherits "provsql_test, provsql", so individual tests no longer set
-- search_path themselves.
DO $$ BEGIN
  EXECUTE format('ALTER DATABASE %I SET search_path = provsql_test', current_database());
END $$;
SELECT provsql.setup_search_path();
