---
name: Bug report (extension)
about: Report a bug or unexpected behaviour in the ProvSQL PostgreSQL extension. For Studio (web UI) bugs, use the Studio template instead.
title: ''
labels: bug
---

## Environment

- **ProvSQL version**: <!-- run: SELECT extversion FROM pg_extension WHERE extname = 'provsql'; -->
- **PostgreSQL version**: <!-- run: SELECT version(); -->
- **Operating system**: <!-- e.g. Ubuntu 22.04, macOS 14.5, WSL2, Docker image -->
- **Installed from**: <!-- source build / PGXN / Docker / distribution package -->
- **`shared_preload_libraries`** set to include `provsql`? <!-- yes/no -->

## Describe the bug

<!-- A clear and concise description of what you expected and what actually happened. -->

## Minimal reproducer

<!--
A minimal SQL snippet that triggers the bug. Please include any CREATE
TABLE / INSERT statements needed to seed the data, or point to an
existing test file.  Shorter is better -- the smallest SQL that still
reproduces is ideal.
-->

```sql
-- your reproducer here
```

## Expected output

<!-- What you expected the query to return or do. -->

## Actual output

<!-- The error message, stack trace, or wrong result you observed. -->

## Verbose output (optional but very helpful for rewriter bugs)

<!--
If this is a query-rewriting or semiring-evaluation issue, re-run with
ProvSQL's verbose logging enabled:

    SET provsql.verbose_level = 50;

and paste the captured backend log lines here.  See
https://provsql.org/docs/dev/debugging.html for what each level shows.
-->
