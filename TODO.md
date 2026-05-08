# Coordinated release: ProvSQL 1.4.0 + Studio 1.0.0

Working document. Not committed (delete after the release window
closes). Tracks the ordered sequence of work needed to ship both
tags together. Source-of-truth lives in `doc/TODO/studio.md` for
the Studio side; this file is the operational checklist.

Two tags ship in the same window:

- **`v1.4.0`** — ProvSQL extension (PGXN, Docker Hub).
- **`studio-v1.0.0`** — ProvSQL Studio (PyPI, GitHub release).

The Docker image carries both: `inriavalda/provsql:1.4.0` includes
the Studio install (item P1.E) so a single `docker pull` gives a
ready-to-demo container.

---

## Phase 1: pre-tag work (can run in parallel)

### P1.A — Studio test infrastructure

- [x] Add `make studio` target to top-level `Makefile`
      (`python -m provsql_studio`). Done: also added a
      `studio-test` target right next to it.
- [x] Add `make studio-test` target running `pytest studio/tests`
      plus the Playwright e2e suite. `pytest tests` walks both
      the unit tests and `tests/e2e/`, so a single invocation
      runs everything.
- [x] Set up Playwright: install dev dependency in
      `studio/pyproject.toml` `[project.optional-dependencies]`
      (`test = ["pytest", "playwright", ...]`), add
      `studio/tests/e2e/` with Playwright config + a smoke
      scenario per mode (Where, Circuit, query history,
      connection editor). Added `playwright>=1.40` and
      `pytest-playwright>=0.4` to the `test` extra.
      `tests/e2e/conftest.py` spawns Studio in a subprocess
      against the existing `test_dsn` fixture, with
      `PROVSQL_STUDIO_CONFIG_DIR` pointed at a tempdir so a
      developer's persisted UI settings can't override the
      `--search-path provsql_test` we pass at startup.
      `tests/e2e/test_smoke.py` covers eight scenarios (the
      four the TODO scoped, plus mode-switch query
      carry-forward, eval-strip running `boolexpr` against a
      pinned node, schema-panel column-click prefilling
      `create_provenance_mapping`, and the per-query
      `where_provenance` toggle being interactive in Circuit
      mode); the result-row assertion uses `#result-count`
      (not the generic `#result-body tr`, which the
      "Running…" placeholder also satisfies).
- [x] Confirm `pytest studio/tests` and the Playwright smoke
      tests pass locally against a live PG with extension built
      from the current branch. Verified: 122/122 pass in ~18s
      (114 unit + 8 Playwright e2e) after the user reinstalled
      the matching extension binary.
- [x] Lint: `cd studio && ruff check .` is clean. Cleaned
      pre-existing issues: an unused `Iterator` import in
      `db.py`, a stray `f`-prefix on a non-format-string SQL
      template, two `if cond: return` E701 one-liners, and two
      unused `import pytest` lines in `test_circuit.py` /
      `test_exec.py`. None were caused by P1.A but the TODO
      requires ruff clean, so they got swept up here.

### P1.B — CI: `studio.yml`

- [x] Write `.github/workflows/studio.yml`:
  - [x] Triggers: `studio/**`, `sql/provsql.common.sql`,
        `sql/provsql.14.sql`, the workflow file itself,
        `workflow_dispatch`.
  - [x] Matrix: Py 3.10 / 3.11 / 3.12 / 3.13 × PG 14 / 15 / 16.
  - [x] Job 1 — pytest: build extension, install Studio, run
        `pytest studio/tests`. Runs `pytest studio/tests
        --ignore=studio/tests/e2e` so the e2e job below stays
        the only one paying the chromium-download tax.
  - [x] Job 2 — Playwright e2e: spin up PG + extension + Studio,
        run the e2e suite. `playwright install --with-deps
        chromium` (root in pgxn/pgxn-tools, no sudo).
  - [x] Job 3 — lint: `ruff check`.
  - [x] Job 4 — package smoke: `python -m build` +
        `pip install dist/*.whl` + `provsql-studio --help`.
        Installs into a fresh `/tmp/wheel-venv` so the smoke
        proves the wheel is self-sufficient (no implicit
        editable-checkout dep).
  - [x] **Bonus**: `test` and `e2e` matrix jobs `needs: lint` so a
        broken `ruff check` short-circuits the 24 matrix cells.
        Also chained `ruff check` into the local
        `make studio-test` target so missed lint can't surprise
        CI.
