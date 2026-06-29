"""Test harness for ProvSQL Studio.

A session-scoped fixture spins up an isolated PostgreSQL database, installs
the provsql extension, and runs the upstream `setup` + `add_provenance` test
SQL files so each test starts with the same `personnel` table the regression
suite uses.

The DSN can be overridden with the `PROVSQL_STUDIO_TEST_DSN` env var (CI use)
; in that case the harness assumes the database is already prepared and only
verifies the personnel table exists.
"""
from __future__ import annotations

import os
import secrets
import subprocess
from pathlib import Path

import psycopg
import pytest

from provsql_studio.app import create_app


REPO_ROOT = Path(__file__).resolve().parents[2]
SETUP_SQL_FILES = [
    REPO_ROOT / "test" / "sql" / "setup.sql",
    REPO_ROOT / "test" / "sql" / "add_provenance.sql",
]
# The Case Study 7 fixture (peer-review data + the external-review pool that
# drives the Möbius step), loaded into its own database for the e2e walkthrough.
CASESTUDY7_SETUP = REPO_ROOT / "doc" / "casestudy7" / "setup.sql"

# The Case Study 2 fixture (Open Science evidence corpus) and the extra SQL its
# Contributions-mode walkthrough needs on top of setup.sql: the `f` view, its
# study_mapping, per-finding probabilities, and the f_replicated view (Steps
# 2-12 of doc/source/user/casestudy2.rst, mirrored in test/sql/casestudy2.sql).
CASESTUDY2_SETUP = REPO_ROOT / "doc" / "casestudy2" / "setup.sql"
CS2_CONTRIB_PREP = """
SET search_path TO public, provsql;
SELECT add_provenance('finding');
CREATE VIEW f AS
  SELECT study.title    AS study,
         study.study_type,
         study.reliability,
         exposure.name   AS exposure,
         outcome.name    AS outcome,
         finding.effect
  FROM finding
    JOIN study    ON finding.study_id    = study.id
    JOIN exposure ON finding.exposure_id = exposure.id
    JOIN outcome  ON finding.outcome_id  = outcome.id;
SELECT create_provenance_mapping('study_mapping', 'f', 'study');
DO $$ BEGIN PERFORM set_prob(provenance(), reliability) FROM f; END $$;
CREATE VIEW f_replicated AS
  SELECT exposure, outcome, effect FROM f
  GROUP BY exposure, outcome, effect HAVING COUNT(*) >= 2;
"""


