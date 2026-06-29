.. cs4 is not generated as a Studio notebook (unlike the other case
   studies): its setup loads external CSV files via psql's \copy,
   which a self-contained .ipynb cannot ship. Like cs3 (external GTFS
   download). The remaining nb: comment markers below are inert
   without an nb:name and are kept in case the data is ever inlined.

Case Study: Government Ministers Over Time
==========================================

This case study, introduced in :cite:`DBLP:conf/pw/WidiaatmajaDDS25`,
applies ProvSQL's temporal extension to a database of French and
Singaporean government ministers. Every fact carries the *validity
interval* during which it was true, and we explore that history through
:ref:`Studio's Temporal mode <studio-temporal-mode>` -- a validity
timeline with as-of, during, and full-history time operations -- with
the underlying SQL shown alongside each step.

.. note::

   The data was imported semi-automatically from
   `Wikidata <https://www.wikidata.org>`_ and may contain imprecisions.
   It was current as of early 2026 and does not reflect subsequent political
   appointments.

The scenario
------------

A database records which person held which governmental position and when.
Each row has a ``validity`` column of type ``tstzmultirange`` that describes
the time intervals during which the fact was true. We will:

* reconstruct the full history of a person's positions,
* snapshot the government as it stood on a given date,
* list everyone who served during a window of time, and
* fire an official and then undo that action.

Setup
-----

.. nb:skip
.. tip::

   **Prefer not to install? Use the Playground.** Instead of the manual setup
   below, open the `cs4 database in the ProvSQL Playground
   <https://provsql.org/playground/?db=cs4&mode=temporal>`_; it ships this case
   study's data pre-loaded and opens straight in Temporal mode, ready to follow
   along as you read. The Playground bundles no external tools, but Temporal
   mode needs none. See the :ref:`Playground note <playground-note>`.

.. nb:omit-begin

This case study assumes a working ProvSQL installation on PostgreSQL 14 or
later (see :doc:`getting-provsql`).  The data files are included in the
ProvSQL source distribution under ``doc/casestudy4/data/``.  Run the setup
script from that directory:

.. code-block:: bash

    cd /path/to/provsql/doc/casestudy4/data
    psql -d mydb -f ../setup.sql

.. nb:omit-end

.. nb:setup: ../../casestudy4/setup.sql

This creates three tables -- ``person`` (politicians, with a
``validity tstzmultirange`` column), ``holds`` (which person held which
position in which country, again with a ``validity``), and ``party``
(party memberships) -- then:

* calls :sqlfunc:`add_provenance` on ``person`` and ``holds``,
* creates ``person_validity`` and ``holds_validity`` mapping views via
  :sqlfunc:`create_provenance_mapping_view`, and
* extends ProvSQL's ``time_validity_view`` to incorporate both.

The convenience view ``person_position`` joins ``person`` and ``holds``
for French officials; it is the relation we place on the timeline below.

Opening Temporal mode
---------------------

Point Studio at the database and pick **Temporal** from the mode switcher
(see :ref:`the mode reference <studio-temporal-mode>` for the full UI):

.. code-block:: bash

    provsql-studio --dsn "dbname=mydb" --search-path "public, provsql"

Two controls drive every view that follows:

* **Source** -- a tracked **Relation** (such as the ``person_position``
  view) or an arbitrary **Query** typed into the query box.
* **Time operation** -- :guilabel:`Full` (every row, full validity),
  :guilabel:`As of` (an instant), or :guilabel:`During` (a window).

Each row is wrapped with :sqlfunc:`sr_temporal` over a **validity
mapping**; leave it at the canonical ``provsql.time_validity_view`` that
``setup.sql`` maintains. All instants are handled at UTC.

The full history of a person (Full)
-----------------------------------

With the **Query** source and the :guilabel:`Full` operation, ask for
every position Jacques Chirac has held:

.. code-block:: postgresql

    SELECT position FROM person_position WHERE name = 'Jacques Chirac';

.. figure:: /_static/casestudy4/cs4-full-chirac.png
   :alt: Temporal mode, Full: four lanes for Jacques Chirac's positions
         (Minister Delegate, Agriculture, Interior, Prime Minister), the
         Prime Minister lane carrying two separate bars for his 1974-1976
         and 1986-1988 terms.

   `Chirac's <https://en.wikipedia.org/wiki/Jacques_Chirac>`_ career, the
   lanes ordered :guilabel:`by start`; the Prime Minister lane carries two
   disjoint bars -- his 1974-1976 term and the
   `1986-1988 cohabitation <https://en.wikipedia.org/wiki/Cohabitation_(government)>`_.

Each position becomes a lane, drawn over the full span of time it was
held; setting the :guilabel:`Order` control to :guilabel:`by start` lists
them in the order he took them up -- Minister Delegate, Agriculture, the
Interior, then Prime Minister. Where a post was held in separate spells --
Prime Minister in 1974-1976 and again during the 1986-1988 cohabitation --
the lane shows two disjoint bars: the timeline is drawing the *union of
validity intervals* that :sqlfunc:`sr_temporal` computes over each row's
provenance circuit. The equivalent SQL, which Temporal mode runs for
you, is:

.. code-block:: postgresql

    SELECT position,
           sr_temporal(provenance(), 'time_validity_view') AS valid
    FROM person JOIN holds ON person.id = holds.id
    WHERE name = 'Jacques Chirac'
    GROUP BY position
    ORDER BY valid;

A snapshot in time (As of)
--------------------------

So far we typed SQL into the **Query** source; the **Relation** source is
quicker when you just want a tracked table or view on the timeline. Pick
``person_position`` from the :guilabel:`Relation` selector, switch the
operation to :guilabel:`As of`, and set the instant to ``1981-07-01``:

