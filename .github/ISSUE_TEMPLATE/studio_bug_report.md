---
name: Studio bug report
about: Report a bug or unexpected behaviour in ProvSQL Studio (the web UI)
title: ''
labels: bug, studio
---

## Environment

- **ProvSQL Studio version**: <!-- run: provsql-studio --version (or `pip show provsql-studio`) -->
- **ProvSQL extension version**: <!-- run in Studio's query box: SELECT extversion FROM pg_extension WHERE extname = 'provsql'; -->
- **PostgreSQL version**: <!-- SELECT version(); -->
- **Python version**: <!-- python3 --version -->
- **Operating system**: <!-- e.g. Ubuntu 22.04, macOS 14.5, Windows 11 -->
- **Browser**: <!-- e.g. Firefox 138, Chrome 140; relevant for any UI-visible bug -->
- **Installed from**: <!-- PyPI / source checkout (`pip install -e ./studio`) / Docker image -->

## Describe the bug

<!-- A clear and concise description of what you expected and what actually happened. Which mode (Where / Circuit) were you in? -->

## Reproducer

<!--
Minimal steps to trigger the bug. If a specific SQL query is involved,
include the schema setup so the reproducer is self-contained:

    CREATE TABLE personnel ...
    INSERT INTO personnel VALUES ...
    SELECT add_provenance('personnel');
    -- then in Studio's query box:
    SELECT name FROM personnel WHERE city = 'Paris';
-->

```sql
-- your reproducer here
```

## Expected behaviour

<!-- What you expected Studio to render or do. -->

## Actual behaviour

<!-- The error banner, console error, broken render, or wrong result you observed. Screenshots help if the bug is visual. -->

## Browser console / network output (optional)

<!--
If the bug is in the front end, open the browser devtools, reproduce, and
paste any red console messages and any failing /api/* responses here.
-->

## Studio server log (optional)

<!--
If `provsql-studio` printed anything to stderr while reproducing, paste
the relevant lines here.
-->
