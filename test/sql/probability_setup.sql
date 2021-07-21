\set ECHO none
SET search_path TO provsql_test,provsql;

/* We use PERFORM in anonymous code block instead of SELECT to avoid the
 * display of provenance tokens */
DO $$ BEGIN
  PERFORM set_prob(provenance(), id*1./10) FROM personnel;
END $$;  

CREATE TABLE probs AS
SELECT get_prob(provenance()) AS value FROM personnel;

SELECT remove_provenance('probs');

SELECT * FROM probs ORDER BY value;
