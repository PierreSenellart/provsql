# Studio: features and integration before / after v1.0

Plan for Studio work landing alongside or after the first public
release (`studio-v1.0.0` on PyPI). The Studio user guide is at
`doc/source/user/studio.rst`; the compatibility matrix, version
streams, and CLI flags are documented there. This file covers what
is still pending across release plumbing, in-app polish, and post-1.0
mode work. The original `studio/TODO.md` (Stages 0–5 of the bring-up)
has been retired now that the work it tracked has shipped; see git
log for the history.

## Out of scope

The following are documented elsewhere and do not need a TODO entry:

- Compiled-semiring proposals: covered by `compiled-semirings.md`.
- Tutorial / case-study coverage gaps: covered by `case-studies.md`
  and `feature-coverage.md`.

## Plan

### Release plumbing for v1.0

- **PyPI**: smoke-test in a fresh venv against a local PG with
  ProvSQL, then tag `studio-v1.0.0` and upload. Confirm the
  `provsql-studio` name on PyPI before tagging. The `pyproject.toml`,
  `README.md`, `LICENSE`, and dynamic version are already in place.
- **Source-tree integration**: top-level `Makefile` targets
  `make studio` (`python -m provsql_studio`, assumes the user has a
  venv with the package installed) and `make studio-install`
  (`pip install -e ./studio` for contributors).
- **Cross-links**: `doc/source/user/where-provenance.rst` should
  point at Studio's Where mode for an interactive view;
  `doc/source/user/export.rst` should point at Studio's circuit
  visualiser next to `view_circuit`.
- **README and demos**: top-level `README.md` mentions Studio under
  "Demos" or "Tools"; `website/_data/demos.yml` gets a new entry.

### CI workflows

- **`.github/workflows/studio.yml`**: trigger on changes under
  `studio/**`, `sql/provsql.common.sql`, `sql/provsql.14.sql`, or
  the workflow file itself (plus `workflow_dispatch`). Matrix
  Python 3.10 / 3.11 / 3.12 / 3.13 × PostgreSQL 14 / 15 / 16. Three
  jobs: build extension + run `pytest studio/tests`; lint with
  `ruff` (and optionally `mypy`); package smoke test
  (`python -m build` + `pip install dist/*.whl` +
  `provsql-studio --help`).
- **`.github/workflows/studio-release.yml`**: trigger on
  `studio-v*` tags. Run the full studio test job, build sdist +
  wheel, publish to PyPI via `pypa/gh-action-pypi-publish` with a
  Trusted Publisher (no API token in repo secrets), and create a
  GitHub release attaching the artifacts.
- **Docker workflow**: extend the existing demo build to publish a
  `provsql-demo:latest` image to GHCR on each release. Out of scope
  for v1.0 if the demo image is currently built ad-hoc.

### Docker integration

- Add Python + Studio install to `docker/Dockerfile`. Hybrid via
  `ARG STUDIO_VERSION=<latest-released>` defaults to the released
  PyPI version; a `--build-arg STUDIO_SOURCE=/opt/provsql/studio`
  override switches to `pip install -e ${STUDIO_SOURCE}` for
  contributors building locally.
- Update `docker/demo.sh`: launch `provsql-studio --host 0.0.0.0
  --port 8000` in addition to (or in place of) Apache. Print the
  Studio URL alongside the psql info.
- Local smoke test:
  `docker build -t provsql-demo docker/ && docker run --rm -p 8000:8000 provsql-demo`,
  then confirm `/where` and `/circuit` load against the test
  database.

### `where_panel/` cleanup

- Remove `where_panel/` once the Docker image and docs are switched
  over to Studio. Drop the
  `cp -r /opt/provsql/where_panel/* /var/www/html/` block in
  `docker/demo.sh`. Remove `where_panel/**` from `docs.yml`'s
  `paths-ignore`. Sweep the docs for stale `where_panel` references.

### In-app polish

- **Online help inside Studio**: `<?>` tooltips and per-section "?"
  links pointing at the relevant chapter of the online doc.
  Coverage: each Config-panel row (each GUC), the Where-mode wrap
  notice, the circuit toolbar buttons, the semiring select /
  methods / args inputs, and the connection editor. Links resolve
  to `provsql.org/docs/...` anchors rather than re-explaining
  things in cramped tooltips.
- **`undo` affordance** (CS4 §7): optional one-button "undo last
  DML" on a result that came from `update_provenance`. Server-side
  calls `SELECT undo(...)`. Useful for the interactive
  data-modification case-study story.

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

#### Time-travel mode

- Dedicated chrome for the temporal SRFs `timeslice` / `history` /
  `timetravel` (CS4 §3–5). Sidebar = view picker + date / window /
  column-filter that composes the SRF call; the result table
  renders the SRF output.
- Motivation: the SRF call shape (`... AS (cols ...)`) plus the
  date / window / filter inputs warrant their own chrome rather
  than a generic eval-strip mini-panel.

### Notebooks (small)

- Save / load notebooks: query history is already persisted
  (sessionStorage `ps.sql` carry-over + the History dropdown). The
  remaining work is a "Download .sql" button next to the History
  dropdown that exports the recent buffer, and a file-picker that
  imports back into the textarea. About 30 lines of front-end.

### v2-shaped

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
- **Formula simplification**: collapse semantically-equivalent
  subgraphs (e.g. shared `times` over identical inputs) for
  readability. Likely a server-side pass before layout.
- **Tweaks panel** for Where mode: theme toggle (light only at
  v1.0), table density (comfortable / compact), highlight colour
  (terracotta / gold / purple), show / hide classification pills.
- **Multi-user demo deployment**: per-browser-session isolation in
  a single Docker container so a conference audience can each hit
  `localhost:8000` against a hosted instance.

## Priorities

1. **Block v1.0 release**: the PyPI smoke test + tag, the
   `studio.yml` and `studio-release.yml` CI workflows, the
   Makefile targets, the cross-links from `where-provenance.rst`
   and `export.rst`, and the README / `demos.yml` updates. None
   of these are large; most are mechanical.
2. **Docker swap-over**: ship the Docker integration in the same
   window as v1.0 so the demo image points at Studio rather than
   the old `where_panel/` PHP. Cleanup of `where_panel/` and the
   `docs.yml` `paths-ignore` follows immediately.
3. **In-app polish**: online help and the `undo` affordance can
   land any time after v1.0 as patch-level improvements; not
   blocking the release.
4. **New modes**: Contributions and Time-travel are v1.1
   candidates. Each is meaty enough to be its own minor release
   and closes substantial coverage gaps in the case studies.
5. **Notebooks + v2-shaped**: reach for these once the modes have
   landed and the broader UI is stable.

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
