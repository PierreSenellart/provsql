# ProvSQL

[![Build Status](https://github.com/PierreSenellart/provsql/actions/workflows/build_and_test.yml/badge.svg?branch=master)](https://github.com/PierreSenellart/provsql/actions/workflows/build_and_test.yml)
[![Build Status](https://github.com/PierreSenellart/provsql/actions/workflows/macos.yml/badge.svg?branch=master)](https://github.com/PierreSenellart/provsql/actions/workflows/macos.yml)
[![Build Status](https://github.com/PierreSenellart/provsql/actions/workflows/wsl.yml/badge.svg?branch=master)](https://github.com/PierreSenellart/provsql/actions/workflows/wsl.yml)

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.19512786.svg)](https://doi.org/10.5281/zenodo.19512786)
[![Archived in Software Heritage](https://archive.softwareheritage.org/badge/origin/https://github.com/PierreSenellart/provsql/)](https://archive.softwareheritage.org/browse/origin/?origin_url=https://github.com/PierreSenellart/provsql)
[![Docker Image Version](https://img.shields.io/docker/v/inriavalda/provsql?sort=semver&label=docker)](https://hub.docker.com/r/inriavalda/provsql)

ProvSQL adds (m-)semiring provenance and uncertainty management to PostgreSQL,
enabling computation of probabilities, Shapley values, and various semiring
evaluations, as a PostgreSQL extension.

Website: **<https://provsql.org/>** – Documentation: **<https://provsql.org/docs/>**

## Quick Install

**Prerequisites:** PostgreSQL ≥ 10, a C++17 compiler, PostgreSQL development
headers, `uuid-ossp`, and the Boost libraries (`libboost-dev`,
`libboost-serialization-dev`).

```sh
make
make install   # as a user with write access to the PostgreSQL directories
```

Add to `postgresql.conf` and restart PostgreSQL:

```
shared_preload_libraries = 'provsql'
```

Then in each database:

```sql
CREATE EXTENSION provsql CASCADE;
```

See the [full installation guide](https://provsql.org/docs/user/getting-provsql.html)
for prerequisites, optional dependencies, testing, and Docker instructions.

## Studio

[ProvSQL Studio](https://pypi.org/project/provsql-studio/) is a separate
Python package that adds a web UI on top of the extension: a Circuit mode
that renders the provenance DAG behind any result UUID, with frontier
expansion, an inspector, and on-the-fly semiring evaluation; and a Where
mode that hover-highlights the source cells of each output value.

```sh
pip install provsql-studio
provsql-studio --dsn postgresql://localhost/mydb
```

See the [Studio chapter](https://provsql.org/docs/user/studio.html) of the
documentation.

## License

ProvSQL is provided as open-source software under the MIT License. See [LICENSE](LICENSE).

## Contact

<https://github.com/PierreSenellart/provsql>

Pierre Senellart <pierre@senellart.com>

Bug reports and feature requests are
preferably sent through the *Issues* feature of GitHub.
