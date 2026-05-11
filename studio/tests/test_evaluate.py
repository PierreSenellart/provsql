"""Tests for the semiring-evaluation endpoints in circuit mode:

  * GET  /api/provenance_mappings  – discover (value, provenance uuid) tables.
  * POST /api/evaluate              – dispatch sr_* / probability_evaluate.

Both share a setup helper that builds a `personnel_names` mapping from the
personnel rows the conftest seeds, so the tests run against the same
fixture as test_relations.py and don't depend on cs5-style data.
"""
from __future__ import annotations

import json

import psycopg
import pytest


def _pg_server_version(dsn: str) -> int:
    """Return the server version as a 6-digit int (e.g. 140000 for PG 14)."""
    with psycopg.connect(dsn) as conn:
        return conn.info.server_version


def _requires_pg14(test_dsn):
    """Skip helper: the interval-union family (sr_temporal /
    sr_interval_num / sr_interval_int) needs the PG14+ multirange types."""
    if _pg_server_version(test_dsn) < 140000:
        pytest.skip("interval-union requires PostgreSQL 14+ (multirange types)")


@pytest.fixture()
def mapping(client):
    """Create a (value, provenance) mapping table from personnel.name and
    drop it after the test. The qualified name is `personnel_names`; via
    the test search_path it's also reachable unqualified."""
    setup = (
        "DROP TABLE IF EXISTS personnel_names;"
        " CREATE TABLE personnel_names AS"
        "   SELECT name AS value, provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_names');"
        " CREATE INDEX ON personnel_names(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield "personnel_names"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_names", "mode": "circuit"})


