# Studio: features and integration before / after v1.0

Plan for Studio work landing alongside or after the first public
release (`studio-v1.0.0` on PyPI). The Studio user guide is at
`doc/source/user/studio.rst`; the compatibility matrix, version
streams, and CLI flags are documented there. This file covers what
is still pending across release plumbing, Docker swap-over, and
post-1.0 mode work. The original `studio/TODO.md` (Stages 0–5 of
the bring-up) has been retired now that the work it tracked has
shipped; see git log for the history.

**Coordinated release**: ProvSQL Studio 1.0.0 will ship at the same
time as ProvSQL extension 1.4.0. This means the extension's own
release.sh run, the Docker Hub publish job (`build_and_test.yml`),
and Studio's PyPI publication need to be sequenced together: the
Docker image and the Studio compatibility matrix should both
reference 1.4.0 as the supported extension floor at the moment
Studio 1.0.0 is announced. Coordinate the two tags (`v1.4.0` and
`studio-v1.0.0`) in the same release window.

## Out of scope

The following are documented elsewhere and do not need a TODO entry:

- Compiled-semiring proposals: covered by `compiled-semirings.md`.
- Tutorial / case-study coverage gaps: covered by `case-studies.md`
  and `feature-coverage.md`.

## Blocking v1.0

### Release plumbing

- **PyPI**: smoke-test in a fresh venv against a local PG with
  ProvSQL, then tag `studio-v1.0.0` and upload. The
  `provsql-studio` name on PyPI is currently unclaimed (404 on
  `pypi.org/pypi/provsql-studio`); confirm before tagging.
- **Source-tree integration**: top-level `Makefile` targets
  `make studio` (`python -m provsql_studio`, assumes the user has
  a venv with the package installed) and `make studio-test` (run
  the studio test suite, including the Playwright end-to-end
  tests). The one-time `pip install -e ./studio` step for
  contributors lives in the contributor docs, not the Makefile.

### CI workflows

- **`.github/workflows/studio.yml`**: trigger on changes under
  `studio/**`, `sql/provsql.common.sql`, `sql/provsql.14.sql`, or
  the workflow file itself (plus `workflow_dispatch`). Matrix
  Python 3.10 / 3.11 / 3.12 / 3.13 × PostgreSQL 14 / 15 / 16.
  Jobs: build extension + run `pytest studio/tests`; run the
  Playwright end-to-end tests; lint with `ruff` (and optionally
  `mypy`); package smoke test (`python -m build` +
  `pip install dist/*.whl` + `provsql-studio --help`).
- **Studio release pipeline**: either a tag-triggered
  `.github/workflows/studio-release.yml` (on `studio-v*` tags:
  run the test job, build sdist + wheel, publish to PyPI via
  `pypa/gh-action-pypi-publish` with a Trusted Publisher, create
  a GitHub release with the artifacts) or a `studio-release.sh`
  analogous to the existing extension `release.sh` (local
  interactive driver: validate version, check CI green, bump
  files, sign tag, build + upload to PyPI, push, create the GH
  release).
- **GitHub release artifacts**: `studio/` is `export-ignore`'d in
  `.gitattributes` so that extension tarballs stay PGXN-clean.
  The auto-generated GitHub tarball for a `studio-v*` tag is
  therefore empty of Studio code. Don't fight that: the release
  pipeline must attach the PyPI artifacts (sdist
  `provsql_studio-X.Y.Z.tar.gz` + wheel
  `provsql_studio-X.Y.Z-py3-none-any.whl` from `python -m build`)
  as explicit release assets via `gh release create ... <files>`,
  and the release notes should point users at `pip install
  provsql-studio` rather than the auto source tarball.

### Docker integration

- Add Python + Studio install to `docker/Dockerfile`. Hybrid via
  `ARG STUDIO_VERSION=<latest-released>` defaults to the released
  PyPI version; a `--build-arg STUDIO_SOURCE=/opt/provsql/studio`
  override switches to `pip install -e ${STUDIO_SOURCE}` for
  contributors building locally. The existing Docker Hub publish
  job in `build_and_test.yml` then carries the new image with no
  workflow changes.
- Update `docker/demo.sh`: launch `provsql-studio --host 0.0.0.0
  --port 8000` in addition to (or in place of) Apache. Print the
  Studio URL alongside the psql info.
- Local smoke test:
  `docker build -t provsql-demo docker/ && docker run --rm -p 8000:8000 provsql-demo`,
  then confirm `/where` and `/circuit` load against the test
  database.