- [x] Push to a feature branch, confirm all matrix cells pass.
      Run id `25565303792` is green: 26 jobs in 4m51s
      (ruff + package + 12 pytest + 12 Playwright).
      **Three follow-up fixes** were needed after the first
      push, each landed in its own commit:
      `Studio CI: invoke pip/pytest/playwright via python -m`
      (`setup-python` exports `pythonLocation` but doesn't put
      `bin/` on PATH inside a `container:`),
      `Studio CI: install graphviz in matrix containers (dot
      binary)` (`circuit.py` shells out to `dot`), and
      `Studio CI: bump actions/setup-python to v6 (Node.js
      24)` (silences the Node 20 deprecation warning; this
      one is committed but not yet pushed at the time of the
      green run).

### P1.C — PyPI name claim

- [x] Confirm `provsql-studio` is unclaimed
      (`pypi.org/pypi/provsql-studio` 404 today). Confirmed:
      `provsql-studio`, `provsql_studio` (the PyPI-normalised
      form), and `provsql-studio` on Test PyPI all return 404.
- [x] Decide release path: tag-triggered workflow, or
      `studio-release.sh`. **Recommended**: workflow + Trusted
      Publisher (no API token in repo secrets). Decided:
      **tag-triggered GitHub workflow** (`studio-release.yml`,
      written in P1.D) using a PyPI **Pending Publisher** so
      no API token lands in repo secrets.
- [x] If workflow: configure Trusted Publisher on PyPI side
      pointing at `studio-release.yml` and the `studio-v*` tag
      pattern. (PyPI Trusted Publisher creation requires the
      first publish to come from the configured workflow; either
      do a `0.0.1.dev1` rehearsal upload or accept that the very
      first 1.0.0 publish doubles as the claim.) **Footnote
      outdated**: PyPI introduced *Pending Publishers* (2023+)
      precisely to skip this chicken-and-egg — pre-register the
      publisher for a non-existent project and the first
      workflow run both claims the name and publishes via
      Trusted Publishing. No rehearsal upload needed.
      Pending Publisher registered with:
      `PyPI Project Name = provsql-studio`,
      `Owner = PierreSenellart`,
      `Repository name = provsql`,
      `Workflow name = studio-release.yml`,
      `Environment name = pypi`. **P1.D consequence**: the
      `studio-release.yml` job that publishes must declare
      `environment: pypi` to match the Pending Publisher; a
      matching `pypi` environment must also exist on the GitHub
      repo (Settings → Environments).

### P1.D — Studio release pipeline

