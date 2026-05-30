ProvSQL Playground (browser build)
==================================

**ProvSQL Playground** is a second distribution target for Studio: the
same UI, plus PostgreSQL and the ProvSQL extension, all compiled to
WebAssembly and running **entirely client-side** in the browser, with no
server and no database connection. It is a zero-install demo on the
tutorial and case-study databases; real users still install ProvSQL
locally (see :doc:`../user/getting-provsql`).

It lives under ``studio/web/`` and is published as static files at
`provsql.org/playground/ <https://provsql.org/playground/>`_. The
authoritative, file-by-file reference is ``studio/web/README.md``; this
chapter is the architectural overview.

Design goal: no parallel port
-----------------------------

The whole point is to run the **unmodified** ``provsql_studio`` Python
(``app.py``, ``db.py``, ``circuit.py``, ``kc.py``) and the unmodified
``static/`` frontend, so Studio feature changes flow through with no
re-implementation to maintain. The browser-specific code is small and
stable:

* a fake ``psycopg`` / ``psycopg_pool`` module (``psycopg_pglite.py``)
  backed by an in-page PGlite;
* a fake ``subprocess`` (in the same file) that routes ``dot`` to a WASM
  Graphviz;
* a ``fetch`` → Flask ``test_client`` bridge and the boot sequence
  (``studio-boot.js``).

Architecture
------------

.. code-block:: text

   static/app.js ─fetch('/api/*')→ window.fetch override (JS)
                                    │  enters Python via PyProxy.callPromising()
                                    ▼
      Pyodide:  app.py (Flask app.test_client) → db.py → fake psycopg
                                    │  cursor.execute → run_sync(pg.query(...))
                                    ▼
                          PGlite + provsql  (WASM, this tab)

* **PGlite** (``@electric-sql/pglite``) is PostgreSQL 17 compiled to
  WebAssembly: a single backend, single connection, no postmaster and no
  background workers. ProvSQL is loaded into it as a normal extension
  bundle (``provsql.tar.gz``: the ``.so`` side module + control + SQL).
* The extension is built with the in-process store flags
  (``PROVSQL_INPROCESS_STORE`` / ``PROVSQL_NO_SUBPROCESS``, automatic
  under ``__EMSCRIPTEN__``): no shared memory, no background worker, no
  ``fork``/``exec``/sockets. The planner hook installs at
  ``CREATE EXTENSION`` rather than via ``shared_preload_libraries``
  (PGlite cannot preload). See :doc:`memory` for the store.
* **Pyodide** (CPython → WASM) runs the unmodified Studio Python. Flask
  and sqlparse are installed by ``micropip`` from a vendored wheel
  closure.
* **JSPI** (WebAssembly JavaScript Promise Integration) bridges the
  synchronous ``db.py`` to the asynchronous PGlite: the shim's
  ``cursor.execute`` does ``run_sync(pg.query(...))``, and the fetch
  bridge enters Python via ``PyProxy.callPromising()``. Backend calls are
  serialised, because the whole app shares one PGlite connection while
  the Flask code assumes a private connection per request.
* **Graphviz** (``@hpcc-js/wasm-graphviz``) replaces the ``dot``
  subprocess the circuit/tree-decomposition renderers shell out to.
* External knowledge compilers (d4, c2d, weightmc, ...) cannot run (no
  subprocesses), so the tool registry is disabled; probability uses the
  in-process tree-decomposition compiler.

Databases
---------

One IndexedDB-persisted PGlite cluster holds a database per tutorial and
case study (``tutorial``, ``cs1``, ``cs2``, ``cs4``-``cs7``; ``cs3`` is
omitted as it needs a large external GTFS download), switchable from the
connection chip. ``build-casestudies.py`` derives them from the canonical
``doc/{tutorial,casestudyN}/setup.sql`` scripts, rewriting the psql-only
``COPY ... FROM stdin`` / ``\copy`` constructs into ``INSERT`` s and
splitting each script into individual statements (PGlite runs a whole
``exec()`` as one transaction). A **Reset** button drops and re-seeds
them.

Self-hosted and path-portable
-----------------------------

The build loads **nothing from a CDN at run time**: ``vendor.sh`` fetches
Pyodide, the wheels, Graphviz and Font Awesome into the doc-root at build
time, and ``build.sh`` rewrites the few root-absolute paths in the copied
``app.js`` to relative ones. The result is a pure static bundle that runs
unchanged at a server root or under a sub-path (``/playground/``), needs
no rewrite rules, and works over ``file://``. A small ``index.html``
landing page gates on JSPI (browser support, the Firefox flag) and links
to the app (``app.html``); shared deep links
(``?mode=`` / ``?db=`` / ``?q=``) forward straight to the app.

Build, test, deploy
-------------------

* **Build** the doc-root with ``studio/web/build.sh`` (it needs the WASM
  artifacts from ``wasm/``: the matched PGlite dist and
  ``provsql.tar.gz``). ``make playground`` reuses the in-place artifacts;
  the first build passes ``--pglite``/``--provsql``.
* **Test**: ``studio/tests/web/`` is a headless-Chromium Playwright suite
  (JSPI is on by default in current Chromium) driving the real frontend +
  Python backend against the in-page PGlite. It covers boot, the
  query → circuit → semiring path, the ``/api`` surface, database
  switching, Reset, deep links, sub-path portability, and a fully
  off-line boot. It is wired into the ``wasm`` CI job
  (``.github/workflows/wasm.yml``).
* **Deploy** with ``make deploy-playground`` (rsync to
  ``provsql.org/playground/``). The only server requirement is the
  ``application/wasm`` MIME type, supplied by the shipped ``.htaccess``.

Browser support
---------------

The Playground requires a browser with WebAssembly JSPI. The landing
page (``studio/web/landing.html``) is the single maintained source of
truth for current browser support (which versions, and the Firefox
flag); it also feature-detects JSPI at load. Keep that list there only,
since it drifts as browsers ship JSPI.

The full WASM build recipe (the Emscripten toolchain, the matched PGlite
core, the libc++ ``inline`` patch) is documented in ``wasm/README.md``.
