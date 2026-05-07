# ProvSQL Studio

[![CI](https://github.com/PierreSenellart/provsql/actions/workflows/studio.yml/badge.svg?branch=master)](https://github.com/PierreSenellart/provsql/actions/workflows/studio.yml)
[![PyPI](https://img.shields.io/pypi/v/provsql-studio?style=flat)](https://pypi.org/project/provsql-studio/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Web UI for [ProvSQL](https://provsql.org/): provenance inspection, circuit
visualisation, and on-the-fly semiring evaluation.

ProvSQL is a PostgreSQL extension that adds (m-)semiring provenance and
uncertainty management to PostgreSQL, transparently rewriting queries to
track provenance as circuit tokens. ProvSQL Studio is a small Flask app
that lets you point a browser at a ProvSQL-enabled database and inspect
that provenance interactively, without writing the query-wrapping
boilerplate by hand.

Website: **<https://provsql.org/>** – Documentation: **<https://provsql.org/docs/user/studio.html>**

## Inspection modes

Two complementary modes share the same UI (query box, result table,
sidebar):

* **Where mode** highlights the source cells that contributed to each
  output value. Hover a result cell and the contributing cells of the
  underlying provenance-tracked relations light up in the sidebar. The
  query is wrapped automatically with `provsql.where_provenance`, so no
  explicit `where_provenance(...)` call is needed.

* **Circuit mode** renders the provenance DAG behind a result's UUID
  or aggregate token. Click a UUID cell to load its DAG, hover to
  highlight a subtree, click to pin a node and open the inspector.
  Frontiers expand on demand so deep circuits stay readable. An
  evaluation strip targets the pinned node (or the root) and runs
  provenance evaluation in various semirings, probability computation
  through various methods, or PROV-XML export, with the result
  rendered inline.

A schema panel and a configuration panel round out the UI; see the
documentation for the full feature reference.

## Install

```sh
pip install provsql-studio
```

Requires Python 3.10+ and a PostgreSQL database with the ProvSQL
extension installed (see the
[extension installation guide](https://provsql.org/docs/user/getting-provsql.html)).
Studio 1.0.x targets ProvSQL extension 1.4.0 or newer; the startup
check refuses to launch against an older extension unless
`--ignore-version` is passed.

## Connecting

Launch Studio with a DSN:

```sh
provsql-studio --dsn postgresql://user@localhost:5432/mydb
```

Without `--dsn`, libpq's standard environment variables (`PGDATABASE`,
`PGSERVICE`, `DATABASE_URL`…) are honoured. If neither is set,
Studio connects to the `postgres` maintenance database and offers an
in-page database picker.

The browser reaches the UI at `http://127.0.0.1:8000/`. Override the
bind address and port with `--host` and `--port`. Per-request size
caps, statement timeout, and search path are tunable on the CLI
(`--max-circuit-nodes`, `--max-sidebar-rows`, `--max-result-rows`,
`--statement-timeout`, `--search-path`) and through the Config panel;
the panel persists its settings to
`~/.config/provsql-studio/config.json`.

## Documentation

The full Studio user guide, including screenshots, the configuration
reference, the compatibility matrix, and worked examples, lives at:

<https://provsql.org/docs/user/studio.html>

For the underlying ProvSQL SQL API (`add_provenance`,
`create_provenance_mapping`, `view_circuit`, `provenance_evaluate`,
and the rest), see <https://provsql.org/docs/>.

## License

MIT: see [LICENSE](LICENSE).

## Contact

<https://github.com/PierreSenellart/provsql>

Pierre Senellart <pierre@senellart.com>

Bug reports and feature requests are preferably sent through the
*Issues* feature of GitHub.
