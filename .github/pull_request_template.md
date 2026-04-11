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

- [ ] `make test` passes locally (integration tests via `pg_regress`)
- [ ] New or changed user-visible behaviour is reflected in the user guide (`doc/source/user/`)
- [ ] New or changed internal behaviour is reflected in the developer guide (`doc/source/dev/`), with any new `:sqlfunc:` / `:cfunc:` / `:cfile:` cross-references added to `doc/source/conf.py`
- [ ] New SQL tests live in `test/sql/` with matching expected output in `test/expected/`, and are registered in `test/schedule.common` or `test/schedule.14`
- [ ] New C / C++ symbols carry a Doxygen comment
- [ ] `make docs` succeeds and the coherence checker (`check-doc-links.py`) reports `OK`

## Notes for reviewers

<!--
Anything reviewers should pay extra attention to?  Sharp edges, open
questions, performance trade-offs, on-disk ABI concerns (see
src/MMappedCircuit.h), etc.
-->

<!--
A note on CHANGELOG.md: please do **not** modify it in pull requests.
The release script (release.sh) maintains it automatically at release
time from the release notes you give it.  The PR description itself is
the right place to explain user-visible changes.
-->
