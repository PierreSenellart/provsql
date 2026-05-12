#!/usr/bin/env python3
"""City Air-Quality Sensor Network demo loader.

Python sibling of ``demo_continuous.sh`` for users who prefer not to
depend on ``bash`` / ``psql`` shell tooling. Re-creates a fresh
database, loads the SQL fixture, and (unless ``--no-launch`` is
passed) starts ProvSQL Studio against it.

The fixture itself is the SQL file ``demo_continuous.sql`` in this
directory.  See ``doc/source/user/casestudy6.rst`` for the worked
example this loader backs.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Load the City Air-Quality demo fixture and launch Studio.",
    )
    parser.add_argument(
        "--db", default="air_quality_demo",
        help="PostgreSQL database name (default: air_quality_demo).",
    )
    parser.add_argument(
        "--port", type=int, default=8000,
        help="Studio HTTP port (default: 8000).",
    )
    parser.add_argument(
        "--no-launch", action="store_true",
        help="Load the fixture but do not start Studio.",
    )
    args = parser.parse_args()

    here = Path(__file__).resolve().parent
    fixture = here / "demo_continuous.sql"
    if not fixture.is_file():
        print(f"missing fixture: {fixture}", file=sys.stderr)
        return 1

    # Recreate the database.
    existing = subprocess.run(
        ["psql", "-lqt"], check=True, capture_output=True, text=True
    ).stdout
    if any(line.split("|", 1)[0].strip() == args.db for line in existing.splitlines()):
        print(f"Dropping existing database {args.db}...")
        subprocess.run(["dropdb", args.db], check=True)
    print(f"Creating database {args.db}...")
    subprocess.run(["createdb", args.db], check=True)

    print("Loading fixture...")
    subprocess.run(["psql", "-d", args.db, "-f", str(fixture)], check=True)

    if args.no_launch:
        print()
        print(f"Database {args.db} is ready.  Launch Studio with:")
        print(f"  provsql-studio --dsn postgresql:///{args.db} --port {args.port}")
        return 0

    dsn = f"postgresql:///{args.db}"
    print()
    print(f"Launching Studio at http://127.0.0.1:{args.port}/ against {dsn} ...")

    studio = shutil.which("provsql-studio")
    if studio:
        os.execvp(studio, [studio, "--dsn", dsn, "--port", str(args.port)])
    # Fall back to module form from the source tree.
    os.execvp(sys.executable, [
        sys.executable, "-m", "provsql_studio",
        "--dsn", dsn, "--port", str(args.port),
    ])


if __name__ == "__main__":
    sys.exit(main())