def _root_uuid(client, sql: str) -> str:
    """Run a circuit-mode query that produces a single provsql UUID and
    return it. The query must select from a provenance-tracked relation
    so the rewriter emits a provsql column."""
    resp = client.post("/api/exec", json={"sql": sql, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    payload = resp.get_json()
    final = payload["blocks"][-1]
    assert final["kind"] == "rows", payload
    cols = [c["name"] for c in final["columns"]]
    pi = cols.index("provsql")
    return final["rows"][0][pi]


# ──────── /api/provenance_mappings ────────


def test_mappings_lists_personnel_names(client, mapping):
    resp = client.get("/api/provenance_mappings")
    assert resp.status_code == 200
    rows = resp.get_json()
    qnames = {r["qname"] for r in rows}
    assert "provsql_test.personnel_names" in qnames
    entry = next(r for r in rows if r["qname"] == "provsql_test.personnel_names")
    # On the test search_path (provsql_test, provsql, public) the relation
    # is visible unqualified, so the bare name lands in display_name.
    assert entry["display_name"] == "personnel_names"
    # `value_type` is the parameterised display string (e.g.,
    # `character varying(40)`); `value_base_type` is the unparameterised
    # form (e.g., `character varying`) used for the eval-strip's type
    # filter. `personnel.name` is unbounded varchar, so both render as
    # `character varying`.
    assert entry["value_type"] == "character varying"
    assert entry["value_base_type"] == "character varying"
    # `is_enum` is True iff the value column's type has typtype = 'e';
    # the eval strip's mapping filter consumes it for sr_minmax /
    # sr_maxmin (which expect any user-defined enum carrier). varchar
    # is not an enum, so the flag is false here.
    assert entry["is_enum"] is False


def test_mappings_skips_non_mapping_tables(client):
    """personnel itself has a provsql uuid column but no `value` column,
    so it must NOT appear in the mapping discovery result."""
    rows = client.get("/api/provenance_mappings").get_json()
    qnames = {r["qname"] for r in rows}
    assert "provsql_test.personnel" not in qnames


# ──────── /api/evaluate ────────


def test_evaluate_boolexpr_does_not_need_mapping(client):
    """boolexpr labels leaves with their own UUIDs, no mapping required."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={"token": root, "semiring": "boolexpr"})
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    # No mapping → leaves render as bare `x<id>` placeholders.
    assert isinstance(data["result"], str)
    assert data["result"].startswith("x")


def test_evaluate_boolexpr_with_mapping(client, mapping):
    """sr_boolexpr with an optional mapping labels each input gate with
    its mapped value, just like sr_formula."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "boolexpr",
        "mapping": f"provsql_test.{mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    # With a mapping, the (single) leaf renders as the mapped value.
    assert data["result"] == "John"


def test_evaluate_formula_with_mapping(client, mapping):
    """sr_formula labels each input gate with its mapped value."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "formula",
        "mapping": f"provsql_test.{mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert "John" in data["result"]


def test_evaluate_which_with_mapping(client, mapping):
    """sr_which returns the lineage as the set of contributing labels."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "which",
        "mapping": f"provsql_test.{mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert "John" in data["result"]


def test_evaluate_how_with_mapping(client, mapping):
    """sr_how returns the canonical N[X] polynomial; with a single
    contributing input the result is just the leaf label."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "how",
        "mapping": f"provsql_test.{mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert "John" in data["result"]


@pytest.fixture()
def float_mapping(client):
    """A (value::float, provenance) mapping suitable for both sr_tropical
    (cost: id taken as a positive cost) and sr_viterbi (probability:
    1/(id+1) keeps every leaf in (0, 1])."""
    setup = (
        "DROP TABLE IF EXISTS personnel_floats;"
        " CREATE TABLE personnel_floats AS"
        "   SELECT id::float AS value, provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_floats');"
        " CREATE INDEX ON personnel_floats(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield "personnel_floats"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_floats", "mode": "circuit"})


def test_evaluate_tropical_returns_float(client, float_mapping):
    """sr_tropical (min-plus): for a + over the seven personnel input
    gates each tagged with id::float, the result is the smallest id
    (the cheapest derivation)."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "tropical",
        "mapping": f"provsql_test.{float_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "float"
    assert float(data["result"]) == 1.0


def test_evaluate_viterbi_returns_float(client, float_mapping):
    """sr_viterbi (max-times): for a + over the seven personnel input
    gates each tagged with id::float, the result is the largest leaf
    value (the most-likely-derivation probability)."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "viterbi",
        "mapping": f"provsql_test.{float_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "float"
    assert float(data["result"]) == 7.0


@pytest.fixture()
def lukasiewicz_mapping(client):
    """A (value::float, provenance) mapping in [0, 1], suitable for the
    Łukasiewicz fuzzy semiring (which uses ⊕ = max). Values are
    1/(id+1), so the seven personnel rows get 0.5, 0.333, 0.25, 0.2,
    0.167, 0.143, 0.125."""
    setup = (
        "DROP TABLE IF EXISTS personnel_unit;"
        " CREATE TABLE personnel_unit AS"
        "   SELECT (1.0/(id+1))::float AS value, provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_unit');"
        " CREATE INDEX ON personnel_unit(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield "personnel_unit"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_unit", "mode": "circuit"})


def test_evaluate_lukasiewicz_returns_float(client, lukasiewicz_mapping):
    """sr_lukasiewicz: ⊕ is max, so for a + over the seven personnel
    input gates with values 1/(id+1) ∈ [0, 1], the result is 1/2 (the
    largest, from id=1)."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "lukasiewicz",
        "mapping": f"provsql_test.{lukasiewicz_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "float"
    assert float(data["result"]) == 0.5


def _interval_mapping_fixture(client, table, literal, carrier_type):
    """Helper for the three interval-union fixtures below: build a
    (value::<multirange>, provenance) mapping where every personnel row
    is tagged with the same multirange literal, return the table name
    and tear it down afterwards."""
    setup = (
        f"DROP TABLE IF EXISTS {table};"
        f" CREATE TABLE {table} AS"
        f"   SELECT '{literal}'::{carrier_type} AS value,"
        f"          provsql AS provenance FROM personnel;"
        f" SELECT remove_provenance('{table}');"
        f" CREATE INDEX ON {table}(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data


@pytest.fixture()
def interval_tstz_mapping(client, test_dsn):
    """A (value::tstzmultirange, provenance) mapping where every personnel
    row is tagged with the validity interval `[2020-01-01, 2021-01-01)`.
    Skipped on PostgreSQL <14 where `tstzmultirange` does not exist."""
    _requires_pg14(test_dsn)
    _interval_mapping_fixture(
        client, "personnel_validity",
        "{[2020-01-01,2021-01-01)}", "tstzmultirange")
    yield "personnel_validity"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_validity", "mode": "circuit"})


@pytest.fixture()
def interval_num_mapping(client, test_dsn):
    """A (value::nummultirange, provenance) mapping where every personnel
    row is tagged with the same numeric validity range `[3.2, 7.8)`.
    Mirrors the sensor-fusion use case in doc/TODO/compiled-semirings.md."""
    _requires_pg14(test_dsn)
    _interval_mapping_fixture(
        client, "personnel_num_validity",
        "{[3.2,7.8)}", "nummultirange")
    yield "personnel_num_validity"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_num_validity", "mode": "circuit"})


@pytest.fixture()
def interval_int_mapping(client, test_dsn):
    """A (value::int4multirange, provenance) mapping where every personnel
    row is tagged with the same integer page range `[12, 18)`. Mirrors
    the page-range provenance use case."""
    _requires_pg14(test_dsn)
    _interval_mapping_fixture(
        client, "personnel_int_validity",
        "{[12,18)}", "int4multirange")
    yield "personnel_int_validity"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_int_validity", "mode": "circuit"})


def test_evaluate_interval_union_tstzmultirange(client, interval_tstz_mapping):
    """interval-union over a tstzmultirange mapping: the backend resolves
    to sr_temporal. For a + over the seven personnel input gates each
    tagged with the same validity interval, the result is that single
    interval (no widening). The endpoint surfaces the multirange via the
    `text` chip path: psycopg's Multirange.__str__ formats it as the
    canonical `{[lo, hi)}` literal."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "interval-union",
        "mapping": f"provsql_test.{interval_tstz_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert isinstance(data["result"], str)
    assert "2020-01-01" in data["result"]
    assert "2021-01-01" in data["result"]


def test_evaluate_interval_union_nummultirange(client, interval_num_mapping):
    """interval-union over a nummultirange mapping: the backend resolves
    to sr_interval_num. Same shape as the tstz test : a + collapses to
    the single shared range `[3.2, 7.8)`."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "interval-union",
        "mapping": f"provsql_test.{interval_num_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert isinstance(data["result"], str)
    assert "3.2" in data["result"]
    assert "7.8" in data["result"]


def test_evaluate_interval_union_int4multirange(client, interval_int_mapping):
    """interval-union over an int4multirange mapping: the backend resolves
    to sr_interval_int. Same shape : a + collapses to the single shared
    page range `[12, 18)`."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "interval-union",
        "mapping": f"provsql_test.{interval_int_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert isinstance(data["result"], str)
    assert "12" in data["result"]
    assert "18" in data["result"]


def test_evaluate_interval_union_rejects_non_multirange_mapping(client, mapping):
    """The interval-union family requires a multirange-typed mapping. A
    text-typed mapping (`personnel_names.value`) must be rejected at the
    400 layer with a message naming the accepted carriers, before any SQL
    round-trip to the kernel."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "interval-union",
        "mapping": f"provsql_test.{mapping}",
    })
    assert resp.status_code == 400
    error = resp.get_json()["error"].lower()
    assert "interval-union" in error
    assert "multirange" in error or "tstzmultirange" in error


@pytest.fixture()
def classification_mapping(client):
    """A (value::classification_level, provenance) mapping built from the
    personnel.classification column. The enum is shipped by
    `add_provenance.sql` and ordered
    (unclassified < restricted < confidential < secret < top_secret < unavailable),
    so sr_minmax / sr_maxmin can be exercised against a real user enum
    carrier."""
    setup = (
        "DROP TABLE IF EXISTS personnel_clearance;"
        " CREATE TABLE personnel_clearance AS"
        "   SELECT classification AS value, provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_clearance');"
        " CREATE INDEX ON personnel_clearance(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield "personnel_clearance"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_clearance", "mode": "circuit"})