- [x] Write `.github/workflows/studio-release.yml` **or**
      `studio-release.sh`:
  - [x] Trigger on `studio-v*` tag (workflow) / take a version
        arg (script). Workflow path chosen (matches the P1.C
        Trusted Publisher decision). Triggers on `push: tags:
        ['studio-v*']` and on `workflow_dispatch` (with a `tag`
        input) so a missing tag push can be retried by hand.
  - [x] Run the studio test job first; abort on red. Done as a
        single-cell `gate` job (Py 3.12 × PG 16) running the
        same ruff + pytest steps as `studio.yml`. Cheaper than
        re-running the 24-cell matrix at release time, and
        the matrix already gates each push of the same commit.
  - [x] `python -m build` to produce
        `dist/provsql_studio-*.tar.gz` (sdist) +
        `dist/provsql_studio-*-py3-none-any.whl` (wheel). The
        `build` job also asserts that the produced filenames
        carry the version parsed from the tag, so a stale
        `__version__` in `provsql_studio/__init__.py` fails
        loudly instead of publishing a mismatched wheel.
  - [x] Publish to PyPI via `pypa/gh-action-pypi-publish`
        (workflow) / `twine upload` (script). `environment:
        pypi` matches the Pending Publisher; OIDC via
        `id-token: write` (no API token in repo secrets).
  - [x] Create GitHub release with `gh release create
        studio-v$VERSION dist/*` so the sdist + wheel are
        attached as assets. The `studio/` line was removed
        from `.gitattributes` `export-ignore`, so the
        auto-generated source tarball now carries the full
        repo (including studio/) for both `v*` and
        `studio-v*` tags. PGXN bundle picks up the same
        ~510 KB of Python source (built via `git archive`
        from the same `.gitattributes`); harmless, since
        PGXN's indexer respects `META.json`'s
        `no_index.directory` and the extension `make` chain
        doesn't touch studio/.
  - [x] Release notes pin `pip install provsql-studio==$VERSION`
        as the canonical install path; mention extension >= 1.4.0
        is required. Notes also embed a "What's changed" section
        extracted from `studio/CHANGELOG.md` by an awk one-liner;
        the workflow fails loudly if no section matches the
        tag's version (catches a missing CHANGELOG entry before
        an empty-notes release lands).

### P1.E — Docker integration

- [x] Edit `docker/Dockerfile`:
  - [x] Install Python 3 + `pip` + `python3-venv`. Also dropped
        `apache2 libapache2-mod-php php-pgsql` (no longer
        needed); kept `graphviz` (`dot` for circuit layout) and
        `libgraph-easy-perl` (extension's `view_circuit` ASCII
        rendering).
  - [x] `ARG STUDIO_VERSION=1.0.0` (default = the released PyPI
        version) and `ARG STUDIO_SOURCE=` (empty default).
  - [x] If `STUDIO_SOURCE` is set, `pip install -e
        ${STUDIO_SOURCE}`; else `pip install
        provsql-studio==${STUDIO_VERSION}`. Installed into
        `/opt/studio-venv` (Debian bookworm's Python 3.11 enforces
        PEP 668, so a system-wide pip install is rejected); venv
        bin/ prepended to PATH so `provsql-studio` resolves.
- [x] Edit `docker/demo.sh`:
  - [x] Drop `cp -r /opt/provsql/where_panel/* /var/www/html/`
        and the `sed` lines that follow. (Done in P1.F.)
  - [x] After PG is up, launch
        `provsql-studio --host 0.0.0.0 --port 8000 --dsn
        postgresql://test:test@localhost/test &`. Used a libpq
        DSN (`'dbname=test user=test'`) since the container's
        `pg_hba.conf` already grants local-trust auth, and added
        `--search-path provsql_test` so the `personnel` fixture
        is reachable without schema-qualified names.
  - [x] Print Studio URL alongside the psql info: `Studio:
        http://${IP}:8000`.
  - [x] Decide whether Apache stays at all (likely no; remove the
        `apache2` start if Studio fully replaces `where_panel`).
        Apache removed: `apache2` package, `apt-get install`
        line, `init.d/apache2 start` invocation, the `/var/www`
        prep, and `EXPOSE 80` are all gone. `EXPOSE 8000` for
        Studio takes its place. Also replaced the dead
        `tail -f /messages | sed ...` URL-rewrite loop with an
        `exec tail -f /messages` PID-1 keeper.
- [ ] Local smoke test:
      `make docker-build && docker run --rm -p 8000:8000
      provsql:dev`, then open `http://localhost:8000` and confirm
      `/where` and `/circuit` load against the test database.
      **User action**: needs the Docker daemon and ~few minutes
      of build time. **Note**: until Studio 1.0.0 is published
      to PyPI, `make docker-build` (which uses the default
      `STUDIO_SOURCE=`) will fail at the pip step; use the
      dry-run below to verify locally before publish.
