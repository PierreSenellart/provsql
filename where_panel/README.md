# WHERE PANEL

The Where Panel module is a Web App written in PHP to display properly
where provenance information computed with the ProvSQL extension for 
PostgreSQL.

## Features

Enter SQL queries directly in your browser and see immediately from where 
the result of each cell come from.

All queries supported with ProvSQL work also with this App.

## Prerequisites

1. Make sure you have a database with ProvSQL extension enabled.
For any question, refer to the main README of the project.

2. A Web Server supporting PHP with the lib pgsql enabled.
We recommend the use of `LAMP` (or `XAMP` or `MAMP` depending on your OS).

## Installation

Put the files of the Where Panel in the appropriate directory of your Web 
Server: for example `/var/www/html` with `LAMP`. 

Enter proper information in the config file and, if needed, check if your 
PostgreSQL server is correctly configured in `/etc/postgresql/X.X/main/pg\_hba.conf`.
Where X.X is the version of the server currently running.

## Testing your installation

Create a new database and run [setup.sql](test/sql/setup.sql), [add\_provenance.sql](test/sql/add\_provenance.sql), and [security.sql](test/sql/security.sql).
Connect to this database and choose predefined queries or test with your own.

## Using the Panel

## Uninstalling

## License

## Contact