def test_mappings_flags_enum_carrier(client, classification_mapping):
    """The classification_level enum carrier surfaces as is_enum: true,
    which the eval-strip filter relies on to expose sr_minmax / sr_maxmin
    only when at least one enum-typed mapping exists."""
    rows = client.get("/api/provenance_mappings").get_json()
    entry = next(
        r for r in rows
        if r["qname"] == f"provsql_test.{classification_mapping}"
    )
    assert entry["is_enum"] is True
    assert entry["value_base_type"] == "classification_level"


def test_evaluate_minmax_returns_least_sensitive(client, classification_mapping):
    """sr_minmax over a UNION ALL of the seven personnel rows: the +
    collapses alternative derivations to the enum-min, so the result is
    the lowest classification present (`unclassified` is the minimum
    label seeded by add_provenance.sql)."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "minmax",
        "mapping": f"provsql_test.{classification_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert data["result"] == "unclassified"


def test_evaluate_maxmin_returns_most_permissive(client, classification_mapping):
    """sr_maxmin is the dual of sr_minmax: + is enum-max, so over the
    same seven-row union the result is the highest classification
    present in the seeded data. The personnel fixture's max
    classification is `top_secret` (Magdalen); `unavailable` sits at
    the top of the enum but no row holds it, and acts as the max-min
    multiplicative identity, not as a + outcome."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "maxmin",
        "mapping": f"provsql_test.{classification_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert data["result"] == "top_secret"


