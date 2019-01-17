# ProvSQL

[![Build Status](https://travis-ci.org/PierreSenellart/provsql.svg?branch=master)](https://travis-ci.org/PierreSenellart/provsql)

The goal of the ProvSQL project is to add support for (m-)semiring provenance
and uncertainty management to PostgreSQL databases, in the form of a
PostgreSQL extension/module/plugin. It is work in progress at the moment.

## Features

The ProvSQL system currently supports proper management of provenance
attached to SQL queries, in the form of a provenance circuit, suitable
both for regular Boolean provenance, arbitrary semiring provenance, with
or without monus (m-semiring), in the free m-semiring, or specialized to
any m-semiring of choice. It also supports where-provenance and
probability computation from the provenance, through a variety of
methods.

The following SQL queries are currently supported.
* Regular SELECT-FROM-WHERE queries (aka conjunctive queries with
  multiset semantics)
* JOIN queries (regular joins only; outer, semijoins, and antijoins
  are not currently supported)
* SELECT queries with nested SELECT subqueries in the FROM clause
* GROUP BY queries (without aggregation)
* SELECT DISTINCT queries (i.e., set semantics)
* UNION's or UNION ALL's of SELECT queries
* EXCEPT of SELECT queries

## Docker container

As an alternative to a ProvSQL installation (see below), you can try
a demonstration version of ProvSQL (full-featured, except for circuit
visualization) as a Docker container. To deploy it, once Docker CE is
installed, simply run:
```
docker run inriavalda/provsqldemo
```
By following the instructions, you will be able to connect to the
PostgreSQL server within the container using a PostgreSQL client,
and to use a Web interface for simple visualization of where-provenance.

## Prerequisites for installation

1. An install of PostgreSQL >= 9.5. The extension has currently been
   tested with versions from 9.5 to 11.1 (inclusive) of PostgreSQL, under
   Linux and Mac OS X (if the extension does not work on a specific version
   or operating system, a bug report is appreciated).

2. A compilation environment for PostgreSQL, including the `make` tool, a
   C compiler (both can be obtained on Debian-based Linux distributions
   from the virtual `build-essential` package), and the headers for your
   PostgreSQL version (as can be obtained for instance from the
   `postgresql-server-dev-xx` package on Debian-based systems, or from
   the `postgresql` package on the Homebrew package manager for Mac OS X).

3. Finally, the `uuid-ossp` extension for PostgreSQL (on Debian-based
   systems, it is found in the `postgresql-contrib-9.x` package for
   PostgreSQL version 9.x, and is installed automatically for PostgreSQL
   version >= 10; on Homebrew, in the `ossp-uuid` package).

4. Optionally, for probability computation, any or all of the following
   software:

   * `c2d`, from http://reasoning.cs.ucla.edu/c2d/download.php

   * `d4`, from http://www.cril.univ-artois.fr/KC/d4.html

   * `dsharp`, from https://bitbucket.org/haz/dsharp

   To be used, an executable with the name of this software must be
   available in the PATH of the PostgreSQL server user (e.g., in
   `/usr/local/bin/`).

5. Optionally, for circuit visualization, the following software:

   * `graphviz`, for production of PDF circuits (`dot` executable)
   
   * `evince`, for visualization of PDF files
   
   Both can be obtained as packages in common Linux distributions.

## Installation

1. Compile the code with `make`. If you have several installed versions
   of PostgreSQL, you can change the version the module is compiled
   against by changing the reference to `pg_config` in the Makefile.

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

You can test your installation by running `make installcheck` as a
PostgreSQL administrator user. If you do not want to run this as the
default administrator user, you can make yourself a PostgreSQL
administrator with ``ALTER USER your_login WITH SUPERUSER``. This assumes that
``your_login`` is a PostgreSQL user: on Debian-based Linux distributions, you
can ensure this by running the command ``createuser your_login`` as the
``postgres`` user.

Note that the tests that depend on external software (`c2d`, `d4`, 
`dsharp`, `dot`) will fail if no executable of that name can be found.

For circuit visualization, the database server will attempt to launch `evince`
on a local X window server (`:0`). You can authorize the display of such windows
with `xhost +`.

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

A demonstration of the ProvSQL system is available as a video, on
https://youtu.be/iqzSNfGHbEE?vq=hd1080

An article describing this demonstration, presented at the VLDB 2018
conference, is available at
http://pierre.senellart.com/publications/senellart2018provsql.pdf

## Uninstalling

You can uninstall ProvSQL by running `make uninstall` (run as a user with
rights to write to the PostgreSQL installation directories).

## License

ProvSQL is provided as open-source software under the MIT License. See [LICENSE](LICENSE).

## Contact

https://github.com/PierreSenellart/provsql

Pierre Senellart <pierre@senellart.com>

Bug reports and feature requests are
preferably sent through the *Issues* feature of GitHub.
