# ProvSQL

[![Build Status](https://github.com/PierreSenellart/provsql/actions/workflows/build_and_test.yml/badge.svg?branch=master)](https://github.com/PierreSenellart/provsql/actions/workflows/build_and_test.yml)

The goal of the ProvSQL project is to add support for (m-)semiring provenance
and uncertainty management to PostgreSQL databases, in the form of a
PostgreSQL extension/module/plugin. It is work in progress at the moment.

## Features

The ProvSQL system currently supports proper management of provenance
attached to SQL queries, in the form of a provenance circuit, suitable
both for regular Boolean provenance, arbitrary semiring provenance, with
or without monus (m-semiring), in the free m-semiring, or specialized to
any m-semiring of choice. It also supports aggregation using semimodule
provenance aggregation, where-provenance, and probability computation from
the provenance, through a variety of methods.

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
* aggregation on the final query

## Docker container

As an alternative to a ProvSQL installation (see below), you can try
a demonstration version of ProvSQL (full-featured, except for `minic2d`
support) as a Docker container. To deploy it, once Docker CE is
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

1. An install of PostgreSQL >= 9.6. The extension has currently been
   tested with versions from 9.6 to 15 (inclusive) of PostgreSQL, under
   Linux and Mac OS X (if the extension does not work on a specific version
   or operating system, a bug report is appreciated).

2. A compilation environment for PostgreSQL, including the `make` tool, a
   C/C++ compiler (both can be obtained on Debian-based Linux distributions
   from the virtual `build-essential` package), and the headers for your
   PostgreSQL version (as can be obtained for instance from the
   `postgresql-server-dev-xx` package on Debian-based systems, or from
   the `postgresql` package on the Homebrew package manager for Mac OS X).
   The C++ compiler should support C++ 2017.

3. The `uuid-ossp` extension for PostgreSQL (on Debian-based
   systems, it is found in the `postgresql-contrib-9.x` package for
   PostgreSQL version 9.x, and is installed automatically for PostgreSQL
   version >= 10; on Homebrew, in the `ossp-uuid` package; if you compile
   PostgreSQL from source, make sure to also compile and install the
   additional modules in the `contrib` directory).

4. The Boost container library (on Debian-based systems, it is found in
   the `libboost-dev` package).

5. Optionally, for probability computation through knowledge compilation,
   any or all of the following software:
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
`dsharp`, `weightmc`, `graph-easy`) will fail if no executable of that
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