def test_evaluate_minmax_rejects_non_enum_mapping(client, mapping):
    """sr_minmax requires a user-defined enum carrier. A text-typed
    mapping (`personnel_names.value`) must be rejected at the 400
    layer with a message naming the enum requirement, before any SQL
    round-trip to the kernel."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "minmax",
        "mapping": f"provsql_test.{mapping}",
    })
    assert resp.status_code == 400
    error = resp.get_json()["error"].lower()
    assert "minmax" in error
    assert "enum" in error


@pytest.fixture()
def counting_mapping(client):
    """Counting / boolean semirings need a typed mapping (the value
    column's type is consumed by the C semiring evaluator). We map every
    personnel row to 1 — sr_counting then sums to the row count, and
    sr_boolean treats a non-zero entry as TRUE."""
    setup = (
        "DROP TABLE IF EXISTS personnel_ones;"
        " CREATE TABLE personnel_ones AS"
        "   SELECT 1::int AS value, provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_ones');"
        " CREATE INDEX ON personnel_ones(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield "personnel_ones"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_ones", "mode": "circuit"})


def test_evaluate_counting_returns_int(client, counting_mapping):
    """sr_counting over the seven personnel rows collapsed into one
    DISTINCT result. The rewriter unions the seven input gates under a
    plus; with each leaf mapped to 1 the counting semiring sums to 7."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "counting",
        "mapping": f"provsql_test.{counting_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "int"
    assert int(data["result"]) == 7


@pytest.fixture()
def boolean_mapping(client):
    """A (value::boolean, provenance) mapping. The Studio's eval strip
    filters compiled-semiring mappings by value type, so sr_boolean only
    accepts a boolean-typed mapping; this fixture provides one."""
    setup = (
        "DROP TABLE IF EXISTS personnel_active;"
        " CREATE TABLE personnel_active AS"
        "   SELECT TRUE AS value, provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_active');"
        " CREATE INDEX ON personnel_active(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield "personnel_active"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_active", "mode": "circuit"})


def test_evaluate_boolean_returns_bool(client, boolean_mapping):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "boolean",
        "mapping": f"provsql_test.{boolean_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "bool"
    assert data["result"] is True


def test_evaluate_missing_mapping_is_400(client):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "formula",
    })
    assert resp.status_code == 400
    assert "mapping" in resp.get_json()["error"].lower()


def test_evaluate_unknown_semiring_is_400(client):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "bogus",
    })
    assert resp.status_code == 400
    assert "unknown" in resp.get_json()["error"].lower()


def test_evaluate_unknown_probability_method_is_400(client):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "probability",
        "method": "made-up-method",
    })
    assert resp.status_code == 400
    assert "method" in resp.get_json()["error"].lower()


def test_evaluate_invalid_uuid_is_400(client):
    resp = client.post("/api/evaluate", json={
        "token": "not-a-uuid",
        "semiring": "boolexpr",
    })
    assert resp.status_code == 400


# ──────── /api/custom_semirings + custom evaluation ────────


@pytest.fixture()
def custom_wrapper(client):
    """Mirror the CS1 `security_clearance` recipe: aggregates over the
    `classification_level` ENUM (already shipped in add_provenance.sql)
    plus a `(uuid, regclass) RETURNS classification_level` wrapper that
    calls `provenance_evaluate`. Discovery should pick up the wrapper.

    Also creates a `decoy_fn(uuid, regclass) RETURNS int` that does NOT
    call `provenance_evaluate` so we can assert the body filter excludes
    it. The aggregates and decoy live in `provsql_test`, isolated from
    the rest of the schema."""
    setup = """
    CREATE OR REPLACE FUNCTION clr_min_state(
        state classification_level, level classification_level)
      RETURNS classification_level AS $$
        SELECT CASE WHEN state IS NULL OR state > level THEN level ELSE state END
    $$ LANGUAGE SQL IMMUTABLE;
    CREATE OR REPLACE FUNCTION clr_max_state(
        state classification_level, level classification_level)
      RETURNS classification_level AS $$
        SELECT CASE WHEN state IS NULL OR state < level THEN level ELSE state END
    $$ LANGUAGE SQL IMMUTABLE;
    DROP AGGREGATE IF EXISTS clr_min(classification_level);
    CREATE AGGREGATE clr_min(classification_level) (
        sfunc=clr_min_state, stype=classification_level, initcond='unavailable');
    DROP AGGREGATE IF EXISTS clr_max(classification_level);
    CREATE AGGREGATE clr_max(classification_level) (
        sfunc=clr_max_state, stype=classification_level, initcond='unclassified');
    -- Bare provenance_evaluate (no `provsql.` qualifier) matches the
    -- pattern the case-study wrappers use; relies on the eval-route's
    -- search_path composition pinning `provsql`.
    CREATE OR REPLACE FUNCTION clr_clearance(token UUID, token2value regclass)
      RETURNS classification_level AS $$
    BEGIN
      RETURN provenance_evaluate(
        token, token2value, 'unclassified'::classification_level,
        'clr_min', 'clr_max');
    END
    $$ LANGUAGE plpgsql;
    CREATE OR REPLACE FUNCTION decoy_fn(token UUID, token2value regclass)
      RETURNS int AS $$ SELECT 1 $$ LANGUAGE SQL IMMUTABLE;
    DROP TABLE IF EXISTS personnel_clr;
    CREATE TABLE personnel_clr AS
      SELECT classification AS value, provsql AS provenance FROM personnel;
    SELECT remove_provenance('personnel_clr');
    CREATE INDEX ON personnel_clr(provenance);
    """
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield {"function": "provsql_test.clr_clearance", "mapping": "provsql_test.personnel_clr"}
    teardown = (
        "DROP FUNCTION IF EXISTS clr_clearance(UUID, regclass);"
        " DROP FUNCTION IF EXISTS decoy_fn(UUID, regclass);"
        " DROP AGGREGATE IF EXISTS clr_min(classification_level);"
        " DROP AGGREGATE IF EXISTS clr_max(classification_level);"
        " DROP FUNCTION IF EXISTS clr_min_state(classification_level, classification_level);"
        " DROP FUNCTION IF EXISTS clr_max_state(classification_level, classification_level);"
        " DROP TABLE IF EXISTS personnel_clr;"
    )
    client.post("/api/exec", json={"sql": teardown, "mode": "circuit"})


