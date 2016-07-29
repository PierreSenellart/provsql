# provsql

The goal of the provsql project is to add support for semiring provenance
and uncertainty management to PostgreSQL databases, in the form of a
PostgreSQL extension/module/plugin. It is work in progress at the moment.

## Features

The provsql system currently support proper management of provenance
attached to SQL queries, in the form of a provenance circuit, suitable
both for regular Boolean provenance and arbitrary semiring provenance (in
the universal semiring, or specialized to any semiring of choice). It
also aims at supporting probability computation from the provenance
circuit, though this is not implemented at the moment.

The following SQL queries are currently supported. At the moment, they
are all *monotone* queries.
* Regular SELECT-FROM-WHERE queries (aka conjunctive queries with
  multiset semantics)
* JOIN queries (regular joins and outer joins; semijoins and antijoins
  are not currently supported)
* SELECT queries with nested SELECT subqueries in the FROM clause
* GROUP BY queries (without aggregation)
* SELECT DISTINCT queries (i.e., set semantics)
* UNION's or UNION ALL's of SELECT queries

## Prerequisites

1. A running install of PostgreSQL. The extension has currently been
   tested with version 9.5.3 of PostgreSQL, though it should work with
   slightly older versions (if it does not, a bug report is appreciated).

2. A compilation environment for PostgreSQL, including the `make` tool, a
   C compiler (both can be obtained on Debian-based Linux distributions
   from the virtual `build-essential` package), and the headers for your
   PostgreSQL version (as can be obtained for instance from the
   `postgresql-server-dev-9.5`  package).

3. Finally, the `uuid-ossp` extension for PostgreSQL. On Debian-based
   systems, it is found in the `postgresql-contrib-9.5` package.

## Installation

1. Compile the code with `make`.

2. Install it in the PostgreSQL extensions directory with `make install`
   (run as a user with rights to write the PostgreSQL installation
   directories)

3. Add the line 
   ```
   shared_preload_libraries = 'provsql'
   ```
   to the postgresql.conf configuration file (required because the
   extension include *hooks*) and restart the PostgreSQL server (e.g.,
   with service postgresql restart on systemd-based distributions)

## Testing your installation

You can test your installation by running `make installcheck` as the
PostgreSQL administrator user.

## Using provsql

You can use provsql from any PostgreSQL extension by loading the
`provsql` extension. See the file [setup.sql](test/sql/setup.sql)
for an example on how to do this.

You then need to add provenance to an existing table using the
`provsql.add_provenance` method.
See [add_provenance.sql](test/sql/add_provenance.sql) for an example.
The table will have an extra `provsql` column added. This column
is handled in a special way and always represent, in query results, the
provenance of each tuple as a UUID.

You can then use this provenance to run computation in various semirings.
See [security.sql](test/sql/security.sql) and
[formula.sql](test/sql/formula.sql) for two examples.
