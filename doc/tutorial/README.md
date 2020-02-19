Who killed Daphine? A ProvSQL tutorial
======================================

A group of 20 persons spent the night in a manor. In the morning,
Daphine, a young lady, went missing. Her dead body was found the
following day in the cellar. The autopsy revealed the following facts:

*   She died of a head injury caused by a blunt force instrument.
*   Her death happened between midnight and 8am.
*   She was not killed in the cellar, but her body was moved there after
    her death.
*   Goose down found in her wound proves that she died in bed, in one of
    the bedrooms of the manor; unfortunately, all beds have the same down
    pillows, and pillowcases were changed in the morning, so it is
    impossible to identify which bedroom.

The police interviewed all 19 suspects, and collected their statements
about who they saw in which room at which time. They also assessed their
reliability as witnesses, based on a psychological evaluation.

Help the police discover who killed Daphine, using the power of
provenance management and probabilistic databases! ProvSQL will help you
do that.

A step-by-step solution is provided in the [solution.sql](solution.sql)
file. We recommend not checking this file unless you are stuck.


Setup
-----

This tutorial assumes you have access to a working installation of
ProvSQL. See [the
documentation](https://github.com/PierreSenellart/provsql/) for
installation instructions.

Load the [setup.sql](setup.sql) SQL script within a fresh PostgreSQL
database to set it up with all the data required; see, for instance, the
[create.sh](create.sh) script for an example of how to do this.

Solving the murder
------------------

1.   Familiarize yourself with the content of the database. Four tables
     are provided: information about persons, rooms, sightings of a
     person in a room by a witness, and reliability scores of witnesses.
     As a reminder, `\d` can be used in the PostgreSQL client
     to show the list of currently defined tables, and `\d tablename` to check
     the schema of the table `tablename`.

1.   In order to be able to call all ProvSQL functions without prefixing
     them with the `provsql.` prefix, type the following command:
     ```sql
     SET search_path TO public, provsql;
     ```
     This should be done at the start of every new session of the
     PostgreSQL client.

1.   Design a query that retrieves for every sighting: the time of the
     sighting, the name of the person seen, the name of the witness, the
     name of the room. Store the content in a table `s` (with `CREATE
     TABLE s AS ...`).

1.   We will now activate the support of provenance for this table `s`;
     by default, ProvSQL does not do anything unless a table has
     provenance enabled. To do this, simply type:
     ```sql
     SELECT add_provenance('s');
     ```
     Display the content of the table `s`; the `provsql` column has been
     added and contains a provenance token, that references a gate in a
     provenance circuit. Feel free to run some basic queries on the table
     `s`: query results will have provenance annotations (for now, they
     appear as a unique identifier). It is not possible to directly refer
     to the `provsql` column, which acts in a “magical” way. But it is
     possible to obtain the provenance token in a query by using the
     `provenance()` user-defined function.

     We will also create a *provenance mapping*: a mapping maps provenance
     tokens to actual values in a semiring. The first such mapping we
     will create use the name of the witness as a value for the
     provenance token. The provenance mapping is created with:
     ```sql
     SELECT create_provenance_mapping('witness_mapping','s','witness');
     ```
     assuming the column `witness` of the table `s` contains the name of
     the witness. A mapping is stored in the database as a simple table,
     as you can check.

1.   Right now, there are contradictions in the sightings, because
     witnesses cannot be fully trusted: the same person may be
     sighted at the same time in different rooms, which of course cannot
     be correct. Run a query that identifies all such contradictions.

1.   Add in the `SELECT` clause of the previous query a term
     `formula(provenance(),'witness_mapping')`. This has for effect to
     evaluate the provenance annotation of each tuple in a semiring that
     displays a formula for the provenance, using `witness_mapping` to
     map initial provenance tokens to actual names.

1.   We now want to store in a new table `consistent_s` all sightings of
     a person at a time in a given room *except* those that are
     inconsistent (i.e., which have been identified as contradictions by
     the previous query). Create such a table with a SQL query, and
     display its content along with the provenance formula for each
     tuple. Attention: at the moment, ProvSQL does not support the `NOT IN`
     SQL keyword; the only way to express negation at the moment is through
     the `EXCEPT` keyword.

1.   Create a `suspects` table containing all distinct persons that were
     sighted (in a consistent sighting) during the time the murder was committed in
     one of the room the murder may have been committed. Display the
     content of the `suspects` table along with the provenance formula
     for each tuple.

1.   We now want to know how many sightings confirmed that a person was a
     suspect. To do that, we will use the counting m-semiring. First, add a
     column to the table `s` that contains an integer. Set this integer
     to `1` for all tuples, and create a provenance mapping
     `count_mapping` that references this column: this is just so that
     each tuple of the `s` table counts for one individual sighting when
     collecting the counts. Now, using the `counting` function instead of
     the `formula` function, and `count_mapping` instead of
     `witness_mapping`, display the number of sightings confirming that a
     person was a suspect. This number may be 0 in case of inconsistent
     sightings! Who had the most sightings?

1.   The number of confirmed sightings is nowhere enough to conclude. We
     will use the reliability score of each witness to help us identify
     the murderer. Similarly as in the previous question, add to the `s`
     table a `reliability` column to store floating-point reliability
     scores; set the reliability score of each sighting to that of the
     witness of the sighting. Create a provenance mapping mapping
     provenance tokens to this reliability score.

1.   Let us know find the murderer! The police needs a confidence of at
     least 0.99 before arresting the suspect. To compute the probability
     of a query answer, we can use the `probability_evaluate` function, which
     takes the following parameters:

     *   the provenance token, obtained in ProvSQL by `provenance()`
     *   the name of the provenance mapping to probabilities   
     *   the probability computation method: it can be set to `'monte-carlo'`
         (for Monte Carlo approximation), to `'possible-worlds'` (for
         exhaustive enumeration of possible worlds), or to
        `'compilation'` (for compilation of the provenance circuit to a
         d-DNNF)
     *   an optional fourth argument: for Monte Carlo sampling, it is the
         number of samples (as a character string); for enumeration of possible worlds, it does not
         need to be set; for knowledge compilation, it is the name of the
         knowledge compilation tool used (one of `'c2d'`, `'d4'`, or `'dsharp'`)