.. figure:: /_static/casestudy4/cs4-asof-1981.png
   :alt: Temporal mode, As of 1981-07-01, over the person_position
         relation: ten lanes valid at the playhead -- the Mauroy
         government, with Pierre Mauroy as Prime Minister and Robert
         Badinter at Justice.

   The government in place just after the
   `Socialist victory of June 1981 <https://en.wikipedia.org/wiki/1981_French_legislative_election>`_:
   `Pierre Mauroy <https://en.wikipedia.org/wiki/Pierre_Mauroy>`_ as Prime
   Minister, `Robert Badinter <https://en.wikipedia.org/wiki/Robert_Badinter>`_
   at Justice. Drag the playhead to travel through time.

Only the rows valid at the playhead stay on the timeline -- the Mauroy
cabinet installed after the Socialist victory, among them Mauroy (Prime
Minister), Badinter (Justice),
`Jacques Delors <https://en.wikipedia.org/wiki/Jacques_Delors>`_ (Economy)
and `Jack Lang <https://en.wikipedia.org/wiki/Jack_Lang_(French_politician)>`_
(Culture). Drag the **playhead** (or click the axis) to scrub through
time and watch the government change. The SQL equivalent is
:sqlfunc:`timetravel`:

.. code-block:: postgresql

    SELECT name, position FROM
      timetravel('person_position', '1981-07-01')
      AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    ORDER BY position;

.. note::

   Under :guilabel:`As of` (and :guilabel:`During`) the result table on
   the right shows the *full* query, while the timeline shows just the
   rows valid at the chosen time -- so the tuple count there can exceed
   the number of lanes.

A window of time (During)
-------------------------

Switch to :guilabel:`During` and set the window to Macron's first term,
``2017-05-16`` to ``2022-05-13``, over the Prime Ministers:

.. code-block:: postgresql

    SELECT name, position FROM person_position
    WHERE position = 'Prime Minister of France';

.. figure:: /_static/casestudy4/cs4-during-macron.png
   :alt: Temporal mode, During Macron's first term: Edouard Philippe and
         Jean Castex framed within the dimmed window.

   The two Prime Ministers who served during
   `Macron's first term <https://en.wikipedia.org/wiki/Presidency_of_Emmanuel_Macron>`_:
   Édouard Philippe, then Jean Castex.

Every row whose validity *meets* the window appears, with the in-window
portion of each bar at full strength and the rest dimmed. The bars are
**not** clipped to the window -- you still see each full term -- matching
the semantics of :sqlfunc:`timeslice`:

.. code-block:: postgresql

    SELECT name, validity FROM
      timeslice('person_position', '2017-05-16', '2022-05-13')
      AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    WHERE position = 'Prime Minister of France'
    ORDER BY validity;

The full history of a role (Full)
---------------------------------

Keep the **Query** source on :guilabel:`Full` and list every holder of a
role to read its succession as a stack of lanes:

.. code-block:: postgresql

    SELECT name FROM person_position
    WHERE position = 'Minister of Justice';

Set the :guilabel:`Order` control to :guilabel:`by start` to read the
holders in chronological order -- the natural way to follow a role through
time -- without adding any sort to the query.

This is the timeline form of :sqlfunc:`history`, which returns all
versions of rows matching a set of column filters:

.. code-block:: postgresql

    SELECT name, validity FROM
      history('person_position', ARRAY['position'], ARRAY['Minister of Justice'])
      AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
    ORDER BY validity;

Beyond the timeline: editing history
------------------------------------

Temporal mode is read-only. To *change* the data -- and have ProvSQL
track the change -- drop to SQL. These steps require
``provsql.update_provenance``:

.. code-block:: postgresql

    SET provsql.update_provenance = on;

In Studio you can instead flip the :guilabel:`update_provenance` toggle
beside the query box, which sets the same GUC for the session.

**Replace the Prime Minister.** ProvSQL intercepts every DML statement
and records it in ``update_provenance``. Note who currently holds the
position, then dismiss them and appoint a placeholder:

.. code-block:: postgresql

    CREATE TEMP TABLE fired_pm AS
      SELECT person.id, name FROM person
      JOIN holds ON person.id = holds.id
      WHERE position = 'Prime Minister of France'
        AND holds.validity @> now()::timestamptz;

    DELETE FROM holds
    WHERE position = 'Prime Minister of France'
      AND holds.validity @> now()::timestamptz;

    INSERT INTO person (id, name, gender)
      VALUES (100000, 'Jeanne Dupont', 'female');
    INSERT INTO holds (id, position, country)
      VALUES (100000, 'Prime Minister of France', 'FR');

Put the Prime Ministers back on the timeline -- the **Query** source with
:guilabel:`As of` set to the present:

.. code-block:: postgresql

    SELECT name FROM person_position WHERE position = 'Prime Minister of France';

Jeanne Dupont is now the current holder. The dismissed minister's own
Prime Minister bar has gained a finite upper bound at the deletion
instant, which you can read off directly:

.. code-block:: postgresql

    SELECT position,
           sr_temporal(provenance(), 'time_validity_view') AS valid
    FROM person JOIN holds ON person.id = holds.id
    JOIN fired_pm ON person.id = fired_pm.id
    GROUP BY position;

**Undo.** The ``update_provenance`` table records every DML query with
its provenance token; :sqlfunc:`undo` reverses any recorded operation:

.. code-block:: postgresql

    SELECT undo(provenance()) FROM update_provenance;

Running the same Prime-Minister query again confirms the original holder
is back, their Prime Minister interval open-ended once more.

.. note::

   :sqlfunc:`undo` reverses each recorded operation independently. The
   ``update_provenance`` table persists across sessions; clear it with
   ``DELETE FROM update_provenance`` when it is no longer needed.