### Website visibility

- Surface Studio in a visible spot on `https://provsql.org/`, not
  buried behind a docs link. Candidates: a dedicated section on
  `index.html`, a top-nav entry, and / or a feature card on
  `overview.md`. Include screenshots — the existing
  `doc/source/_static/studio/*.png` (Where mode, Circuit mode,
  Config panel, circuit close-up) are a starting point and can be
  copied / re-shot with the OS-level capture procedure documented
  in `CLAUDE.md`.

### `where_panel/` cleanup

- Remove `where_panel/` once the Docker image and docs are switched
  over to Studio. Drop the
  `cp -r /opt/provsql/where_panel/* /var/www/html/` block in
  `docker/demo.sh`. Remove `where_panel/**` from `docs.yml`'s
  `paths-ignore`. Sweep the docs for stale `where_panel` references.

## Beyond v1.0

### New inspection modes

Two additional modes share the existing chrome (query box,
result-table rendering, mode switcher) and add their own sidebar
plus per-cell click affordances.

#### Contributions mode

- Heat-map of per-input Shapley / Banzhaf contributions for the
  current result. The mode switcher gains a third tab; the sidebar
  lists input gates with mapping-resolved labels and a per-input
  contribution bar; result-table rows get a "→ Contributions"
  affordance similar to the existing "→ Circuit".
- Backed by `shapley_all_vars` / `banzhaf_all_vars`. The eval-strip
  variants (per-node `shapley` / `banzhaf` with a variable-token
  picker) likely fold into this mode rather than living on the
  circuit canvas.
- Closes the CS2 §13–15 gap.

#### Time-travel / Temporal DB mode

- Dedicated chrome for the temporal SRFs `timeslice` / `history` /
  `timetravel` (CS4 §3–5). Sidebar = view picker + date / window /
  column-filter that composes the SRF call; the result table
  renders the SRF output.
- Motivation: the SRF call shape (`... AS (cols ...)`) plus the
  date / window / filter inputs warrant their own chrome rather
  than a generic eval-strip mini-panel.
- Natural home for a future **"undo last DML"** button (CS4 §7)
  that calls `SELECT undo(...)` server-side. Kept out of the
  main modes for now since `update_provenance` is not yet mature
  enough to expose prominently.

### Notebooks (small)

- Save / load notebooks: query history is already persisted
  (sessionStorage `ps.sql` carry-over + the History dropdown). The
  remaining work is a "Download .sql" button next to the History
  dropdown that exports the recent buffer, and a file-picker that
  imports back into the textarea. About 30 lines of front-end.

### Larger features

- **Result-table evaluation extension**: run the selected semiring
  across every row of the current result and add a column with the
  per-row value. Today the eval strip evaluates one node at a time;
  this would batch-evaluate all UUIDs in the displayed `provsql`
  column.
- **Knowledge-compilation view**: render the d-DNNF compiled from a
  circuit, not just the raw provenance DAG. Surfaces what
  `provenance_evaluate_compiled` actually consumes and makes
  probability evaluation legible. Could ship as a sub-mode toggled
  from the circuit-mode toolbar (`Π`-shaped circuit ↔ d-DNNF view).
- **Multi-user demo deployment**: per-browser-session isolation in
  a single Docker container so a conference audience can each hit
  `localhost:8000` against a hosted instance.

## Implementation observations

- **Mode pattern**: each new mode is a sidebar template plus a
  route plus an `/api/...` endpoint, with the existing mode
  switcher gaining a tab. The `wrap_last` flag in `db.exec_batch`
  is the generalisation point: extend the dispatch in `app.py` to
  support the new mode's wrapping (or no wrapping) without
  touching the rest of the request path.
- **Eval-strip generalisation**: the per-node evaluation strip
  already shares `/api/evaluate` with what the future result-table
  extension would call. The extension batch-evaluates the displayed
  UUID column instead of one node, but the back-end dispatch is
  identical.
- **Independent versioning**: Studio releases as `studio-vX.Y.Z`
  on PyPI; the extension stays on `vX.Y.Z` on PGXN. Compatibility
  surfaced via a startup check
  (`SELECT extversion FROM pg_extension WHERE extname = 'provsql'`)
  that refuses to start if the installed extension is older than
  Studio's minimum requirement, plus the matrix in
  `doc/source/user/studio.rst`.
