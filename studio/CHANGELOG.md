# Changelog

All notable changes to [ProvSQL Studio](https://provsql.org/docs/user/studio.html)
are documented in this file. Studio's version stream is independent of
the ProvSQL extension's; the
[compatibility matrix](https://provsql.org/docs/user/studio.html#compatibility)
in the user manual records each Studio release's minimum required
extension version.

This file is **maintained by hand at release time** by the Studio
maintainer (mirroring how `release.sh` maintains the extension's
top-level `CHANGELOG.md`); pull requests should not modify it. The
release workflow (`.github/workflows/studio-release.yml`) extracts the
section matching the tag's version and embeds it under "What's
changed" in the GitHub release notes.

## [1.0.0]

First public release. Requires the ProvSQL extension at version 1.4.0
or later.

### Highlights

- **Circuit mode** renders the provenance DAG behind any result UUID or
  `agg_token` cell. Drag-to-reposition, wheel-to-zoom, frontier
  expansion via gold-`+` badges, plus fullscreen, fit-to-screen and
  reset-positions toolbar buttons. Pinning a node opens an inspector
  with gate-specific metadata; `input` and `update` gates expose the
  stored probability as click-to-edit (sends `set_prob` server-side).

- **Semiring evaluation strip** runs any compiled semiring against the
  pinned node (or the root by default). Optgroups for Boolean, Lineage
  (`formula` / `how` / `why` / `which`), Numeric (`counting` /
  `tropical` / `viterbi` / `lukasiewicz`), Intervals (`interval-union`),
  User-enum (`minmax` / `maxmin`), plus probability methods, PROV-XML
  export, and any user-defined wrappers over `provenance_evaluate`. The
  mapping picker filters by the selected semiring's expected value
  type.

- **Where mode** turns on `provsql.where_provenance` and wraps every
  `SELECT` so each output cell becomes hover-aware: the source rows
  that contributed to it light up in the per-relation sidebar. Each
  result row carries a `→ Circuit` button to switch modes with the
  same provenance UUID.

- **Schema panel** lists every selectable relation with `PROV` /
  `MAPPING` pills. Click-to-prefill `add_provenance` /
  `remove_provenance` / `create_provenance_mapping` calls into the
  query box.

- **Config panel** with Provenance, Session and Display-Limits
  sections. Values persist to `$XDG_CONFIG_HOME/provsql-studio/`
  (Linux), `~/Library/Application Support/provsql-studio/` (macOS) or
  `%APPDATA%\provsql-studio\` (Windows); same options exposed as CLI
  flags.

- **In-page connection layer**: DSN editor probes new endpoints with
  `SELECT 1` before swapping pools; database switcher; the per-batch
  `search_path` always pins `provsql` at the end (lock chip in the
  header).

- **Query history**: in-session, with `Alt+↑` / `Alt+↓` to step in
  place and a `History` listbox. Mode switching carries the current
  SQL forward via `sessionStorage`; auto-replay only after an explicit
  run.
