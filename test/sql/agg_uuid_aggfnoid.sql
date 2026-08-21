\set ECHO none
\pset format unaligned

-- Regression for the agg-gate UUID collision: provenance_aggregate(...)
-- used to derive the gate UUID from the children list alone. SUM(id)
-- and AVG(id) over the same rows then collapsed to a single gate, and
-- their concurrent set_infos calls would race to overwrite each
-- other's aggfnoid (observed as wrong probabilities in the
-- having_boolean_connectors parallel group, where provsql_having would
-- read the wrong agg_kind). The aggfnoid is now part of the v5 hash,
-- so two aggregations differing only in their aggregation function
-- get distinct gates.
--
-- The rewriter is disabled here so that array_agg(provenance_semimod
-- (...)) is evaluated as a plain SQL aggregate (returning uuid[]) and
-- not lifted into a provenance_aggregate(...) call.
--
-- The aggregate values are read from the table rather than written as
-- literals: an agg gate's UUID hashes its function and children but not
-- its value, so a made-up value would collide with the real one the
-- rewriter records for the same aggregation -- and a gate's annotation
-- is written once.
SET provsql.active = false;

CREATE TEMP TABLE _agg_uuid_tmp AS
SELECT array_agg(provsql.provenance_semimod(id, provsql)) AS sm
FROM provsql_test.personnel WHERE city='Paris';

SET provsql.active = true;

CREATE TEMP TABLE _agg_uuid_uuids AS
SELECT
  provsql.agg_token_uuid(provsql.provenance_aggregate(
    'sum(int)'::regprocedure::oid::integer,
    'bigint'::regtype::oid::integer,
    (SELECT sum(id) FROM provsql_test.personnel WHERE city='Paris'),
    sm)) AS sum_uuid,
  provsql.agg_token_uuid(provsql.provenance_aggregate(
    'avg(int)'::regprocedure::oid::integer,
    'numeric'::regtype::oid::integer,
    (SELECT avg(id) FROM provsql_test.personnel WHERE city='Paris'),
    sm)) AS avg_uuid
FROM _agg_uuid_tmp;

SELECT (sum_uuid <> avg_uuid)                            AS sum_avg_distinct,
       provsql.get_gate_type(sum_uuid)::text             AS sum_type,
       provsql.get_gate_type(avg_uuid)::text             AS avg_type,
       (provsql.get_infos(sum_uuid)).info1::oid::regproc AS sum_aggfn,
       (provsql.get_infos(avg_uuid)).info1::oid::regproc AS avg_aggfn
FROM _agg_uuid_uuids;

DROP TABLE _agg_uuid_uuids;
DROP TABLE _agg_uuid_tmp;
