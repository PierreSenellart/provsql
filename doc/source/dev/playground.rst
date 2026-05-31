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
* a ``fetch`` → Flask ``test_client`` bridge, split across the shell /
  iframe boot pair (``shell-boot.js`` / ``child-boot.js``, below).

Shell + iframe (keeping the backend warm)
-----------------------------------------

The backend (PGlite + Pyodide) is expensive to instantiate, and the
unmodified frontend reloads the page to switch mode or database. So the
page is two same-origin documents: a **shell** (``app.html``) that owns the
warm backend and never reloads, and an **iframe** (``ui.html``) that runs
the unmodified Studio UI. The UI's ``/api/*`` fetches are forwarded to the
shell over ``postMessage``. A **mode switch** then reloads only the iframe
(≈140 KB of JS) and a **database switch** reopens just PGlite (the shell
handles ``POST /api/conn`` in place), leaving Pyodide and Flask live across
both. JSPI runs only in the shell (the top frame); the iframe needs none.
Each iframe load tags its messages with an epoch, so a reply that straddles
a reload cannot resolve the wrong request in the fresh child.

Architecture
------------

.. code-block:: text

   ui.html app.js ─fetch('/api/*')→ child-boot bridge ─postMessage→ shell
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
  ``cursor.execute`` does ``run_sync(pg.query(...))``, and the shell
  enters Python via ``PyProxy.callPromising()``. Backend calls are
  serialised on one chain, because the whole app shares one PGlite
  connection while the Flask code assumes a private one per request;
  ``switchDb`` and Reset run on that same chain.
* **Graphviz** (``@hpcc-js/wasm-graphviz``) replaces the ``dot``
  subprocess the circuit/tree-decomposition renderers shell out to.
* External knowledge compilers (d4, c2d, weightmc…) cannot run (no
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
``app.js`` to relative ones. The boot modules resolve sibling assets
against their own module URL and the shell mounts ``ui.html`` by a relative
URL, so the result is a pure static bundle that runs unchanged at a server
root or under a sub-path (``/playground/``), needs no rewrite rules, and
works over ``file://``. A small ``index.html`` landing page gates on JSPI
(browser support, the Firefox flag) and links to the shell (``app.html``);
shared deep links (``?mode=`` / ``?db=`` / ``?q=``) forward straight to it.

Build, test, deploy
-------------------

* **Build the WASM artifacts**: ``make wasm`` reproduces the ``wasm`` CI
  job locally (``wasm/build-wasm.sh``): it builds the matched PGlite core +
  the ProvSQL extension against the Emscripten builder image (podman or
  docker), runs the headless Node smoke test, and assembles the doc-root
  from the freshly built artifacts. The (slow) WASM Postgres core build runs
  only once -- it is skipped when ``wasm/.build`` already has it; pass
  ``WASM_REBUILD_CORE=1`` to force a clean core rebuild. Iterating on the
  extension therefore re-runs only the extension compile + relink.
* **Assemble** the doc-root with ``studio/web/build.sh`` (it needs the WASM
  artifacts from ``wasm/``: the matched PGlite dist and
  ``provsql.tar.gz``). ``make playground`` reuses the in-place artifacts;
  ``make wasm`` and the first build pass ``--pglite``/``--provsql``.
* **Test**: ``make playground-test`` runs ``studio/tests/web/``, a
  headless-Chromium Playwright suite (JSPI is on by default in current
  Chromium) driving the real frontend + Python backend against the in-page
  PGlite. It covers boot, the query → circuit → semiring path, the ``/api``
  surface, database switching, Reset, deep links, sub-path portability, and
  a fully off-line boot. The browser build and this e2e run locally only (via
  ``make wasm`` / ``make playground-test``), not in CI; the per-PR
  ``.github/workflows/wasm.yml`` job covers just the cheaper in-process-store
  single-session smoke.
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