# Temporal-mode fixture: the CS4 personnel/holds tenure data (table, a join
# view, and the extended time_validity_view) plus a handful of small tables
# that exercise the timeline's edge cases (sub-minute scale, a single instant,
# an unbounded validity, an empty union, and a two-component multirange). All
# validity is set BEFORE add_provenance so the update trigger does not fire.
TEMPORAL_SETUP = """
SET search_path TO public, provsql;
SET TIME ZONE 'UTC';

CREATE TABLE cs4_person (id INTEGER PRIMARY KEY, name TEXT NOT NULL,
                         validity tstzmultirange);
CREATE TABLE cs4_holds (id INTEGER REFERENCES cs4_person(id), position TEXT NOT NULL,
                        country CHAR(2) NOT NULL, validity tstzmultirange,
                        PRIMARY KEY (id, position));
INSERT INTO cs4_person (id, name, validity) VALUES
  (1, 'Alice Blanc',  tstzmultirange(tstzrange('1960-01-01+00', NULL))),
  (2, 'Bernard Chai', tstzmultirange(tstzrange('1965-01-01+00', NULL))),
  (3, 'Carla Diop',   tstzmultirange(tstzrange('1970-01-01+00', NULL)));
INSERT INTO cs4_holds (id, position, country, validity) VALUES
  (1, 'Prime Minister', 'FR', tstzmultirange(tstzrange('2010-01-01+00','2016-01-01+00'))),
  (2, 'Prime Minister', 'FR', tstzmultirange(tstzrange('2016-01-01+00','2022-01-01+00'))),
  (3, 'Prime Minister', 'FR', tstzmultirange(tstzrange('2022-01-01+00', NULL)));
SELECT add_provenance('cs4_person');
SELECT add_provenance('cs4_holds');
SELECT create_provenance_mapping('cs4_person_validity', 'cs4_person', 'validity', maintained => true);
SELECT create_provenance_mapping('cs4_holds_validity',  'cs4_holds',  'validity', maintained => true);
ALTER VIEW provsql.time_validity_view RENAME TO time_validity_view_cs4_save;
CREATE VIEW provsql.time_validity_view AS
    SELECT * FROM provsql.time_validity_view_cs4_save
  UNION ALL SELECT * FROM cs4_person_validity
  UNION ALL SELECT * FROM cs4_holds_validity;
CREATE VIEW cs4_person_position AS
  SELECT DISTINCT name, position
  FROM cs4_person JOIN cs4_holds ON cs4_person.id = cs4_holds.id
  WHERE country = 'FR';

CREATE TABLE sensor (id int, reading text, validity tstzmultirange);
INSERT INTO sensor VALUES
  (1, 'A', tstzmultirange(tstzrange('2024-03-15 14:00+00','2024-03-15 14:05+00'))),
  (2, 'B', tstzmultirange(tstzrange('2024-03-15 14:03+00','2024-03-15 14:12+00'))),
  (3, 'C', tstzmultirange(tstzrange('2024-03-15 14:10+00','2024-03-15 14:30+00')));
SELECT add_provenance('sensor');
SELECT create_provenance_mapping('sensor_validity', 'sensor', 'validity', maintained => true);

CREATE TABLE degen (id int, label text, validity tstzmultirange);
INSERT INTO degen VALUES
  (1, 'point',   tstzmultirange(tstzrange('2018-03-15 14:30+00','2018-03-15 14:30+00','[]'))),
  (2, 'alltime', tstzmultirange(tstzrange(NULL, NULL)));
SELECT add_provenance('degen');
SELECT create_provenance_mapping('degen_validity', 'degen', 'validity', maintained => true);

CREATE TABLE emptyval (id int, label text, validity tstzmultirange);
INSERT INTO emptyval VALUES
  (1, 'normal', tstzmultirange(tstzrange('2016-01-01+00','2022-01-01+00'))),
  (2, 'never',  tstzmultirange());
SELECT add_provenance('emptyval');
SELECT create_provenance_mapping('emptyval_validity', 'emptyval', 'validity', maintained => true);

CREATE TABLE multi (id int, label text, validity tstzmultirange);
INSERT INTO multi VALUES
  (1, 'twoparts', tstzmultirange(tstzrange('2000-01-01+00','2001-01-01+00'),
                                 tstzrange('2010-01-01+00','2011-01-01+00')));
SELECT add_provenance('multi');
SELECT create_provenance_mapping('multi_validity', 'multi', 'validity', maintained => true);
"""


def _read_setup_sql(path: Path) -> str:
    """Strip pg_regress meta-commands (\\set, \\pset) so we can run the file
    directly via psycopg without psql. Both setup files start with a couple
    of `\\` lines for output formatting; the rest is plain SQL."""
    out_lines = []
    for line in path.read_text().splitlines():
        if line.startswith("\\"):
            continue
        out_lines.append(line)
    return "\n".join(out_lines)


@pytest.fixture(scope="session")
def test_dsn() -> str:
    """Either reuse PROVSQL_STUDIO_TEST_DSN or create a fresh one-off DB."""
    override = os.environ.get("PROVSQL_STUDIO_TEST_DSN")
    if override:
        # Caller is responsible for the schema. Verify minimally.
        with psycopg.connect(override) as conn, conn.cursor() as cur:
            cur.execute("SELECT 1 FROM pg_extension WHERE extname='provsql'")
            assert cur.fetchone(), (
                "PROVSQL_STUDIO_TEST_DSN points to a database without "
                "the provsql extension. Install it before running tests."
            )
        yield override
        return

    # Create a unique database keyed on a random suffix so parallel runs don't
    # collide. We connect to the maintenance database `postgres`.
    suffix = secrets.token_hex(4)
    db_name = f"provsql_studio_test_{suffix}"
    admin_dsn = "dbname=postgres"

    with psycopg.connect(admin_dsn, autocommit=True) as admin:
        admin.execute(f'CREATE DATABASE "{db_name}"')

    try:
        target_dsn = f"dbname={db_name}"
        # setup.sql installs the extension itself (CREATE EXTENSION CASCADE,
        # then a drop/recreate cycle) and establishes the database-level
        # search_path via ALTER DATABASE. That default only applies to
        # *future* sessions, so each file gets its own fresh connection:
        # add_provenance.sql (which calls into the provsql schema unqualified)
        # must run in a session opened after setup.sql committed, the way
        # pg_regress runs every test file in a fresh session.
        for sqlfile in SETUP_SQL_FILES:
            with psycopg.connect(target_dsn, autocommit=True) as conn:
                conn.execute(_read_setup_sql(sqlfile))
        yield target_dsn
    finally:
        with psycopg.connect(admin_dsn, autocommit=True) as admin:
            # Forcibly drop any leftover connections so DROP doesn't block.
            admin.execute(
                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                "WHERE datname = %s AND pid <> pg_backend_pid()",
                (db_name,),
            )
            admin.execute(f'DROP DATABASE IF EXISTS "{db_name}"')