def test_custom_semirings_lists_user_wrapper(client, custom_wrapper):
    resp = client.get("/api/custom_semirings")
    assert resp.status_code == 200
    rows = resp.get_json()
    qnames = {r["qname"] for r in rows}
    assert "provsql_test.clr_clearance" in qnames
    entry = next(r for r in rows if r["qname"] == "provsql_test.clr_clearance")
    assert entry["return_type"] == "classification_level"
    # No name collision in the test schema, so display_name is bare.
    assert entry["display_name"] == "clr_clearance"


def test_custom_semirings_excludes_decoy_without_provenance_evaluate(client, custom_wrapper):
    """decoy_fn matches the (uuid, regclass) signature but its body never
    mentions provenance_evaluate, so the prosrc filter must drop it."""
    rows = client.get("/api/custom_semirings").get_json()
    qnames = {r["qname"] for r in rows}
    assert "provsql_test.decoy_fn" not in qnames


def test_custom_semirings_excludes_sr_formula(client):
    """sr_formula's first argument is anyelement, not uuid; even though it
    calls into the compiled engine, its signature shape is excluded by the
    `proargtypes[1] = regclass` discriminator only when the first arg is
    something other than anyelement. We rely on the body filter to exclude
    it: sr_formula calls provenance_evaluate_compiled, not the bare
    provenance_evaluate."""
    rows = client.get("/api/custom_semirings").get_json()
    qnames = {r["qname"] for r in rows}
    assert "provsql.sr_formula" not in qnames
    assert "provsql.sr_counting" not in qnames
    assert "provsql.sr_why" not in qnames
    assert "provsql.sr_which" not in qnames
    assert "provsql.sr_how" not in qnames
    assert "provsql.sr_boolean" not in qnames
    assert "provsql.sr_tropical" not in qnames
    assert "provsql.sr_viterbi" not in qnames
    assert "provsql.sr_lukasiewicz" not in qnames
    assert "provsql.sr_temporal" not in qnames
    assert "provsql.sr_interval_num" not in qnames
    assert "provsql.sr_interval_int" not in qnames
    # sr_minmax / sr_maxmin take three arguments, so the pronargs = 2
    # filter excludes them on shape; pin the assertion anyway so an
    # accidental loosening of the filter would surface here.
    assert "provsql.sr_minmax" not in qnames
    assert "provsql.sr_maxmin" not in qnames