- [ ] Dry-run with `--build-arg
      STUDIO_SOURCE=/opt/provsql/studio` to confirm the
      contributor path also works when Studio isn't on PyPI yet
      (this is the path used at release time, before the PyPI
      tag has landed). **User action**: invoke as
      `make clean && docker build -f docker/Dockerfile
      --build-arg PROVSQL_VERSION=1.4.0-dev
      --build-arg STUDIO_SOURCE=/opt/provsql/studio
      -t provsql:dev .`, then `docker run --rm -p 8000:8000
      provsql:dev`.

### P1.F — `where_panel/` cleanup

- [x] Delete the `where_panel/` directory.
- [x] Confirm `docker/demo.sh` no longer references it (P1.E
      already drops the copy block). Done here in P1.F: dropped
      the `cp -r .../where_panel/*`, the two `sed` lines that
      followed, and the trailing `where_panel web interface`
      echo block. Apache start and `/var/www/html/pdf` setup
      stay for P1.E to revisit alongside the Studio launch.
- [x] Remove `where_panel/**` from the two `paths-ignore` blocks
      in `.github/workflows/docs.yml`.
- [x] `git grep -n where_panel` and clean any stale references
      in `doc/source/`, `website/`, `README.md`. None in
      `doc/source/`, `website/` (current pages), or `README.md`.
      Cleaned: `META.json` `no_index.directory` entry,
      attribution comment in `studio/provsql_studio/db.py:221`.
      Left intact: `CHANGELOG.md` and
      `website/_data/releases.yml` (immutable history),
      `doc/TODO/studio.md` (Phase 3 cleanup),
      `studio/design/ui_kits/where_panel/` and references to it
      from `studio/design/ui_kits/circuit/index.html` (the
      whole `studio/design/` tree is deleted in P1.I).

### P1.G — Website visibility

- [x] Add a Studio section to `website/index.html` (top-of-fold
      card or hero panel) with at least one screenshot.
      Done as a 3-up first feature row: "Transparent Query
      Rewriting" + "Evaluate Provenance" + "ProvSQL Studio",
      each carrying an image. The original three-card row
      (Query Rewriting / Rich Semiring / Probability & Shapley)
      was reduced by merging Semiring + Probability into a
      single "Evaluate Provenance" card to make room for Studio
      while keeping the row at 3.
- [x] Add a Studio top-nav entry (or "Studio" tile on
      `overview.md`) linking to `/docs/user/studio.html` and to
      the PyPI page. Both: top-nav entry "Studio" between
      Documentation and Publications (links to docs); a
      `## ProvSQL Studio` H2 on `overview.md` between Query
      Rewriting and Lean Formalization (links to docs and the
      PyPI page).
- [x] Pick screenshots: `doc/source/_static/studio/where-mode.png`
      and `circuit-mode.png` are the strongest existing two.
      Optionally re-shoot at higher resolution using the
      OS-level capture procedure documented in `CLAUDE.md`.
      `circuit-mode.png` used on the Studio card. Two further
      images extracted from the ICDE'26 poster
      (`sen2026provsql_poster.pdf`): the Query rewriting flow
      diagram for the Query Rewriting card, and the Semiring
      Instantiation / Evaluation of circuits / Result trio for
      the Evaluate Provenance card. Re-shoots not needed.
- [x] Verify with `make website` and a local Jekyll preview that
      the Studio panel renders correctly on desktop and mobile
      widths. `make website` clean; Jekyll build OK; verified
      locally at `http://localhost:8765/`.

### P1.H — Compatibility floor

- [x] Update `doc/source/user/studio.rst` compatibility matrix:
      add a row for Studio 1.0.0 ↔ extension >= 1.4.0.
      (Already present, lines 519-531; calls out
      `circuit_subgraph`, `resolve_input`, and the
      `provsql.aggtoken_text_as_uuid` GUC, all confirmed absent
      from `sql/upgrades/*` for any version ≤ 1.3.1.)