@pytest.fixture(scope="session")
def cs7_dsn() -> str:
    """A fresh database loaded with the Case Study 7 fixture
    (`doc/casestudy7/setup.sql`): the peer-review relations plus the dense
    external-review pool the Möbius step needs.  Mirrors `test_dsn`'s
    create / load / drop lifecycle; overridable with `PROVSQL_STUDIO_CS7_DSN`."""
    override = os.environ.get("PROVSQL_STUDIO_CS7_DSN")
    if override:
        yield override
        return

    suffix = secrets.token_hex(4)
    db_name = f"provsql_studio_cs7_{suffix}"
    admin_dsn = "dbname=postgres"
    with psycopg.connect(admin_dsn, autocommit=True) as admin:
        admin.execute(f'CREATE DATABASE "{db_name}"')
    try:
        target_dsn = f"dbname={db_name}"
        # setup.sql installs the extension itself; feed it the file contents
        # with the psql meta-commands (\\echo) stripped.
        with psycopg.connect(target_dsn, autocommit=True) as conn:
            conn.execute(_read_setup_sql(CASESTUDY7_SETUP))
        yield target_dsn
    finally:
        with psycopg.connect(admin_dsn, autocommit=True) as admin:
            admin.execute(
                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                "WHERE datname = %s AND pid <> pg_backend_pid()",
                (db_name,),
            )
            admin.execute(f'DROP DATABASE IF EXISTS "{db_name}"')


