"""Tests for the semiring-evaluation endpoints in circuit mode:

  * GET  /api/provenance_mappings  – discover (value, provenance uuid) tables.
  * POST /api/evaluate              – dispatch sr_* / probability_evaluate.

Both share a setup helper that builds a `personnel_names` mapping from the
personnel rows the conftest seeds, so the tests run against the same
fixture as test_relations.py and don't depend on cs5-style data.
"""
from __future__ import annotations

import psycopg
import pytest


def _pg_server_version(dsn: str) -> int:
    """Return the server version as a 6-digit int (e.g. 140000 for PG 14)."""
    with psycopg.connect(dsn) as conn:
        return conn.info.server_version


def _requires_pg14(test_dsn):
    """Skip helper: `sr_temporal` and `tstzmultirange` need PostgreSQL 14+."""
    if _pg_server_version(test_dsn) < 140000:
        pytest.skip("sr_temporal requires PostgreSQL 14+ (tstzmultirange)")


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
    # boolexpr returns the (single) input gate's UUID for a one-row select.
    assert isinstance(data["result"], str) and data["result"]


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
def temporal_mapping(client, test_dsn):
    """A (value::tstzmultirange, provenance) mapping where every personnel
    row is tagged with the validity interval `[2020-01-01, 2021-01-01)`.
    Skipped on PostgreSQL <14 where `tstzmultirange` does not exist."""
    _requires_pg14(test_dsn)
    setup = (
        "DROP TABLE IF EXISTS personnel_validity;"
        " CREATE TABLE personnel_validity AS"
        "   SELECT '{[2020-01-01,2021-01-01)}'::tstzmultirange AS value,"
        "          provsql AS provenance FROM personnel;"
        " SELECT remove_provenance('personnel_validity');"
        " CREATE INDEX ON personnel_validity(provenance);"
    )
    resp = client.post("/api/exec", json={"sql": setup, "mode": "circuit"})
    assert resp.status_code == 200, resp.data
    yield "personnel_validity"
    client.post("/api/exec", json={"sql": "DROP TABLE personnel_validity", "mode": "circuit"})


def test_evaluate_temporal_returns_text(client, temporal_mapping):
    """sr_temporal (interval-union): for a + over the seven personnel
    input gates each tagged with the same validity interval, the result
    is that single interval (no widening). The endpoint surfaces the
    multirange via the `text` chip path: psycopg's Multirange.__str__
    formats it as the canonical `{[lo, hi)}` literal."""
    root = _root_uuid(client, "SELECT 1 AS k FROM personnel GROUP BY 1")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "temporal",
        "mapping": f"provsql_test.{temporal_mapping}",
    })
    assert resp.status_code == 200, resp.data
    data = resp.get_json()
    assert data["kind"] == "text"
    assert isinstance(data["result"], str)
    assert "2020-01-01" in data["result"]
    assert "2021-01-01" in data["result"]


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


def test_evaluate_boolean_returns_bool(client, counting_mapping):
    root = _root_uuid(client, "SELECT * FROM personnel WHERE name = 'John'")
    resp = client.post("/api/evaluate", json={
        "token": root,
        "semiring": "boolean",
        "mapping": f"provsql_test.{counting_mapping}",
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
    assert "provsql.sr_boolean" not in qnames
    assert "provsql.sr_tropical" not in qnames
    assert "provsql.sr_viterbi" not in qnames
    assert "provsql.sr_temporal" not in qnames


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
