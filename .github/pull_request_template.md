<!-- Thanks for contributing to ProvSQL! -->

## Summary

<!-- What does this PR change? One or two sentences. -->

## Motivation

<!--
Why is this change necessary?  Link to the related issue if applicable:

    Closes #123
    Fixes #456
-->

## Checklist

Tick the rows that apply to this PR.

**Extension (C / C++ / SQL under `src/`, `sql/`, `test/`)**

- [ ] `make test` passes locally (integration tests via `pg_regress`)
- [ ] New SQL tests live in `test/sql/` with matching expected output in `test/expected/`, and are registered in `test/schedule.common` or `test/schedule.14`
- [ ] New C / C++ symbols carry a Doxygen comment

**Studio (Python / JS / CSS under `studio/`)**

- [ ] `make studio-test` passes locally (chains `ruff check` and runs the unit + Playwright e2e suite under `studio/tests/`)
- [ ] New tests for the changed behaviour live in `studio/tests/` (pytest unit) or `studio/tests/e2e/` (Playwright smoke)

**Documentation (`doc/`, applies to both components)**

- [ ] New or changed user-visible behaviour is reflected in the user guide (`doc/source/user/`)
- [ ] New or changed internal behaviour is reflected in the developer guide (`doc/source/dev/`), with any new `:sqlfunc:` / `:cfunc:` / `:cfile:` cross-references added to `doc/source/conf.py`
- [ ] `make docs` succeeds and the coherence checker (`check-doc-links.py`) reports `OK`

## Notes for reviewers

<!--
Anything reviewers should pay extra attention to?  Sharp edges, open
questions, performance trade-offs, on-disk ABI concerns (see
src/MMappedCircuit.h), etc.
-->

<!--
A note on CHANGELOG files: please do **not** modify either
`CHANGELOG.md` (extension) or `studio/CHANGELOG.md` (Studio) in pull
requests.  The extension's CHANGELOG is maintained automatically by
`release.sh` at extension release time; Studio's CHANGELOG is
maintained by hand by the maintainer at Studio release time.  The PR
description itself is the right place to explain user-visible changes
so the maintainer has the material when cutting the release.
-->