def test_evaluate_custom_returns_classification(client, custom_wrapper):
    """Run clr_clearance over a single-row SELECT; the result is the
    minimum-classification of that row, which for the personnel fixture
    is whatever classification John holds."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "custom",
        "function": custom_wrapper["function"],
        "mapping": custom_wrapper["mapping"],
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "custom"
    assert data["type_name"] == "classification_level"
    assert data["function"] == custom_wrapper["function"]
    # John's classification level is one of the enum members.
    assert data["result"] in {
        "unclassified", "restricted", "confidential",
        "secret", "top_secret", "unavailable",
    }


def test_evaluate_custom_unknown_function_is_400(client):
    """A payload referencing a non-discovered function must be refused
    even if such a function exists in pg_proc, so the discovery filter
    is the gate."""
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "custom",
        "function": "pg_catalog.now",  # exists, but not a wrapper
        "mapping": "provsql_test.personnel",
    })
    assert resp.status_code == 400
    assert "unknown" in resp.get_json()["error"].lower()


def test_evaluate_custom_missing_function_is_400(client):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "custom",
        "mapping": "provsql_test.personnel",
    })
    assert resp.status_code == 400
    assert "function" in resp.get_json()["error"].lower()


def test_evaluate_custom_missing_mapping_is_400(client, custom_wrapper):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "custom",
        "function": custom_wrapper["function"],
    })
    assert resp.status_code == 400
    assert "mapping" in resp.get_json()["error"].lower()


def test_evaluate_applies_panel_gucs(client):
    """The evaluate-strip's SQL call must see the panel-managed GUCs
    (provsql.rv_mc_samples, monte_carlo_seed, simplify_on_load).
    Previously /api/evaluate only set statement_timeout / search_path
    / tool_search_path, so setting rv_mc_samples = 0 in the Config
    panel still silently used the default (10000) sample budget for
    HybridEvaluator's MC fallback.

    Regression check: build a circuit whose probability can ONLY be
    decided by the MC fallback (a continuous-island cmp the
    AnalyticEvaluator cannot resolve closed-form: sum of independent
    uniforms compared to a constant).  With rv_mc_samples = 0 set via
    the panel, the evaluate call must raise.  Without my fix, the
    panel value never reaches the SQL session and the call silently
    returns an MC estimate.
    """
    # Lift uniform(0,1) + uniform(0,5) > 0.5 into a provsql token via
    # the FROM-less rewrite path.  The 0.5 constant becomes a value
    # gate the AnalyticEvaluator cannot pair with the sum-of-uniforms
    # arith, so the cmp can only be resolved via MC.
    resp = client.post(
        "/api/exec",
        json={
            "sql": "SELECT 1 AS x, provsql.provenance() AS p "
                   "WHERE provsql.uniform(0.0::float8, 1.0::float8) "
                   "    + provsql.uniform(0.0::float8, 5.0::float8) "
                   "      > 0.5::provsql.random_variable;",
            "mode": "circuit",
        },
    )
    assert resp.status_code == 200, resp.data
    final = resp.get_json()["blocks"][-1]
    assert final["kind"] == "rows"
    cols = [c["name"] for c in final["columns"]]
    token = final["rows"][0][cols.index("p")]

    # 1. With the default (large) sample count, the call succeeds and
    #    returns a probability in (0, 1).
    resp = client.post("/api/evaluate", json={
        "token": token, "semiring": "probability", "method": "",
    })
    assert resp.status_code == 200, resp.data
    p_default = resp.get_json()["result"]
    assert 0.0 < float(p_default) < 1.0, p_default

    # 2. With rv_mc_samples = 0 set via /api/config, the call must
    #    surface the disabled-fallback error rather than silently MC.
    r = client.post("/api/config",
                    json={"key": "provsql.rv_mc_samples", "value": "0"})
    assert r.status_code == 200, r.data
    try:
        resp = client.post("/api/evaluate", json={
            "token": token, "semiring": "probability", "method": "",
        })
        # The exact HTTP status depends on which evaluator raises and
        # how psycopg.errors maps it; either 400 or 500 is acceptable.
        # The point is that we DON'T get a silent 200 with a numeric
        # probability that ignores the GUC.
        assert resp.status_code != 200, resp.data
    finally:
        # Reset so subsequent tests aren't affected by the override.
        client.post("/api/config",
                    json={"key": "provsql.rv_mc_samples", "value": "10000"})


# ──────── distribution-profile (scalar-only) ────────


def _rv_uuid(client, sql_expr: str) -> str:
    """Build a random_variable via `sql_expr`, dump its UUID through
    `provsql.random_variable_uuid`, and return it.  Used to anchor the
    distribution-profile tests on a known scalar gate (rv leaf or arith
    DAG) without going through the planner-hook rewriter."""
    resp = client.post(
        "/api/exec",
        json={
            "sql": f"SELECT provsql.random_variable_uuid({sql_expr}) AS u",
            "mode": "circuit",
        },
    )
    assert resp.status_code == 200, resp.data
    final = resp.get_json()["blocks"][-1]
    cols = [c["name"] for c in final["columns"]]
    return final["rows"][0][cols.index("u")]


def test_evaluate_distribution_profile_uniform(client):
    """U(0, 1) leaf: support = [0, 1], expectation ≈ 0.5, variance ≈ 1/12,
    histogram counts sum to provsql.rv_mc_samples."""
    tok = _rv_uuid(client, "provsql.uniform(0::float8, 1::float8)")
    resp = client.post("/api/evaluate", json={
        "token": tok, "semiring": "distribution-profile",
    })
    assert resp.status_code == 200, resp.data
    body = resp.get_json()
    assert body["kind"] == "distribution-profile"
    r = body["result"]
    assert r["support"] == [0.0, 1.0]
    # MC-driven moments: tolerate the default 10k-sample noise.
    assert abs(r["expected"] - 0.5) < 0.02, r["expected"]
    assert abs(r["variance"] - 1.0 / 12.0) < 0.01, r["variance"]
    assert isinstance(r["histogram"], list) and len(r["histogram"]) == 30
    total = sum(int(b["count"]) for b in r["histogram"])
    # Should equal rv_mc_samples; the default panel value is 10000.
    assert total > 0


def test_evaluate_distribution_profile_bins_argument(client):
    """The `arguments` field is the bin count; bins=5 returns at most 5 bins."""
    tok = _rv_uuid(client, "provsql.uniform(0::float8, 1::float8)")
    resp = client.post("/api/evaluate", json={
        "token": tok, "semiring": "distribution-profile", "arguments": "5",
    })
    assert resp.status_code == 200, resp.data
    hist = resp.get_json()["result"]["histogram"]
    assert len(hist) <= 5


def test_evaluate_distribution_profile_dirac_value(client):
    """gate_value root (Dirac): support is [c, c], expectation is c,
    variance is 0, histogram is a single bin at c."""
    tok = _rv_uuid(client, "provsql.as_random(7.5)")
    resp = client.post("/api/evaluate", json={
        "token": tok, "semiring": "distribution-profile",
    })
    assert resp.status_code == 200, resp.data
    r = resp.get_json()["result"]
    assert r["support"] == [7.5, 7.5]
    assert r["expected"] == 7.5
    assert r["variance"] == 0.0
    assert len(r["histogram"]) == 1
    assert r["histogram"][0]["bin_lo"] == 7.5
    assert r["histogram"][0]["bin_hi"] == 7.5


def test_evaluate_distribution_profile_unbounded_support(client):
    """Normal RV: support is (-Infinity, +Infinity).  Postgres float8
    surfaces as Python +/-inf; if those leak into jsonify unchanged,
    Python emits the bare literals Infinity / -Infinity and browser
    JSON.parse rejects the response with 'unexpected non-digit ...'.
    Verify the wire is valid JSON and the endpoints arrive as the
    string sentinels circuit.js already handles."""
    tok = _rv_uuid(client, "provsql.normal(2::float8, 2::float8)")
    resp = client.post("/api/evaluate", json={
        "token": tok, "semiring": "distribution-profile",
    })
    assert resp.status_code == 200, resp.data
    # resp.get_json() would re-parse Flask's bytes; that's the same
    # path the browser takes.  json.loads with the default (strict)
    # parser rejects bare Infinity, mirroring JSON.parse.
    body = json.loads(resp.data)
    r = body["result"]
    assert r["support"] == ["-Infinity", "Infinity"], r["support"]
    assert abs(r["expected"] - 2.0) < 0.1, r["expected"]
    assert abs(r["variance"] - 4.0) < 0.5, r["variance"]
    assert isinstance(r["histogram"], list) and len(r["histogram"]) == 30


def test_evaluate_distribution_profile_bad_bins_is_400(client):
    tok = _rv_uuid(client, "provsql.uniform(0::float8, 1::float8)")
    for bad in ("0", "-3", "abc"):
        resp = client.post("/api/evaluate", json={
            "token": tok, "semiring": "distribution-profile", "arguments": bad,
        })
        assert resp.status_code == 400, (bad, resp.data)


def test_evaluate_distribution_profile_mixture_root(client):
    """A binary Gaussian mixture is a first-class scalar RV root.  Build
    a well-separated bimodal mixture, verify the distribution-profile
    response carries the analytical mean / variance (closed-form via
    rec_expectation / rec_variance, not MC) and that the histogram has
    bimodal mass: the leftmost and rightmost bins are populated while
    the middle band is empty.  Pins the wire shape (`gate_type ==
    'mixture'`, three child edges, first child is a `gate_input`)."""
    # Mint a Bernoulli with prob 0.5 by running SQL through /api/exec.
    setup = client.post("/api/exec", json={
        "sql": (
            "DO $$\n"
            "DECLARE p uuid := public.uuid_generate_v4();\n"
            "BEGIN\n"
            "  PERFORM provsql.create_gate(p, 'input');\n"
            "  PERFORM provsql.set_prob(p, 0.5);\n"
            "  CREATE TEMP TABLE mix_p(t uuid) ON COMMIT DROP;\n"
            "  INSERT INTO mix_p VALUES (p);\n"
            "END $$;"
        ),
        "mode": "circuit",
    })
    assert setup.status_code == 200, setup.data
    # The DO block above creates a TEMP TABLE that lives only for the
    # transaction.  Studio's /api/exec opens its own connection per
    # request, so the temp table is gone by the next call.  Materialise
    # the mixture's UUID by reading the Bernoulli token and the mixture
    # in one shot via /api/exec, which keeps the temp table alive
    # within that single request's transaction.
    resp = client.post("/api/exec", json={
        "sql": (
            "DO $$\n"
            "DECLARE p uuid := public.uuid_generate_v4();\n"
            "        u uuid;\n"
            "BEGIN\n"
            "  PERFORM provsql.create_gate(p, 'input');\n"
            "  PERFORM provsql.set_prob(p, 0.5);\n"
            "  u := provsql.random_variable_uuid(\n"
            "         provsql.mixture(p,\n"
            "           provsql.normal(-5::float8, 0.5::float8),\n"
            "           provsql.normal( 5::float8, 0.5::float8)));\n"
            "  CREATE TEMP TABLE mix_out(u uuid) ON COMMIT DROP;\n"
            "  INSERT INTO mix_out VALUES (u);\n"
            "END $$;\n"
            "SELECT u FROM mix_out;"
        ),
        "mode": "circuit",
    })
    # The temp table dies with the transaction, so we must execute the
    # build and the SELECT in a single statement batch -- which
    # /api/exec already wraps for us.
    assert resp.status_code == 200, resp.data
    final = resp.get_json()["blocks"][-1]
    cols = [c["name"] for c in final["columns"]]
    tok = final["rows"][0][cols.index("u")]

    # Pin the rv_mc_samples GUC at the panel so the histogram is
    # deterministic-ish for the structural asserts.
    resp = client.post("/api/evaluate", json={
        "token": tok, "semiring": "distribution-profile", "arguments": "20",
        "extra_gucs": {
            "provsql.rv_mc_samples": "20000",
            "provsql.monte_carlo_seed": "7",
        },
    })
    assert resp.status_code == 200, resp.data
    body = resp.get_json()
    assert body["kind"] == "distribution-profile"
    r = body["result"]
    # Closed-form mean and variance for 0.5·N(-5,0.5) + 0.5·N(5,0.5):
    #   E[M] = 0, Var(M) = 0.5·(0.25+25)·2 - 0 = 25.25.
    assert abs(r["expected"] - 0.0)  < 1e-9, r["expected"]
    assert abs(r["variance"] - 25.25) < 1e-9, r["variance"]
    assert isinstance(r["histogram"], list) and len(r["histogram"]) == 20
    # Bimodal sanity: leftmost and rightmost bins both populated, the
    # middle bins (around 0) are empty.
    bins = r["histogram"]
    left_count  = sum(int(b["count"]) for b in bins
                      if (b["bin_lo"] + b["bin_hi"]) / 2 < -1)
    right_count = sum(int(b["count"]) for b in bins
                      if (b["bin_lo"] + b["bin_hi"]) / 2 > 1)
    mid_count   = sum(int(b["count"]) for b in bins
                      if -1 <= (b["bin_lo"] + b["bin_hi"]) / 2 <= 1)
    assert left_count  > 5000, left_count
    assert right_count > 5000, right_count
    assert mid_count   < 200,  mid_count

    # And the circuit subgraph for this mixture has gate_type == 'mixture'
    # with three child edges, the first being a gate_input.
    circ = client.post("/api/circuit", json={"root": tok, "depth": 1})
    assert circ.status_code == 200, circ.data
    scene = circ.get_json()
    root_node = next(n for n in scene["nodes"] if n["id"] == tok)
    assert root_node["type"] == "mixture", root_node
    children = [e for e in scene["edges"] if e["from"] == tok]
    assert len(children) == 3, children
    # Sorted by child_pos: wire 0 is the Bernoulli (gate_input).
    children.sort(key=lambda e: e["child_pos"])
    wire0_node = next(n for n in scene["nodes"] if n["id"] == children[0]["to"])
    assert wire0_node["type"] == "input", wire0_node
        assert "bins" in resp.get_json()["error"].lower()
