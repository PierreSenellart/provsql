# ProvSQL

[![Build Status](https://github.com/PierreSenellart/provsql/actions/workflows/build_and_test.yml/badge.svg?branch=master)](https://github.com/PierreSenellart/provsql/actions/workflows/build_and_test.yml)
[![Build Status](https://github.com/PierreSenellart/provsql/actions/workflows/macos.yml/badge.svg?branch=master)](https://github.com/PierreSenellart/provsql/actions/workflows/macos.yml)
[![Build Status](https://github.com/PierreSenellart/provsql/actions/workflows/wsl.yml/badge.svg?branch=master)](https://github.com/PierreSenellart/provsql/actions/workflows/wsl.yml)

The goal of the ProvSQL project is to add support for (m-)semiring provenance
and uncertainty management to PostgreSQL databases, in the form of a
PostgreSQL extension/module/plugin. It is work in progress.

**Table of contents**
- [Features](#features)
- [Docker container](#docker-container)
- [Prerequisites for installation](#prerequisites-for-installation)
- [Installation](#installation)
- [Testing your installation](#testing-your-installation)
- [Using ProvSQL](#using-provsql)
- [Uninstalling](#uninstalling)
- [License](#license)
- [Contact](#contact)

## Features

The ProvSQL system supports:
* computation of provenance of SQL queries, in the form of a provenance
  circuit, for the following forms of provenance:
  * Boolean provenance
  * semiring provenance in arbitrary semirings
  * m-semiring provenance in arbitrary semirings with monus
  * semimodule provenance of aggregate queries
  * where-provenance (if the option `provsql.where_provenance` is set)
* probability computation from the Boolean provenance for query
  evaluation over probabilistic databases, through the
  following methods:
  * naïve evaluation
  * Monte-Carlo sampling
  * building a d-DNNF representation of the provenance from a tree
    decomposition of the Boolean circuit
  * compilation to a d-DNNF using an external tool (`d4`, `c2d`,
    `minic2d` or `dsharp`)
  * approximate weighted model counting using an external tool
    (`weightmc`)
* expected value computation for COUNT/SUM/MIN/MAX aggregate queries over
  probabilistic databases
* Shapley/Banzhaf value computation
* [PROV-XML](https://www.w3.org/TR/prov-xml/) export of provenance
* expected Shapley/Banzhaf value computation over probabilistic data
* provenance tracking of updates (requires PostgreSQL ≥ 14)
* applications of provenance to temporal database support (requires
  PostgreSQL ≥ 14)

The following SQL features are currently supported.

* Regular SELECT-FROM-WHERE queries (aka conjunctive queries with
  multiset semantics)
* JOIN queries (regular joins only; outer, semijoins, and antijoins
  are not currently supported)
* SELECT queries with nested SELECT subqueries in the FROM clause
* GROUP BY queries
* SELECT DISTINCT queries (i.e., set semantics)
* UNION's or UNION ALL's of SELECT queries
* EXCEPT of SELECT queries
* VALUES() literal tables (assumed to have no provenance)
* aggregation
* HAVING queries (for simple cases)
* simple update operations (INSERT, DELETE, UPDATE), if the option `provsql.update_provenance` is set

## Docker container

As an alternative to a ProvSQL installation (see below), you can try
a demonstration version of ProvSQL (full-featured, except for `c2d` and
`minic2d` support) as a Docker container. To deploy it, once Docker CE is
installed, simply run:

```
docker run inriavalda/provsql
```

By following the instructions, you will be able to connect to the
PostgreSQL server within the container using a PostgreSQL client,
and to use a Web interface for simple visualization of where-provenance.
The Docker container can also be built locally, using:

```
make docker-build
```

## Prerequisites for installation

1. An install of PostgreSQL ≥ 10. The extension has currently been
   tested with versions from 10 to 17 (inclusive) of PostgreSQL, under
   Linux, Mac OS (both x86-64 and ARM architectures), and [Windows Subsystem for
   Linux](https://learn.microsoft.com/en-us/windows/wsl/about) (if the
   extension does not work on a specific version or operating system, a
   bug report is appreciated).

2. A compilation environment for PostgreSQL, including the `make` tool, a
   C/C++ compiler (both can be obtained on Debian-based Linux distributions
   from the virtual `build-essential` package), and the headers for your
   PostgreSQL version (as can be obtained for instance from the
   `postgresql-server-dev-xx` package on Debian-based systems, or from
   the `postgresql` package on the Homebrew package manager for Mac OS X).
   The C++ compiler should support C++ 2017.

3. The `uuid-ossp` extension for PostgreSQL (on Debian-based
   systems, it is installed automatically with PostgreSQL; on Homebrew,
   it is found in the `ossp-uuid` package; if you compile
   PostgreSQL from source, make sure to also compile and install the
   additional modules in the `contrib` directory).

4. The Boost container library and the Boost serializer library
   (on Debian-based systems, they are found in the `libboost-dev` and
   `libboost-serialization-dev` packages).

5. Optionally, for probability computation through knowledge compilation,
   any or all of the following software (note that some of them are not
   available under other OSs than Linux):
  * `c2d`, from <http://reasoning.cs.ucla.edu/c2d/download.php>
  * `d4`, from <https://github.com/crillab/d4>
  * `dsharp`, from <https://github.com/QuMuLab/dsharp>
  * `minic2d`, from <http://reasoning.cs.ucla.edu/minic2d/>
  * `weightmc`, from <https://bitbucket.org/kuldeepmeel/weightmc/src/master/>

    To be used, an executable with the name of this software must be
    available in the PATH of the PostgreSQL server user (e.g., in
    `/usr/local/bin/`).
    Using `minic2d` also requires the
    `hgr2htree` executable (it is provided with `minic2d`).

6. Optionally, for circuit visualization, the `graph-easy` executable
   from the Graph::Easy Perl library (that can be obtained from the
   `libgraph-easy-perl` package on Debian-based Linux distributions, or
   from CPAN).

## Installation

1. Compile the code with `make`. If you have several installed versions
   of PostgreSQL, you can change the version the module is compiled
   against by changing the reference to `pg_config` in the
   `Makefile.internal` file.

2. Install it in the PostgreSQL extensions directory with `make install`
   (run as a user with rights to write to the PostgreSQL installation
   directories).

3. Add the line

   ```
   shared_preload_libraries = 'provsql'
   ```

   to the `postgresql.conf` configuration file (on Linux systems, it should
   be in `/etc/postgresql/VERSION/main/postgresql.conf`) and restart the
   PostgreSQL server (e.g., with `service postgresql restart` on
   systemd-based distributions). This is required because the extension
   includes *hooks*.

## Testing your installation

You can test your installation by running `make test` as a PostgreSQL
administrator user. It will run all tests then, if tests fail, launch the
pager command (usually less) on the diff between expected and actual
output.

If you do not want to run this as the default administrator user, you can
make yourself a PostgreSQL administrator with `ALTER USER your_login
WITH SUPERUSER`. This assumes that `your_login` is a PostgreSQL user:
on Debian-based Linux distributions, you can ensure this by running the
command `createuser your_login` as the `postgres` user.

If your installation of PostgreSQL does not listen on the default (5432)
port, you can add `--port=xxxx` to the `EXTRA_REGRESS_OPTS` line of
`Makefile.internal`, where `xxxx` is the port number.

Note that the tests that depend on external software (`c2d`, `d4`,
`dsharp`, `minic2d`, `weightmc`, `graph-easy`) will fail if no executable of that
name can be found.

## Using ProvSQL

You can use ProvSQL in any PostgreSQL database by loading the
`provsql` extension. See the file [setup.sql](test/sql/setup.sql)
for an example on how to do this.

You then need to add provenance to an existing table using the
`provsql.add_provenance(regclass)` user-defined function.
See [add_provenance.sql](test/sql/add_provenance.sql) for an example.
The table will have an extra `provsql` column added. This column
is handled in a special way and always represents, in query results, the
provenance of each tuple as a UUID.

You can then use this provenance to run computation in various semirings.
See [security.sql](test/sql/security.sql) and
[formula.sql](test/sql/formula.sql) for two examples.

See the other examples in [test/sql](test/sql) for other use cases.

A demonstration of an early version of the ProvSQL system is available as
a video, on <https://youtu.be/iqzSNfGHbEE?vq=hd1080>
The SQL commands used in this demonstration can be found in the [doc/demo/](doc/demo/)
directory. An article describing this demonstration, presented at the VLDB 2018
conference, is available at
<http://pierre.senellart.com/publications/senellart2018provsql.pdf>

Finally, a ProvSQL tutorial is provided, in the form of a crime mystery.
It can be found in the [doc/tutorial/](doc/tutorial/) directory.

## Design of ProvSQL

The design of ProvSQL is described in a
[preprint](https://arxiv.org/pdf/2504.12058). The semantics implemented
by ProvSQL is also partly [formally described (and proved) in
Lean4](https://github.com/PierreSenellart/provenance-lean/).

## Uninstalling

You can uninstall ProvSQL by running `make uninstall` (run as a user with
rights to write to the PostgreSQL installation directories), and by removing the
reference to `provsql` in the `postgresql.conf` configuration file.

## License

ProvSQL is provided as open-source software under the MIT License. See [LICENSE](LICENSE).

## Contact

<https://github.com/PierreSenellart/provsql>

Pierre Senellart <pierre@senellart.com>

Bug reports and feature requests are
preferably sent through the *Issues* feature of GitHub.