@pytest.fixture(scope="session")
def cs2_dsn() -> str:
    """A fresh database with the Case Study 2 fixture prepared for
    Contributions mode.  Its `setup.sql` loads the base tables with
    `COPY ... FROM stdin` (a psql client feature, not runnable through
    psycopg), so it is fed to `psql`; the `f` view / study_mapping /
    probabilities / f_replicated are then layered on with psycopg.
    Overridable with `PROVSQL_STUDIO_CS2_DSN`."""
    override = os.environ.get("PROVSQL_STUDIO_CS2_DSN")
    if override:
        yield override
        return

    suffix = secrets.token_hex(4)
    db_name = f"provsql_studio_cs2_{suffix}"
    admin_dsn = "dbname=postgres"
    with psycopg.connect(admin_dsn, autocommit=True) as admin:
        admin.execute(f'CREATE DATABASE "{db_name}"')
    try:
        subprocess.run(
            ["psql", "-q", "-v", "ON_ERROR_STOP=1", "-d", db_name,
             "-f", str(CASESTUDY2_SETUP)],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        with psycopg.connect(f"dbname={db_name}", autocommit=True) as conn:
            conn.execute(CS2_CONTRIB_PREP)
        yield f"dbname={db_name}"
    finally:
        with psycopg.connect(admin_dsn, autocommit=True) as admin:
            admin.execute(
                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                "WHERE datname = %s AND pid <> pg_backend_pid()",
                (db_name,),
            )
            admin.execute(f'DROP DATABASE IF EXISTS "{db_name}"')


@pytest.fixture(scope="session")
def cs8_dsn() -> str:
    """A fresh database with only the provsql extension installed, for the
    Case Study 8 notebook walkthrough.  CS8 is notebook-first and fully
    self-contained -- its cells create every table inline -- so the database
    needs nothing but the extension.  Overridable with
    `PROVSQL_STUDIO_CS8_DSN`."""
    override = os.environ.get("PROVSQL_STUDIO_CS8_DSN")
    if override:
        yield override
        return

    suffix = secrets.token_hex(4)
    db_name = f"provsql_studio_cs8_{suffix}"
    admin_dsn = "dbname=postgres"
    with psycopg.connect(admin_dsn, autocommit=True) as admin:
        admin.execute(f'CREATE DATABASE "{db_name}"')
    try:
        with psycopg.connect(f"dbname={db_name}", autocommit=True) as conn:
            conn.execute("CREATE EXTENSION IF NOT EXISTS provsql CASCADE")
        yield f"dbname={db_name}"
    finally:
        with psycopg.connect(admin_dsn, autocommit=True) as admin:
            admin.execute(
                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                "WHERE datname = %s AND pid <> pg_backend_pid()",
                (db_name,),
            )
            admin.execute(f'DROP DATABASE IF EXISTS "{db_name}"')


@pytest.fixture(scope="session")
def temporal_dsn() -> str:
    """A fresh database loaded with the Temporal-mode fixture (`TEMPORAL_SETUP`):
    the CS4 tenure data, a join view, the extended time_validity_view, and the
    small edge-case tables. Mirrors the other `*_dsn` lifecycles; overridable
    with `PROVSQL_STUDIO_TEMPORAL_DSN`."""
    override = os.environ.get("PROVSQL_STUDIO_TEMPORAL_DSN")
    if override:
        yield override
        return

    suffix = secrets.token_hex(4)
    db_name = f"provsql_studio_temporal_{suffix}"
    admin_dsn = "dbname=postgres"
    with psycopg.connect(admin_dsn, autocommit=True) as admin:
        admin.execute(f'CREATE DATABASE "{db_name}"')
    try:
        with psycopg.connect(f"dbname={db_name}", autocommit=True) as conn:
            conn.execute("CREATE EXTENSION IF NOT EXISTS provsql CASCADE")
            conn.execute(TEMPORAL_SETUP)
        yield f"dbname={db_name}"
    finally:
        with psycopg.connect(admin_dsn, autocommit=True) as admin:
            admin.execute(
                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                "WHERE datname = %s AND pid <> pg_backend_pid()",
                (db_name,),
            )
            admin.execute(f'DROP DATABASE IF EXISTS "{db_name}"')


@pytest.fixture()
def app(test_dsn: str, tmp_path, monkeypatch):
    """Per-test Flask app bound to the test DSN, with the schema search_path
    pre-set so unqualified `personnel` references resolve.

    Also redirects Studio's on-disk config (used by /api/config persistence)
    into a per-test tmp dir so tests can't read or write the user's real
    ~/.config/provsql-studio/config.json. The env var must be in place
    before `create_app()` runs because the factory eagerly loads any
    persisted GUC overrides into RUNTIME_GUCS, so we set it here rather
    than in an autouse fixture (whose ordering relative to `app` is
    fragile).

    Closes the app's connection pool on teardown : create_app opens a
    psycopg ConnectionPool with min_size=1, so every test that did not
    explicitly close it would leak at least one PG connection for the
    rest of the pytest process.  At ~160 tests that's enough to blow
    past PG's default max_connections."""
    monkeypatch.setenv("PROVSQL_STUDIO_CONFIG_DIR", str(tmp_path / "studio_cfg"))
    app = create_app(dsn=f"{test_dsn} options='-c search_path=provsql_test,provsql,public'")
    app.config.update(TESTING=True)
    try:
        yield app
    finally:
        kernels = app.extensions.get("provsql_kernels") or {}
        close_all = kernels.get("close_all")
        if close_all is not None:
            try:
                close_all()
            except Exception:
                pass
        pool = app.extensions.get("provsql_pool")
        if pool is not None:
            try:
                pool.close()
            except Exception:
                pass


@pytest.fixture()
def client(app):
    return app.test_client()


@pytest.fixture()
def temporal_app(temporal_dsn: str, tmp_path, monkeypatch):
    """Per-test Flask app bound to the Temporal fixture DB, search_path set to
    `public, provsql` so the fixture's relations and mappings resolve. Same
    config-dir redirect and pool teardown as `app`."""
    monkeypatch.setenv("PROVSQL_STUDIO_CONFIG_DIR", str(tmp_path / "studio_cfg"))
    app = create_app(dsn=f"{temporal_dsn} options='-c search_path=public,provsql'")
    app.config.update(TESTING=True)
    try:
        yield app
    finally:
        pool = app.extensions.get("provsql_pool")
        if pool is not None:
            try:
                pool.close()
            except Exception:
                pass


@pytest.fixture()
def temporal_client(temporal_app):
    return temporal_app.test_client()
