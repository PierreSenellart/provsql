\set ECHO none
\pset format unaligned
SET client_min_messages = warning;

-- An external compiler picked by the cost-based chooser is stopped once it
-- has run for the time the next-best method was expected to take (times a
-- margin), and that method is then tried.  Without the limit, a compiler
-- that does not return holds the statement until statement_timeout.
--
-- The compilers are replaced by a fake one that records that it started,
-- then sleeps for a minute.  The circuit is the disjunction, over the
-- neighbours u, v of a grid of 5 by 40 inputs, of u AND NOT v: neither
-- read-once nor a DNF over inputs, with too many inputs to enumerate, so
-- the chooser has tree decomposition and compilation.  Tree decomposition
-- is priced on a lower bound of the treewidth; once it finds the real
-- width it is priced above compilation, which is therefore launched, and
-- must be stopped for tree decomposition to answer.
\! rm -rf /tmp/provsql_ctl && mkdir -p /tmp/provsql_ctl && for t in d4 d4v2 c2d minic2d dsharp panini-obdd panini-obdd-and panini-decdnnf; do printf '#!/bin/sh\ncase "$1" in --provsql-probe) exit 0 ;; esac\ntouch /tmp/provsql_ctl/started\nexec sleep 60\n' > /tmp/provsql_ctl/$t; done && chmod 777 /tmp/provsql_ctl && chmod 755 /tmp/provsql_ctl/*
\set CANEXEC `/tmp/provsql_ctl/d4 --provsql-probe >/dev/null 2>&1 && echo true || echo false`
\if :CANEXEC

CREATE FUNCTION ctl_circuit() RETURNS uuid LANGUAGE plpgsql AS $$
DECLARE ands uuid[] := '{}'; u uuid; v uuid; x uuid;
BEGIN
  FOR i IN 1..40 LOOP FOR r IN 1..5 LOOP
    u := md5('tl:c5_40:'||i||':'||r)::uuid;
    PERFORM create_gate(u, 'input'); PERFORM set_prob(u, 0.01);
    PERFORM create_gate(md5('tl:n:c5_40:'||i||':'||r)::uuid, 'monus',
                        ARRAY[gate_one(), u]);
  END LOOP; END LOOP;
  FOR i IN 1..40 LOOP FOR r IN 1..5 LOOP
    u := md5('tl:c5_40:'||i||':'||r)::uuid;
    IF r < 5 THEN
      v := md5('tl:n:c5_40:'||i||':'||(r+1))::uuid;
      x := md5('tl:a:c5_40:'||i||':'||r)::uuid;
      PERFORM create_gate(x, 'times', ARRAY[u,v]); ands := ands || x;
    END IF;
    IF i < 40 THEN
      v := md5('tl:n:c5_40:'||(i+1)||':'||r)::uuid;
      x := md5('tl:b:c5_40:'||i||':'||r)::uuid;
      PERFORM create_gate(x, 'times', ARRAY[u,v]); ands := ands || x;
    END IF;
  END LOOP; END LOOP;
  u := md5('tl:root:c5_40')::uuid;
  PERFORM create_gate(u, 'plus', ands);
  RETURN u;
END $$;
CREATE TABLE ctl AS SELECT ctl_circuit() AS token;

-- The reference, by tree decomposition called by name.
CREATE TABLE ctl_reference AS
  SELECT probability_evaluate(token, 'tree-decomposition') AS p FROM ctl;
SELECT round(p::numeric, 6) AS reference FROM ctl_reference;

SET provsql.tool_search_path = '/tmp/provsql_ctl';
SET statement_timeout = '30s';
SET provsql.last_eval_method = '';
SELECT abs(probability_evaluate(token) - p) < 1e-9 AS default_agrees,
       current_setting('provsql.last_eval_method') AS method
FROM ctl, ctl_reference;
RESET statement_timeout;
RESET provsql.tool_search_path;
\echo 'compiler started, then stopped:'
\! test -f /tmp/provsql_ctl/started && echo t || echo f

DROP TABLE ctl_reference;
DROP TABLE ctl;
DROP FUNCTION ctl_circuit();
\else
\echo 'SKIPPING compilation_time_limit: fake compiler not executable here (noexec /tmp?)'
\endif

\! rm -rf /tmp/provsql_ctl