- [x] Confirm Studio's startup version check
      (`SELECT extversion FROM pg_extension WHERE extname =
      'provsql'`) refuses to start if extension < 1.4.0; add /
      bump the floor in `studio/provsql_studio/db.py` or
      wherever the check lives. (Verify the check exists; if
      not, add it.) Lives in `studio/provsql_studio/cli.py`
      (`REQUIRED_PROVSQL_VERSION = (1, 4, 0)`,
      `_check_extension_version`); accepts a `-dev` suffix as
      the matching release; `--ignore-version` overrides.
- [x] `studio/pyproject.toml`: confirm `version` resolves to
      `1.0.0` (already done via dynamic `__version__ = "1.0.0"`).
- [x] `make docs` clean.

### P1.I – `studio/` housekeeping

- [x] Audit `studio/` and remove anything that is not the Python
      package, its tests, or its docs / release plumbing. Targets:
  - [x] `studio/design/` – initial design documents, the embedded
        `where_panel/` UI-kit copy under
        `design/ui_kits/where_panel/`, and `design/screenshots/`.
        12 tracked files removed via `git rm -r studio/design/`.
  - [x] `studio/.claude/` – not shipped with the package. Already
        untracked (covered by the user's global `~/.gitignore`,
        verified via `git check-ignore`); no project change needed.
  - [x] Any other build artefacts, scratch files, or legacy
        prototypes still tracked by git. Use `git ls-files studio/`
        before and after the cleanup as a check. Before: 44 files
        (incl. `design/`). After: 32 files (`LICENSE`,
        `provsql_studio/`, `pyproject.toml`, `README.md`,
        `scripts/`, `tests/`). `scripts/` (six `load_demo_*.sql`
        + `big_demo_queries.sql`) kept as developer-facing demo
        loaders for the case-study semirings.
- [x] After the cleanup, rerun `python -m build && pip install
      dist/*.whl` in a throwaway venv to confirm the wheel still
      installs and `provsql-studio --help` still works.

---

## Merge gate: `studio` → `master`

All Phase 1 work happens on the `studio` branch. Before Phase 2
can run (the release scripts operate on `master`), the branch
needs to land on master via a merge.

- [ ] On `studio`, merge in the latest `master` to absorb any
      drift and resolve conflicts there:
      `git fetch origin && git merge origin/master`.
- [ ] Confirm the full extension test suite is green on `studio`
      after the merge (`sudo make install && sudo service
      postgresql restart && make installcheck`), plus `pytest
      studio/tests` and `make docs`.
- [ ] Switch to `master`, merge `studio` in (no fast-forward, so
      the integration is visible in the history):
      `git checkout master && git merge --no-ff studio`.
- [ ] Push `master`. The branch deletion is deferred to Phase 3
      so any release-time fixups can still land on `studio` and
      be re-merged if needed.

---

## Phase 2: coordinated release

Run **only after** every Phase 1 item is checked, the merge gate
above has landed on `master`, both extension master and Studio
master are green on all CI workflows (Linux / macOS / WSL /
studio.yml), and the local Docker smoke test succeeds.

### P2.1 — Extension v1.4.0

- [ ] On master, run `./release.sh 1.4.0`. The script handles:
      version bump in `provsql.common.control`, CHANGELOG entry,
      `website/_data/releases.yml` entry, `CITATION.cff`,
      `META.json`, signed tag, push, GitHub release. Approve the
      post-release `1.5.0-dev` bump it offers at the end.
- [ ] Wait for `build_and_test.yml` Docker job to complete.
      Confirm `inriavalda/provsql:1.4.0` and
      `inriavalda/provsql:latest` are visible on Docker Hub.
- [ ] Confirm PGXN auto-built v1.4.0 (check pgxn.yml run; PGXN
      catalogue updates within ~minutes).

