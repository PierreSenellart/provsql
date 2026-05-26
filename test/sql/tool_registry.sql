\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- The registry is seeded per backend with every external tool ProvSQL can
-- invoke: the d-DNNF compilers (d4, d4v2, c2d, minic2d, dsharp), the three
-- Panini variants (all running the 'panini' binary), the weighted model
-- counters (sharpsat-td, ganak, weightmc, dpmc), and the graph-easy ASCII
-- renderer. dpmc has no single binary (it is the htb | dmc pipeline), so its
-- executable is empty. The capability triple operations / input_formats /
-- output_format uses the KCMCP registry names; `parser` is the CLI decode
-- tag (the compilers share the single tolerant `nnf` parser; d4v2 also
-- advertises circuit-bcs12 input). The host-dependent `available` column is
-- not selected.
SELECT name, executable, operations, input_formats, output_format, parser
  FROM tools ORDER BY name;

-- register_tool (named args): a new compiler added without recompiling, with
-- its KCMCP triple, parser and command template.
SELECT register_tool('acme-kc', executable => '/opt/acme/akc',
                     operations => ARRAY['compile'],
                     input_formats => ARRAY['dimacs-cnf'],
                     output_format => 'ddnnf-nnf', parser => 'nnf',
                     argtpl => '-dDNNF {in} -out={out}', preference => 120);
SELECT name, executable, operations, input_formats, output_format, parser,
       argtpl, preference
  FROM tools WHERE name='acme-kc';

-- Re-registering the same name replaces the record (no duplicate row).
SELECT register_tool('acme-kc', executable => '/usr/bin/akc',
                     operations => ARRAY['compile','wmc'], preference => 5,
                     enabled => false);
SELECT count(*) AS rows, max(preference) AS pref, bool_or(enabled) AS enabled
  FROM tools WHERE name='acme-kc';

-- A NULL executable defaults to the logical name; NULL kind defaults to cli.
SELECT register_tool('baretool');
SELECT executable, kind FROM tools WHERE name='baretool';

-- set_tool_enabled / set_tool_preference return void and take effect.
SELECT set_tool_enabled('d4', false);
SELECT enabled FROM tools WHERE name='d4';
SELECT set_tool_preference('d4', 7);
SELECT preference FROM tools WHERE name='d4';

-- They error loudly on an unknown tool name (a typo must not silently no-op).
SELECT set_tool_enabled('no-such-tool', false);
SELECT set_tool_preference('no-such-tool', 7);

-- unregister_tool removes the record; a second call errors (already gone).
SELECT unregister_tool('acme-kc');
SELECT count(*) FROM tools WHERE name='acme-kc';
SELECT unregister_tool('acme-kc');

-- Dispatch consults the registry: an unknown compiler, a disabled native
-- compiler, and a disabled Panini variant all fail before any tool is
-- resolved on PATH, so these are host-independent.
CREATE TABLE tr_r(x int);
INSERT INTO tr_r VALUES (1);
SELECT add_provenance('tr_r');
SELECT probability_evaluate(provsql, 'compilation', 'no-such-compiler') FROM tr_r;
SELECT set_tool_enabled('dsharp', false);
SELECT probability_evaluate(provsql, 'compilation', 'dsharp') FROM tr_r;
SELECT set_tool_enabled('panini-obdd', false);
SELECT probability_evaluate(provsql, 'compilation', 'panini-obdd') FROM tr_r;
DROP TABLE tr_r;

-- The mutators are superuser-only: execute is revoked from PUBLIC.
SELECT
  has_function_privilege('public', 'provsql.register_tool(text,text,text,text[],text[],text,text,text,int,boolean)', 'EXECUTE') AS register,
  has_function_privilege('public', 'provsql.unregister_tool(text)', 'EXECUTE') AS unregister,
  has_function_privilege('public', 'provsql.set_tool_enabled(text,boolean)', 'EXECUTE') AS set_enabled,
  has_function_privilege('public', 'provsql.set_tool_preference(text,int)', 'EXECUTE') AS set_preference;
