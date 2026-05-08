"""Tests for the provsql-studio CLI: argparse + the startup version check.

The version check is best-effort against the target database: a version that
meets or exceeds the minimum lets startup proceed silently, anything older
exits with a clear message, and an unreachable database is allowed through
(the connection-pool error path will surface that more usefully).
"""
from __future__ import annotations

import secrets

import psycopg
import pytest

from provsql_studio import cli


class TestParseExtversion:
    @pytest.mark.parametrize(
        "s, expected",
        [
            ("1.4.0", (1, 4, 0)),
            # The version pinned in provsql.control during development is
            # `1.4.0-dev`. Studio must accept it as the matching release so
            # devs don't need --ignore-version on every run.
            ("1.4.0-dev", (1, 4, 0)),
            ("1.5.2", (1, 5, 2)),
            ("2.0", (2, 0, 0)),
            ("  1.4.0\n", (1, 4, 0)),
            ("1.4.0-rc1", (1, 4, 0)),
        ],
    )
    def test_parses(self, s, expected):
        assert cli._parse_extversion(s) == expected

    @pytest.mark.parametrize("s", ["", "garbage", "abc.def", "v1.4.0"])
    def test_rejects(self, s):
        assert cli._parse_extversion(s) is None


class TestCheckExtensionVersion:
    def test_passes_when_extension_meets_required(self, test_dsn):
        # The test fixture runs setup.sql + add_provenance.sql, so the
        # extension is installed at the live source-tree version, which by
        # construction meets cli.REQUIRED_PROVSQL_VERSION.
        cli._check_extension_version(test_dsn)  # no raise

    def test_exits_when_extension_too_old(self, test_dsn, monkeypatch, capsys):
        monkeypatch.setattr(cli, "REQUIRED_PROVSQL_VERSION", (99, 0, 0))
        with pytest.raises(SystemExit) as excinfo:
            cli._check_extension_version(test_dsn)
        assert excinfo.value.code == 1
        err = capsys.readouterr().err
        assert "is too old" in err
        assert "99.0.0" in err
        assert "--ignore-version" in err

    def test_exits_when_extension_not_installed(self, capsys):
        # Spin up a one-off DB with no extension loaded.
        suffix = secrets.token_hex(4)
        db = f"provsql_studio_noext_{suffix}"
        admin = "dbname=postgres"
        with psycopg.connect(admin, autocommit=True) as conn:
            conn.execute(f'CREATE DATABASE "{db}"')
        try:
            with pytest.raises(SystemExit) as excinfo:
                cli._check_extension_version(f"dbname={db}")
            assert excinfo.value.code == 1
            err = capsys.readouterr().err
            assert "not installed" in err
            assert "CREATE EXTENSION provsql" in err
        finally:
            with psycopg.connect(admin, autocommit=True) as conn:
                conn.execute(f'DROP DATABASE IF EXISTS "{db}"')

    def test_skips_on_unreachable_db(self, capsys):
        # An unreachable DSN must not exit: the version check is
        # best-effort, the connection pool will surface the failure
        # in the normal path with a more useful error.
        cli._check_extension_version(
            "host=127.0.0.1 port=1 dbname=nope user=nobody connect_timeout=2"
        )
        err = capsys.readouterr().err
        assert "Skipping" in err


class TestArgparse:
    def test_ignore_version_flag(self):
        ns = cli.build_parser().parse_args(["--ignore-version"])
        assert ns.ignore_version is True

    def test_ignore_version_default_false(self):
        ns = cli.build_parser().parse_args([])
        assert ns.ignore_version is False
