# Client-side ProvSQL in the browser (PostgreSQL + ProvSQL in WebAssembly)

## Intro

This is a feasibility study and code-level implementation plan for
running ProvSQL (and ProvSQL Studio) **entirely client-side in a Web
browser**, with no server, by compiling PostgreSQL + the ProvSQL
extension to WebAssembly through
[Emscripten](https://github.com/emscripten-core/emscripten). The
realistic vehicle is **[PGlite](https://pglite.dev/)**
(`@electric-sql/pglite`): PostgreSQL itself, compiled to WASM with
Emscripten and run in PostgreSQL's *single-user mode* (not a Linux VM),
packaged as a TypeScript library that instantiates Postgres in-process
inside the JS runtime. PGlite already supports dynamically-loaded C
extensions (pgvector, PostGIS, Apache AGE, pg_uuidv7, …) fetched as
`.tar.gz` bundles and loaded via `CREATE EXTENSION`, so it is the
natural host for a WASM ProvSQL.

The headline finding: **the core of ProvSQL ports cleanly, but its
process/IPC architecture does not.** ProvSQL today is built around a
*multi-backend* PostgreSQL: a background worker owns the mmap circuit
store and every backend talks to it over pipes, with circuits shipped
across the pipe via Boost serialization, plus shared memory + LWLocks
for coordination, plus `fork`/`exec` of external knowledge-compiler
binaries. None of that exists in a single-process Emscripten sandbox
(no `fork`, no second process, no `MAP_SHARED` write-back, no sockets,
no `exec`). The good news is that PGlite is single-process and
single-backend *by construction*, which is exactly the precondition
that lets us **collapse the worker/backend split into one in-process
store** and delete most of the offending layer rather than emulate it.

The work happens on branch **`wasm-browser-port`**. The guiding
principle throughout is **maximise the shared code path with the native
build**: every change is gated by a single compile flag and validated on
native *first*, so the WASM build inherits already-green code.

> **Implementation discipline (read before writing any code).** This
> document, its section numbers (§1, §2, …), its "phase 1 / phase 2"
> wording, and its M0–M7 milestones are **planning scaffolding only**.
> They must never leak into the codebase. As you implement: do not cite
> `doc/TODO/*` (this file included) from any comment, commit message,
> source, SQL, or test; and do not name things after the plan's
> sequencing. No `/* phase 1 */`, no `wasm_phase2_*` symbols, no
> "for now / temporary / will be removed in M7" comments. Comments and
> identifiers must be **timeless** — they describe what the code *is* and
> *does*, not when it was added or which step of a plan produced it. A
> compile flag like `PROVSQL_INPROCESS_STORE` is a real, durable name
> describing a build configuration and is fine; `PROVSQL_PHASE1_STORE`
> is not. When this plan says "keep X for phase 1, drop it later", the
> intermediate state still gets a self-explanatory, plan-free comment
> (e.g. "circuit transfer goes through the FIFO so the same code serves
> the native worker path"), never "kept until phase 2".

This document is anchored on a porting-surface audit of `src/` (cited
inline as `file:line`), the PGlite v0.4 architecture and filesystem
docs, and the known Emscripten limitations on `mmap(MAP_SHARED)`,
`fork`/`exec`, and sockets.

## Out of scope

- **External CLI knowledge compilers in-browser** (`d4`, `c2d`,
  `minic2d`, `dsharp`, `weightmc`, `sharpsat-td`, `dpmc`): these are
  separate native binaries invoked via `fork`/`exec`
  (`src/external_tool.cpp:56-102`, `src/BooleanCircuit.cpp` `compilation()`,
  `src/probability_evaluate.cpp:420-551`). Recompiling each to WASM is a
  research project per tool and is *not* attempted here. The plan keeps
  probability working via the **in-process** paths (tree-decomposition
  knowledge compiler + Monte Carlo + safe-query rewriting) and offers an
  optional **remote KCMCP-over-WebSocket** escape hatch (§7) for users
  who want heavy compilation without giving up the client-side model.
- **`graph-easy` ASCII circuit rendering** (`src/DotCircuit.cpp:147-199`):
  Perl, not portable. Studio renders the DOT in-browser with a JS
  Graphviz (`@hpcc-js/wasm` / `d3-graphviz`) already; the server-side
  ASCII path is simply unavailable in the WASM build.
- **Multi-tenant / concurrent-backend semantics**: the browser build is
  single-user single-connection by nature; we do not try to reproduce
  cross-backend coordination. This is not a restriction the port adds —
  it is inherent to PGlite (see Concurrency model below).
- **Persisting to a real `$PGDATA` on a server disk**: persistence is
  PGlite's (IndexedDB / OPFS / in-memory), see §3.

## Target architecture

```
┌─────────────────────────────── Browser tab ───────────────────────────────┐
│                                                                            │
│  Studio UI (static JS/TS, served from any CDN or file://)                  │
│        │  query text, mode toggles                                         │
│        ▼                                                                    │
│  Studio client core (db.ts, circuit.ts)  ← port of studio/db.py+circuit.py │
│        │  pg.query(...)                                                     │
│        ▼                                                                    │
│  PGlite  ──────────────  PostgreSQL (single-user mode, WASM)               │
│        │ CREATE EXTENSION provsql                                          │
│        ▼                                                                    │
│  provsql.wasm  (extension, dlopen'd by PGlite)                             │
│        │  in-process calls (no worker, no pipe, no socket)                 │
│        ▼                                                                    │
│  In-process circuit store (was: bgworker + mmap + Boost-over-pipe)         │
│        │  file writes under $PGDATA/base/<oid>/                            │
│        ▼                                                                    │
│  PGlite VFS  ──flush──►  IndexedDB / OPFS  (persistence)                    │
└────────────────────────────────────────────────────────────────────────────┘
```

The single biggest architectural move is **#define-ing away the
backend/worker boundary**: in PGlite there is exactly one PostgreSQL
process and one backend, so the worker that today owns the
`MMappedCircuit` becomes "the same process", and every backend→worker
pipe message becomes a direct, synchronous, in-process call.

### Concurrency model (why single-backend is free, not a sacrifice)

PGlite runs PostgreSQL in **single-user mode** (no postmaster, no backend
forking) in a single WASM instance built with `-sUSE_PTHREADS=0`. So the
target is single-process, single-connection, single-threaded *by
construction*:

- there is never more than one backend, so two backends can never touch
  the store at once — the in-process store's single-backend assumption is
  satisfied automatically;
- no intra-query parallel workers (they need forked processes; Emscripten
  has none) and no background workers — which is exactly why M1 deletes
  the worker;
- application-level "concurrent" queries (`Promise.all` over
  `pg.query`) are serialized through PGlite's single I/O pathway and run
  one at a time;
- even multi-tab PGlite routes all execution through one leader instance.

Consequently the native pg_regress parallel-group failure under the flag
(M1) is an artifact of **native** multi-process PostgreSQL, a mode that
does not exist under PGlite; native flag testing therefore runs serially
(`--max-connections=1`) at every milestone. M2's per-backend buffer store
does **not** enable concurrency — it makes each backend's store fully
independent, which is correct precisely because the real target never has
a second backend. We lose nothing relative to what PGlite itself can do.

### The central design decision: swap the transport, not the protocol

ProvSQL's backend↔worker layer is a request/response **message
protocol** (one-byte opcode + `MyDatabaseId` + payload), with:

- a **client side** in `src/provsql_mmap.c` — each SQL-callable function
  (`create_gate`, `get_gate_type`, `set_prob`, `get_children`,
  `set_table_info`, …) builds a message with `STARTWRITEM`/`ADDWRITEM`,
  `SENDWRITEM`s it down the pipe, and reads the reply with `READB` /
  raw `read(pipembr,…)`;
- a **server side** — `provsql_mmap_main_loop()` in `src/MMappedCircuit.cpp`
  (`while(READM(c,char)) { switch(c) { case 'C': … } }`,
  `MMappedCircuit.cpp:207`) that consumes a message with `READM` and
  writes the reply with `WRITEM`, dispatching to a process-global
  `std::map<Oid, MMappedCircuit*> circuits` (`MMappedCircuit.cpp:39`).

The naïve port rewrites every client function to call `MMappedCircuit`
methods directly. That is a large, divergent, hard-to-test diff. The
better move, given the browser is **single-process / single-threaded**:

> Keep both the client and server code **unchanged**. Replace the *pipe*
> with an **in-memory byte FIFO**, and run the server's per-message
> dispatch **synchronously, inline, from `SENDWRITEM`**.

Because a backend always writes a *complete* request before reading its
reply, in single-process mode `SENDWRITEM` can: append the buffered
message to the request FIFO, invoke the dispatch for exactly that one
message (which drains the request FIFO and writes the reply to the
response FIFO), and return. The subsequent `READB` just pops from the
response FIFO. The opcode `switch`, the payload framing, the
`MMappedCircuit` methods, and even the Boost-serialized circuit transfer
(`'g'`/`'j'`, `MMappedCircuit.cpp:375,501`) all work **verbatim** over
the FIFO.

This gives (a) almost zero logic change, (b) an in-process build that is
exercisable on native (force the flag on, run `make test`), and (c) the
option to keep Boost for phase 1 and optimise it away later (§6).

A single compile flag selects transport:

```c
/* src/provsql_utils.h (or a new src/provsql_config.h) */
#if defined(__EMSCRIPTEN__) && !defined(PROVSQL_MULTIPROCESS)
#  define PROVSQL_INPROCESS_STORE 1
#endif
/* Forceable on native for testing:
   make PG_CPPFLAGS='-DPROVSQL_INPROCESS_STORE -O0 -g'
   (PG_CPPFLAGS, not CPPFLAGS, so PGXS still adds the server include path) */
```

## Implementation

### 1. In-memory transport (`provsql_shmem.{h,c}`)

**Goal:** under `PROVSQL_INPROCESS_STORE`, no shared memory, no LWLock,
no pipe fds; instead two growable byte FIFOs.

- `provsqlSharedState` (`provsql_shmem.h:74`): under the flag, drop
  `lock` and the four pipe `long`s; add two FIFO handles. Drop
  `kcmcp_endpoint` (the KCMCP path is off by default, §7). Define the
  struct as a plain file-scope global, not a `ShmemInitStruct`
  allocation.

  ```c
  typedef struct { uint8_t *buf; size_t head, tail, cap; } provsql_fifo;
  /* in-process: provsql_fifo req, resp;  (req = backend→server,
                                            resp = server→backend) */
  ```

- `provsql_shmem_startup` / `provsql_shmem_request` / `provsql_memsize`
  (`provsql_shmem.c`): under the flag, make them no-ops (the struct is a
  static global initialised on first use). Do **not** register
  `shmem_request_hook` / `shmem_startup_hook` in `provsql.c:6302-6303`
  under the flag.

- `provsql_shmem_lock_exclusive/shared/unlock`: under the flag, empty
  inline bodies (one process, one backend ⇒ no contention).

### 2. Macro redefinition + dispatch extraction (`provsql_mmap.{h,c}`, `MMappedCircuit.cpp`)

**2a. Redefine the I/O macros** (`provsql_mmap.h:77-90`) under the flag so
the *same* client and server source compiles against the FIFO:

```c
#ifdef PROVSQL_INPROCESS_STORE
  /* server-side consume (was: read from pipe) */
  #define READM(var,type)  provsql_fifo_pop(&pstate.req,  &(var), sizeof(type))
  /* server-side produce (was: write to pipe) */
  #define WRITEM(pvar,type) (provsql_fifo_push(&pstate.resp, (pvar), sizeof(type)), true)
  /* client-side consume reply (was: read from pipe) */
  #define READB(var,type)  provsql_fifo_pop(&pstate.resp, &(var), sizeof(type))
  /* client-side batched send: append message, then run ONE dispatch */
  #define SENDWRITEM()     provsql_inproc_send(buffer, bufferpos)
  #define STARTWRITEM()    (bufferpos=0)
  #define ADDWRITEM(p,t)   (memcpy(buffer+bufferpos,(p),sizeof(t)), bufferpos+=sizeof(t))
#endif
```

`provsql_inproc_send(buf,len)` pushes `len` bytes onto `req` then calls
`provsql_mmap_dispatch_one()` (see 2c). The few raw
`read(provsql_shared_state->pipembr,…)` sites that read variable-length
replies — `get_gate_type` children (`provsql_mmap.c:151`), `get_children`
(`:463`), `get_extra` (`:392`) — must also pop from `resp`. Introduce a
`READB_BYTES(ptr,len)` macro (`read(pipembr,…)` natively, `provsql_fifo_pop`
in-process) and replace those three raw `read` calls with it. Mirror with
`WRITEM_BYTES(ptr,len)` for the server's raw byte writes (the Boost
stringstream flush in the `'g'`/`'j'` arms, `MMappedCircuit.cpp:~378,504`).

`buffer[PIPE_BUF]` (`provsql_mmap.c:49`): in-process the `PIPE_BUF` atomicity
constraint is irrelevant. Make `buffer` a growable heap buffer in-process
so an arbitrarily large message is built before one `SENDWRITEM`.

**2b. Force the single-message `create_gate` path** (`provsql_mmap.c:232-271`):
the `else` branch exists only to keep each pipe `write` ≤ `PIPE_BUF`. The
FIFO has no such limit. Under the flag, always take the contiguous path so
every client op is *exactly one* complete message before any reply read —
the precondition that makes synchronous inline dispatch correct.

**2c. Extract the per-message dispatch** from
`provsql_mmap_main_loop()` (`MMappedCircuit.cpp:207-…`). Refactor:

```cpp
// reads ONE message and writes its reply
static bool provsql_mmap_dispatch_one();    // the body of the while-loop's switch
void provsql_mmap_main_loop() {              // native only
  char c; Oid db_oid;
  while (READM(c, char)) { if(!READM(db_oid,Oid)) break; dispatch(c, db_oid); }
}
```

The `switch` body moves verbatim into `dispatch(c, db_oid)`. Native build:
`main_loop` calls it in a loop reading from the pipe. In-process build:
`provsql_inproc_send` reads the opcode+db_oid from the `req` FIFO and calls
`dispatch` once. The `std::map<Oid,MMappedCircuit*> circuits`
(`MMappedCircuit.cpp:39`) and `get_or_create_circuit(db_oid)` stay
exactly as-is.

**2d. Worker lifecycle** (`provsql_mmap.c:52-88`): under the flag,
`provsql_mmap_worker` / `RegisterProvSQLMMapWorker` are compiled out (or
become no-ops) and **not** called from `_PG_init`. The circuit map is
created lazily by the first `provsql_inproc_send` (call
`initialize_provsql_mmap()` once, guarded by a static bool, or rely on
`get_or_create_circuit`'s lazy `new MMappedCircuit(db_oid)`).
`destroy_provsql_mmap` is wired to a `before_shmem_exit` / `on_proc_exit`
callback so the store `sync()`s on shutdown.

This subsumes what the high-level study called "eliminate the background
worker, the IPC pipe, and the shared memory + LWLocks": the worker
(`provsql_mmap.c:52,65-87`), the two `pipe()` pairs
(`provsql_shmem.c:34-66`), the `ShmemInitStruct` /
`RequestAddinShmemSpace` / `RequestNamedLWLockTranche`
(`provsql_shmem.c:45,82-100`) are all gone in one flag.

### 3. Storage backend: `mmap(MAP_SHARED)` → buffer + explicit write-back

*The crux, and the one true Emscripten wall.* `MMappedVector`
(`src/MMappedVector.{h,hpp}`) and `MMappedUUIDHashTable`
(`src/MMappedUUIDHashTable.{h,cpp}:37-164`) both do
`open()`+`mmap(…,MAP_SHARED,…)`, `grow()` = `munmap`+`ftruncate`+`mmap`,
`sync()` = `msync(MS_SYNC)`. Two Emscripten facts decide the design:

1. **`MAP_SHARED` write-back is not reliably supported** in Emscripten's
   default filesystem; `mmap` asserts/falls back to `MAP_PRIVATE`
   (copy-on-write), so writes through the mapping **do not reach the
   file**. WasmFS has partial non-private mmap support but is not
   something to depend on.
2. **PGlite persists at the *file* level**: IndexedDB-FS loads every
   `$PGDATA` file into memory on start and flushes changed files back
   after each query; OPFS-AHP keeps synchronous file handles. So *if our
   bytes live in real files under `$PGDATA` and we `write()`/`fsync`
   them, PGlite persists them for free.*

**Plan:** introduce one small abstraction, `src/MappedRegion.h`, that both
classes already implicitly use (pointer + length + grow + sync). Two
implementations behind the flag:

- **POSIX (native, unchanged):** today's `mmap(MAP_SHARED)` path,
  byte-for-byte. Zero behavioural change off-WASM.
- **WASM (`PROVSQL_INPROCESS_STORE`):**
  - open the same `$PGDATA/base/<oid>/provsql_*.mmap` file (`O_CREAT|O_RDWR`);
  - on open, `fstat` + `read()` the whole file into a `malloc`'d buffer
    (fresh file → write the 16-byte header first);
  - `data()` returns the heap pointer;
  - `grow(n)` = `realloc` + `ftruncate(file,n)`;
  - `sync()` = `lseek(fd,0)` + `write(fd, buf, len)` + `fsync(fd)` so
    PGlite's post-query VFS flush captures it.

This is `MAP_PRIVATE`-equivalent semantics with **explicit write-back**,
which is all single-process correctness needs. The 16-byte header
(`magic|version|elem_size|reserved`), the four/five file names
(`MMappedCircuit.h:98-102`), `makePath` = `DataDir + "/base/" + oid`
(`MMappedCircuit.cpp:43`), and the on-disk ABI (`GateInformation`,
hash-table slots) are **all preserved** — so `provsql_migrate_mmap` and
`ALTER EXTENSION … UPDATE` are unaffected.

*Tuning (defer until profiled):* whole-region write-back on every `sync()`
is simplest; if slow, track a dirty high-water mark and write only
`[0,hwm)`. MEMFS caps files at 2 GB → document OPFS for large circuits.

### 4. Disable the process-spawning / socket paths (compile-guard)

All of these are guarded by `PROVSQL_NO_SUBPROCESS` (defined in
`provsql_config.h` under `__EMSCRIPTEN__`), made to fail-soft so callers
fall back to the in-process compiler. The guard is tied to the platform
(`__EMSCRIPTEN__`), *not* to `PROVSQL_INPROCESS_STORE`, so a native build —
even one forcing the in-process store for testing — keeps the
subprocess/socket paths and stays a faithful 176/176 regression baseline;
the guarded branches are compile-checked by forcing the flags on native
and exercised for real by the WASM build:

- **External CLI solvers** — `src/external_tool.cpp:56-102`
  (`run_in_own_pgroup`: `fork`/`setpgid`/`execl`/`waitpid`/`killpg`) and
  `find_external_tool` (`:141-186`). Under the flag, `find_external_tool`
  returns "not found" without forking; `run_external_tool` is never
  reached. `ToolRegistry` (`src/ToolRegistry.cpp:51-107`) ships **no**
  `cli`/`kcmcp` tools by default in-process.
- **KCMCP supervisor + client** — `src/kcmcp_supervisor.c` (whole file,
  `fork`/`execl`, `:106-120,236-252`) compiled out; `src/kcmcp_client.cpp`
  (`socket(AF_UNIX/…)`, `connect`, `:46-79`) compiled out. The
  `compilation()` dispatcher in `src/BooleanCircuit.cpp` must treat
  "no kcmcp, no cli" as "use the in-process tree-decomposition compiler".
- **`graph-easy` render** — `src/DotCircuit.cpp:147-199` `render()`
  (`ScopedTempDir`+`exec`) compiled out; DOT *generation* stays. Studio
  renders DOT client-side.
- **WMC forks** — `src/probability_evaluate.cpp:420-551`: the external
  weighted-model-count branch guarded out; the in-process tree-dec / MC /
  safe-query paths remain the only routes.

Net surviving probability surface in-browser: tree-decomposition
knowledge compiler (`dDNNFTreeDecompositionBuilder`, `TreeDecomposition`,
`dDNNF` — the `tdkc` engine, no OS deps), Monte Carlo (`MonteCarloSampler`),
safe-query rewriting, analytic continuous-RV evaluators
(`AnalyticEvaluator`, `Expectation`, `HybridEvaluator`), Shapley/Banzhaf.
All pure C++. Only externally-compiled exact `#SAT` (d4/c2d/…) is absent,
with the well-understood trade-off (cf. `bounded-treewidth-data.md`: d4
only overtakes the in-process compiler around treewidth ≈ 8–9, so the
in-process path covers the common case). No `std::thread`/`pthread`
anywhere in ProvSQL — one fewer WASM headache.

### 5. Phase-1 validation on native (the safety net)

Before any Emscripten work, prove the in-process store is behaviourally
identical to the worker-based one **on native**:

1. Add a build variant: `make PG_CPPFLAGS='-DPROVSQL_INPROCESS_STORE
   -O0 -g'` (`PG_CPPFLAGS`, not `CPPFLAGS`, so PGXS keeps the server
   include path). It compiles the same extension with the FIFO
   transport, no bgworker, no shmem, and the buffer-backed storage
   backend. Run pg_regress with `--max-connections=1` (see Implementation
   observations: the flag's per-backend mmap store is single-backend).
2. Run the full `make test` (~106 pg_regress files) against it. Because
   single-user-mode concurrency is absent, *every* test that does not
   rely on a second backend must pass identically. Triage any diff: the
   likely suspects are (a) lazy-init ordering of the store, (b) the
   `create_gate` single-message forcing, (c) `READB_BYTES` plumbing.
3. Add this variant as a GitHub Actions job so the in-process path cannot
   silently rot relative to the worker path.

Only once green do we cross into WASM. This is the single most important
de-risking step in the whole plan.

### 6. Boost: keep for phase 1, drop later

Phase 1 keeps the `'g'`/`'j'` Boost-serialized circuit transfer
(`MMappedCircuit.cpp:375,501` ↔ `CircuitFromMMap.cpp:75,220`) flowing over
the FIFO unchanged → must build **`libboost_serialization`** (currently
`-lboost_serialization`, `Makefile.internal:140`) for WASM via
`./b2 toolset=emscripten link=static --with-serialization`. Header-only
Boost (`multi_index`, `heap`, `container`, `property_tree`) needs only an
`-I` include path.

Phase 2 optimisation: since there is one process, `CircuitFromMMap` can
call `MMappedCircuit::createGenericCircuit(token)` /
`createGenericCircuit(roots)` (`MMappedCircuit.h:308,328`) **directly**,
deleting the serialize→FIFO→deserialize round-trip for `'g'`/`'j'`. That
removes the *only* compiled-Boost dependency → no `b2` build needed at
all. Worth doing once phase 1 is green (also a latency win), but not on
the critical path. Note the Boost-over-pipe layer existed *only* because
of the worker/backend split — it has no reason to exist in one process.

### 7. Optional: remote KCMCP over WebSocket (deferred, additive)

Re-enable exact d4-class compilation without a server-side data plane: a
`kind='kcmcp-ws'` `ToolRegistry` variant whose client speaks the existing
wire codec (`src/kcmcp_protocol.{h,cpp}`) over an Emscripten WebSocket
(`emscripten/websocket.h`) instead of `AF_UNIX`/TCP. Purely additive;
land after §1–§5. This is the one place a network call re-enters an
otherwise serverless deployment, so make it explicit opt-in: the data
plane (circuits) stays client-side; only the NP-hard compile is offloaded.

### 8. WASM build target (`Makefile.internal`)

- Add a `wasm` goal that builds `provsql` with Emscripten against the
  **same Postgres WASM headers/ABI PGlite uses** (mirror PGlite's Docker
  SDK; pin a PGlite version — its extension API is explicitly unstable).
  Concretely (PGlite 0.4.6): the target is **PostgreSQL 17.5,
  `wasm32-unknown-linux-gnu`** — *not* the local PG18 dev cluster, so
  the extension must build against PG17 server headers as patched for
  WASM. The build is **Docker-based**: add the extension as a submodule
  under `postgres-pglite/pglite/other_extensions`, append its folder to
  that directory's `Makefile` `SUBDIRS`, and run `pnpm build:all`; the
  output is a `.tar.gz` of the `.so` + `.control` + `--.sql`. ProvSQL is
  the documented "unhappy path" (Boost must be compiled for WASM; some
  symbols may need explicit exporting), so expect iteration there.
- Toolchain: `em++`, `-std=c++17` (already required), `with_llvm=no`
  (already forced — correct for WASM). `-fPIC` stays.
- No `-pthread` needed (ProvSQL uses no threads); PGlite itself may want a
  Web Worker for the OPFS backend, independent of ProvSQL.
- Output the PGlite extension bundle: `.tar.gz` of `provsql.wasm` +
  `provsql--<ver>.sql` (already generated from `provsql.common.sql` +
  `provsql.14.sql`) + `provsql.control`, with `module_pathname` pointing
  at the `.wasm`. PGlite stages control+SQL under `$sharedir` and fetches
  the `.wasm` on `CREATE EXTENSION provsql`.
- **Planner-hook timing — resolved (M0).** ProvSQL installs
  `prev_planner` (the transparent-rewrite hook) in `_PG_init`; it must be
  live before the first provenance query. Confirmed: PGlite's loader
  (`extensionUtils.ts`) untars the bundle into the WASM sharedir,
  preloads each `.so` for `dlopen`, and `CREATE EXTENSION` then loads the
  library — running `_PG_init`, which sets the global `planner_hook`
  pointer that covers *every subsequent query* in the (single) backend.
  No `shared_preload_libraries` is needed, which is fortunate because
  (a) PGlite's loader has no `shared_preload_libraries` plumbing and
  (b) the WASM build deletes the background worker, the only native
  reason preload was required. If a provenance query could run before any
  provsql C function has loaded the `.so`, force the load from PGlite's
  extension `init()` callback (run `CREATE EXTENSION provsql` there). The
  hook mechanism itself was validated natively (see Implementation
  observations).

### 9. ProvSQL Studio: static TS port (`studio/web/`)

Studio's server is thin: `db.py` (psycopg connect + `pg.query`),
`circuit.py` (circuit→JSON for the D3 view), `app.py` (routes that wrap
the user query, e.g. `provsql.where_provenance(...)`). Port the
query-wrapping + circuit-shaping logic to `db.ts` / `circuit.ts` calling
`pg.query(...)` on an in-page PGlite instance; keep `studio/static/`
JS essentially unchanged (it already consumes JSON over `fetch` — point
it at the in-page PGlite instead of an HTTP endpoint). Result: a fully
static Studio servable from `file://` or a CDN, no Python at runtime.

- The tool-registry-driven pickers (Studio queries `provsql.tools`) just
  see an empty CLI-tool set in-browser, which the UI already tolerates.
- Port the Playwright e2e suite (`studio/tests/e2e/`) to drive the static
  build (use the `PLAYWRIGHT_HOST_PLATFORM_OVERRIDE` note in
  `CLAUDE.local.md`).
- Flask-under-Pyodide is rejected: Pyodide cannot speak libpq to PGlite
  (no socket), so `db.py` would still need a JS bridge — you end up
  writing the TS bridge anyway, plus a multi-MB Pyodide download.
- The PyPI `provsql-studio` package stays the canonical server product;
  the browser build is a **second distribution target** sharing
  `static/` and the wrapping logic, not a replacement.

## Milestones / priorities

Ship-when ordering; each milestone is independently demonstrable.

- [x] **M0 — host spike: done.** A trivial planner-hook C extension was
      built to WASM and loaded live into PGlite; its `_PG_init` hook fires
      for subsequent queries and returns correct results. Both M0
      questions are now settled end-to-end (see Implementation
      observations for the build recipe and the ABI-compatibility result
      that informs M4).
- [x] **M1 — in-process transport (native): done.** §1 + §2 behind
      `PROVSQL_INPROCESS_STORE` (FIFO loopback + synchronous dispatch,
      bgworker/shmem/LWLock compiled out). Built with
      `make PG_CPPFLAGS='-DPROVSQL_INPROCESS_STORE -O0 -g'`; native build
      unchanged. All 176 pg_regress tests pass against a self-managed
      cluster (staged install + `extension_control_path`, provsql
      preloaded by absolute path) **when run serially**
      (`--max-connections=1`); see Implementation observations for why
      serial is required and the validation recipe.
- [x] **M2 — buffer storage (native): done.** §3 `MappedRegion`
      abstraction (shared-mmap backend native; `malloc`+`pread`/`pwrite`
      heap-buffer backend under the flag); `MMappedVector` and
      `MMappedUUIDHashTable` compose it. Per-backend `on_proc_exit`
      write-back persists the buffer so a later backend (and PGlite
      reload) sees it. Native build unchanged: 176/176 in parallel. Flag
      build: 176/176 with a **fully-serial schedule** (see Implementation
      observations — the heap-buffer store is single-backend, and
      `--max-connections=1` alone does not serialise pg_regress parallel
      groups tightly enough; one test per line does).
- [x] **M3 — guards: done.** §4 process/socket paths compiled out under
      `PROVSQL_NO_SUBPROCESS` (auto under `__EMSCRIPTEN__`): the
      `run_in_own_pgroup` fork/exec, the KCMCP supervisor fork/exec, and
      the KCMCP `connect_endpoint` socket. Guards compile both natively
      and under the full WASM flag combo. With the combo forced on native,
      only the 12 external-tool-specific tests fail (d4/c2d/minic2d/
      dsharp/weightmc + their timeouts, compile-to-ddnnf-via-tool,
      probability_benchmark, graph-easy view_circuit, tool_search_path) —
      all 164 others pass, confirming probability falls back to the
      in-process tree-decomposition compiler / Monte Carlo / safe-query.
- [ ] **M4 — first WASM build:** §8 + §6-phase1 (Boost built for WASM);
      `CREATE EXTENSION provsql` in PGlite; `add_provenance`, a
      provenance SELECT, `sr_boolean`/`sr_counting`, `view_circuit` DOT.
- [ ] **M5 — probability in-browser:** tree-dec compiler + Monte Carlo +
      Shapley + continuous-RV evaluators verified client-side.
- [ ] **M6 — Studio web build:** §9 static Studio over in-page PGlite;
      Playwright e2e green.
- [ ] **M7 (optional)** — Boost-drop (§6 phase 2) and WS-KCMCP (§7).

A compelling public demo exists at M5: one static HTML page that boots
Postgres+ProvSQL and computes provenance + probabilities entirely
client-side, no server — already a teaching artefact before Studio lands.

## Risks / watch-list

- **The single-process collapse is the whole game.** Most "hard
  blockers" (worker, pipes, shmem, Boost-over-pipe) are *artefacts of
  multi-backend coordination* with no reason to exist in PGlite's
  one-process world. They are deletions behind one flag, not emulations.
  The genuinely new code is the §3 storage backend and the §8 build.
- **PGlite extension API instability** — pin the version; budget for
  re-pinning on PGlite minor bumps.
- **Planner-hook preload timing** (M0) — highest-uncertainty unknown;
  spike before committing to the full port.
- **`mmap(MAP_SHARED)` is the one true Emscripten wall** — do not try to
  make it write back; use buffer + explicit `write()`/`fsync` and let
  PGlite's file-level flush persist it (§3).
- **`sync()` cost** — whole-region write-back may dominate on large
  circuits; have the dirty-range fallback ready.
- **MEMFS 2 GB/file cap** — bounds circuit size in the default FS; OPFS is
  the path for large circuits.
- **Boost-for-WASM build friction** (M4) — if `b2 toolset=emscripten`
  fights back, jump straight to §6 phase 2 (direct in-process circuit
  build, no compiled Boost) rather than fighting the toolchain.
- **Don't oversell exact probability** — in-browser exact `#SAT` is
  bounded by the in-process tree-decomposition compiler's reach
  (d4 only wins ≈ treewidth 8–9, per `bounded-treewidth-data.md`); be
  explicit in docs about exact-vs-Monte-Carlo and the WS-KCMCP escape.

## Implementation observations

### M2 buffer storage — what was established

- **One storage primitive, two backends.** `src/MappedRegion.h` owns the
  file descriptor + base pointer + length, with `map`/`remap`/`sync`/
  `close`.  Native: `mmap(MAP_SHARED)` + `msync` (unchanged behaviour,
  176/176 in parallel).  Flag: a `malloc` buffer loaded with `pread` and
  written back with `pwrite`.  `MMappedVector` and `MMappedUUIDHashTable`
  hold a `MappedRegion` and a typed view of `region.base()`, refreshed
  after each `remap` (the mmap backend already moved the pointer on grow,
  so neither class held stale pointers — the realloc backend is safe for
  the same reason).
- **`resizeFile` is a no-op under the flag — and that matters.** The
  mmap backend must `ftruncate` the file to the mapped size; the buffer
  backend must NOT.  If it pre-sized the file, a fresh file that is never
  synced (a backend that `abort()`s before write-back — e.g. an uncaught
  C++ exception, which skips `on_proc_exit`) would persist full-size with
  a zero header and fail magic validation on the next open, cascading
  aborts.  Leaving the file unsized until the first `sync()` means an
  unsynced fresh file stays empty on disk and is re-initialised cleanly.
- **Write-back must be armed per-backend, not at `_PG_init`.** The store
  syncs to its files from an `on_proc_exit` hook so a later backend (and
  PGlite reload-from-IndexedDB) sees the data.  Registering that hook in
  `_PG_init` does NOT work under `shared_preload_libraries`: `_PG_init`
  runs in the postmaster and a forked backend resets the inherited
  `on_proc_exit` list.  It is registered lazily in `provsql_inproc_send`
  on the backend's first store access instead.
- **Native flag testing requires a fully-serial schedule.** The
  heap-buffer store is per-backend and single-writer; it cannot tolerate
  concurrent backends (each would read/grow/write its own copy and lose
  the others' updates).  pg_regress parallel groups break it, and
  `--max-connections=1` does not serialise them tightly enough.  Run with
  a schedule that has one test per line:
  `awk '/^test:/{$1="";n=split($0,a," ");for(i=1;i<=n;i++)if(a[i]!="")print "test: "a[i];next}{print}' test/schedule > schedule.serial`
  then `make installcheck REGRESS_OPTS="--load-extension=plpgsql
  --inputdir=test --outputdir=<dir> --schedule schedule.serial
  --dbname=contrib_regression" EXTRA_REGRESS_OPTS="--host=… --port=…
  --user=… --max-connections=1"`.  Keep `--dbname=contrib_regression`
  (some tests `\c` to it by name).  This is a native-test artifact only:
  the single-process WASM target never has a second backend.

### M1 in-process transport — what was established

- **The transport swap is correct.** With `PROVSQL_INPROCESS_STORE`, the
  whole backend↔worker pipe layer is replaced by two in-memory FIFOs
  (`req`/`resp` in `provsqlSharedState`), `SENDWRITEM` appends the
  message and runs `provsql_mmap_dispatch` once inline, and the existing
  client/worker code is otherwise untouched (the worker `switch` was
  lifted verbatim out of `provsql_mmap_main_loop` into
  `provsql_mmap_dispatch`). The five raw pipe-IO sites became
  `READB_BYTES`/`READM_BYTES`/`WRITEB_BYTES` macros (FIFO pop/push under
  the flag, a looping `provsql_read_all` / `write` natively).
  `create_gate` is forced down its single-message path (the FIFO has no
  `PIPE_BUF` limit) and the IPC buffer becomes heap-growable. Locks are
  no-ops; the workers are not registered; `_PG_init` calls
  `provsql_inproc_init` instead. All 176 regression tests pass.
- **Native build is unaffected.** The `#else` branches preserve the
  pipe/shmem/worker code byte-for-byte; a plain `make` still produces a
  worker-based extension and passes all 176 tests.
- **Native flag testing must be serial (`--max-connections=1`).** Under
  the flag there is no single-writer worker: each backend maps the
  shared `provsql_*.mmap` files itself (the §3 buffer-backed store is
  still M2 to-do). pg_regress *parallel groups* (e.g. schedule line with
  `probability_having` and five siblings) then run several backends that
  grow and read the same mmap files concurrently, and a grow in one
  backend leaves another's mapping stale — surfacing as a sporadic
  failure such as `probability_having`'s "semiring does not support
  value gates". This is the documented single-backend limitation, not a
  transport bug: the same suite is 176/176 green serially, and 176/176
  on the native worker build run in parallel. The genuinely
  single-process WASM target has no concurrent backends, so this is moot
  there; the native CI job for the flag (§5) should pass
  `--max-connections=1`.
- **Validation recipe (no root needed, PG18).** `make install
  DESTDIR=<stage>`; edit the staged `provsql.control` `module_pathname`
  to the absolute staged `.so`; `initdb` a private cluster; in
  `postgresql.conf` set `shared_preload_libraries='<abs .so>'`,
  `extension_control_path='<stage ext dir>:$system'`, `lc_messages='C'`;
  then `make installcheck EXTRA_REGRESS_OPTS="--host=<sock> --port=<p>
  --user=<me> --max-connections=1"`. Preloading the flag build is valid
  because its `_PG_init` only installs hooks + `provsql_inproc_init`
  (no worker/shmem), and it arms the planner hook in every backend.

### M0 host spike — what was established (and what is blocked)

Findings from the M0 spike, recorded so the next attempt does not
re-derive them:

- **Planner-hook mechanism (validated natively).** A minimal
  `MODULES = …` extension whose `_PG_init` chains `planner_hook` was
  built with the stock PGXS toolchain and `LOAD`ed into a throwaway
  PostgreSQL cluster. Result: queries issued *before* the load are not
  hooked; the `LOAD` runs `_PG_init` (confirmed in the server log); and
  *every* query after it fires the hook and chains into
  `standard_planner`. This is exactly the contract ProvSQL's
  transparent-rewrite hook needs, and it holds without
  `shared_preload_libraries`.
- **PGlite packaging/loader (read from source, `extensionUtils.ts`).** An
  extension is a `.tar.gz` untarred into `WASM_PREFIX` (the WASM
  sharedir); `.so` entries are registered with Emscripten's WASM preload
  plugin so `dlopen` finds the pre-compiled module, and `.control` /
  `--.sql` files are copied verbatim. The TS side is an `Extension`
  object whose `setup()` returns a `bundlePath` URL to the tarball, plus
  an optional `init()` that runs after Postgres starts (the right place
  to force `CREATE EXTENSION provsql`).
- **PGlite runtime (booted in-sandbox).** Prebuilt `@electric-sql/pglite`
  0.4.6 boots in Node and runs queries here; it reports **PostgreSQL
  17.5 on `wasm32-unknown-linux-gnu`**. This is the Node half of the
  eventual browser test harness, and pins the build target to PG17.
- **Toolchain (resolved with rootless podman; no Docker daemon needed).**
  Building an extension to a PGlite-loadable `.wasm` uses the published
  builder image `electricsql/pglite-builder:3.1.74-5-postgis-libicu-min`
  (pinned Emscripten 3.1.74 + zlib/libxml2/libxslt/openssl/ossp-uuid/ICU
  pre-compiled to WASM under `/install/libs`). It runs fine under
  **rootless `podman`** as the unprivileged sandbox user — no daemon, no
  `docker` group. Recipe that worked:
  1. clone `pglite` + init the `postgres-pglite` submodule (pinned
     commit; ~150 MB);
  2. add the extension as a normal PGXS dir under
     `pglite/other_extensions/<name>/` (`MODULES`/`EXTENSION`/`DATA` +
     `.control` + `--<ver>.sql`);
  3. run a trimmed `build-pglite.sh`: the `emconfigure ./configure
     --host wasm32-unknown-linux-gnu …` block + `emmake make` +
     `emmake make install` (builds Postgres core to WASM), then
     `emmake make -C pglite/other_extensions SUBDIRS=<name> <name>.tar.gz`
     (skip contrib / postgis / the final pglite link for a fast
     extension-only build);
  4. `podman run --rm -v $PWD:$PWD:rw -v $PWD/dist:/pglite:rw <image>
     ./build-pglite-trimmed.sh`.
  The output `<name>.tar.gz` has exactly the loader layout
  (`lib/postgresql/<name>.so`, `share/postgresql/extension/<name>.control`
  + `--<ver>.sql`); the `.so` is a real `\0asm` SIDE_MODULE.
- **Live load confirmed (the actual M0 result).** Wired the tarball as a
  PGlite `Extension` (`{ name, setup → { bundlePath } }`) and loaded it in
  Node. Queries before `CREATE EXTENSION` are un-hooked; `CREATE
  EXTENSION` + one call to the module's C function force-loads the `.so`
  (running `_PG_init`); and every query afterwards fires the planner hook
  and returns correct rows. `onNotice` (constructor or per-query) is how
  the JS side observes server `NOTICE`s — relevant for Studio surfacing
  ProvSQL notices.
- **ABI compatibility → M4 simplification.** The extension was built
  against the `postgres-pglite` **submodule HEAD** yet loaded cleanly into
  the **prebuilt npm `@electric-sql/pglite` 0.4.6** (both PostgreSQL 17.5
  / `wasm32`). So M4 can likely ship ProvSQL as a standalone extension
  bundle against stock PGlite, without rebuilding `pglite.wasm` itself —
  provided the symbols ProvSQL needs are in PGlite's exported set (verify
  per-symbol during M4; `-sERROR_ON_UNDEFINED_SYMBOLS=0` means missing
  ones surface only at `dlopen`/run time).
```