### P2.2 — Studio v1.0.0

- [ ] If using the workflow path: push tag `studio-v1.0.0`. The
      release workflow runs studio.yml's test matrix, builds
      sdist + wheel, publishes to PyPI via Trusted Publisher,
      and creates the GitHub release with the artifacts
      attached.
- [ ] If using the script path: run
      `./studio-release.sh 1.0.0`. Same outcome, locally
      driven.
- [ ] Confirm `pip install provsql-studio==1.0.0` works in a
      throwaway venv. Confirm
      `https://pypi.org/project/provsql-studio/` renders the
      README.

### P2.3 — End-to-end smoke

- [ ] In a fresh shell:
      `docker run --rm -p 8000:8000 inriavalda/provsql:1.4.0`,
      open `http://localhost:8000`, run a query in Where mode,
      switch to Circuit mode, confirm both work end-to-end.
- [ ] Outside Docker: in a fresh venv,
      `pip install provsql-studio` against an existing local PG
      with extension 1.4.0 installed; confirm the same.
- [ ] If Studio refuses to start against a 1.3.0 install
      (intentional, per the version check), confirm the error
      message is clear.

### P2.4 — Website deploy

- [ ] On master: `make deploy`. The website pulls in the new
      `releases.yml` entry (1.4.0), the Studio visibility
      section (P1.G), and the compatibility matrix.
- [ ] Spot-check `https://provsql.org/`: Studio panel visible,
      release page lists 1.4.0 with notes, docs include the
      Studio chapter at the new compatibility floor.

### P2.5 — Announce

- [ ] Lab / mailing list announcement covering both releases.
- [ ] Update Studio's PyPI long-description (the README
      shipped in the wheel) if anything in the announcement
      should land there too — would require a 1.0.1 follow-up
      since PyPI uploads are immutable.

---

## Phase 3: post-release housekeeping

- [ ] Studio: bump `provsql_studio/__version__` to `"1.1.0.dev0"`
      (or `1.0.1.dev0` if the next release is a patch). Commit
      to master.
- [ ] Extension: confirm `release.sh` already pushed the
      `1.5.0-dev` post-release bump.
- [ ] Strike the v1.0-blocking items out of
      `doc/TODO/studio.md` (the "Blocking v1.0" section) so the
      file only carries the "Beyond v1.0" plan going forward.
      Update the intro paragraph to drop the coordinated-release
      note (or rewrite it to describe what shipped).
- [ ] Delete this file (`TODO.md`) once both releases are
      announced and stable for ~a week.
- [ ] Delete the `studio` branch locally and on the remote once
      both releases are stable: `git branch -d studio && git push
      origin --delete studio`.

---

## Risks / things to watch

- **PyPI Trusted Publisher first-publish**: Trusted Publishers
  on PyPI need the project to exist before the publisher can be
  configured, which is awkward when the project doesn't exist
  yet. Two ways out: (a) upload a `0.0.1.dev1` from a workflow
  one-time (claims the name and lets you configure Trusted
  Publishing for subsequent releases), or (b) do the very first
  upload with an API token, then switch to Trusted Publisher
  for 1.0.1 onwards. Decide before P1.C.
- **Docker image size**: adding Python + Studio increases the
  image. If size matters, consider a multi-stage build (out of
  scope for the release window unless it bloats noticeably).
- **`v1.4.0` vs `studio-v1.0.0` ordering**: the extension tag
  must land first so the Docker image with extension 1.4.0
  exists before Studio 1.0 announces it as the supported floor.
  Phase 2 enforces this order.
- **`.gitattributes` and the auto-tarball**: ~~studio/ was
  `export-ignore`'d so the auto-tarball would be empty of Studio
  code on `studio-v*`~~ — line removed in this branch; the
  auto-tarball now carries the full repo at both `v*` and
  `studio-v*` tags. PGXN bundle gains the same ~510 KB of
  Python source, harmless per the analysis above.
