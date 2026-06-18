Knowledge-Compiler / Model-Counter Server Protocol
===================================================

This page specifies **KCMCP** (Knowledge Compiler / Model Counter
Protocol), a small, backend-agnostic wire protocol for talking to a
*warm*, long-lived knowledge compiler or model counter over a stream
socket.  It is the protocol ProvSQL aims to use to call out to an
external knowledge compiler or model counter: the same external tool it
would otherwise invoke on the command line, reached through a more
convenient, efficient, and expressive interface than spawning a fresh
process and exchanging temporary files.  The protocol is written so that
one client and one server code path work regardless of which engine sits
behind them (see :doc:`probability-evaluation` for where such an external
tool sits in the probability pipeline).

It is an alternative to the way ProvSQL invokes these tools today, in
**command-line mode**: forking a fresh process, writing the problem to a
temporary file, ``exec``-ing the tool, and reading its output back from
another temporary file on every call.  KCMCP replaces that per-call
process spawn and the temporary files with a single warm process and a
socket round-trip, while keeping the command-line path as a fallback so
the two modes coexist.

The protocol is designed from first principles around explicit framing
and capability negotiation.  It is the protocol ProvSQL aims to converge
on; it does not depend on, and is not derived from, the wire format of
any server that exists today.

.. note::

   The standalone :program:`tdkc` tool is a **reference implementation** of
   the server side.  Run it with ``tdkc --kcmcp unix:/path/to.sock`` or
   ``tdkc --kcmcp host:port``; it advertises ``compile`` (to ``ddnnf-nnf``)
   and ``wmc`` (to ``decimal``) over ``dimacs-cnf`` input, serving each
   request through ProvSQL's in-process tree decomposition, and implements
   the ``cancel`` and ``progress`` features.  It exists to exercise and pin
   the protocol; ProvSQL itself keeps tree decomposition in process rather
   than talking to it over a socket.  ``test/kcmcp/conformance.py`` drives it
   through a handshake, ``compile``/``wmc`` requests, ``PING``/``PONG``, and
   error cases (``make test-kcmcp``).


Design goals
------------

KCMCP is built to the following principles.  Each is a positive
requirement on any conformant implementation, stated without reference
to any particular engine's current behaviour:

- **Explicit, byte-order-defined framing.**  Every integer is unsigned
  big-endian; every variable-length payload is length-prefixed.  A
  message boundary is never found by scanning for a sentinel character,
  so a partial ``recv`` is always unambiguous and content can never
  truncate a frame.

- **Location-independent transport.**  The protocol runs over any
  reliable, ordered byte stream, so the server may be co-located with the
  client (a Unix socket or a loopback connection) or reached across a
  network (a TCP connection to a configured host and port).  Where the
  channel is not inherently trusted, securing it -- a private network, an
  SSH tunnel, or a TLS proxy in front of the server -- is a deployment
  concern, not part of this version of the protocol.

- **A warm, supervised, queueing server.**  One long-lived process
  accepts many connections in a loop and serves requests over a
  persistent connection, so a client pays no per-request process-spawn
  cost and a future cross-query cache has somewhere to live.  Concurrent
  clients queue rather than being refused.

- **Robust to bad input.**  A malformed or unsupported request is
  answered with an :msg:`ERROR` frame; the server never terminates on it.

- **Capability negotiation.**  A client discovers what the connected
  backend supports through a handshake, rather than assuming a fixed
  feature set, so one client speaks to counters and compilers alike.

- **Operation- and format-neutral.**  Counting, weighted counting and
  knowledge compilation are selected by fields in the request, and the
  input and output encodings are negotiated numbers, so the same wire
  carries every engine's job.

KCMCP mandates **no particular operation or format**.  What a backend can
do is exactly what it advertises at the handshake; a client and a backend
interoperate when their advertised capabilities share at least one
``(operation, input_format, output_format)`` combination.  The names of
operations and formats are drawn from a shared registry (below), so that
a given name means the same thing to both sides -- but which of them any
one tool implements is its own choice.  A pure knowledge compiler that
only reads a particular circuit format and only emits a compiled form is
as conformant as a counter that reads DIMACS-CNF and returns a number.
DIMACS-CNF is merely the most widely understood input and therefore a
convenient common denominator, not a requirement.  When two peers share
no usable combination, the handshake makes that explicit and the client
falls back (for ProvSQL: to another tool).


Transport
---------

KCMCP runs over a reliable, ordered, byte-stream socket.  Where the
server runs is a deployment choice, not part of the protocol; an
implementation MUST support at least one of the following:

- **Local, same host.**  A Unix domain socket (filesystem permissions
  are the access-control boundary, with no port to allocate or leak), or
  a loopback TCP endpoint.  When a supervisor launches the server it MAY
  let the OS assign an ephemeral port (``bind`` to port ``0``) and learn
  the chosen port from the child (for ProvSQL: a supervisor background
  worker writes it into shared memory, mirroring how
  :cfile:`provsql_mmap.c` registers its worker and how
  :cfile:`provsql_shmem.h` carries shared state).
- **Remote host.**  A TCP connection to a host and port the client is
  configured with.  A single warm server can then serve several client
  machines.

KCMCP v1 defines no authentication or transport encryption, and places no
requirement on the bind address.  On a single trusted host the local
boundary is the security model; for a remote server the channel must be
secured out of band (a private network, an SSH tunnel, or a TLS proxy in
front of the server).

The server runs an ``accept`` loop.  How it schedules work **across**
connections is left to the implementation: it MAY serve them
sequentially in a single process -- the intended posture for a warm
cross-query cache, in which case concurrent clients queue on the socket
-- or handle several concurrently.  The protocol does not constrain this.

Endpoint identification
^^^^^^^^^^^^^^^^^^^^^^^^

The protocol defines no discovery mechanism: the endpoint -- a
Unix-socket path or a ``host:port`` -- is communicated to the client out
of band.  For ProvSQL the supervisor background worker publishes the live
loopback port in shared memory, and republishes it after a respawn, so a
backend always reaches the current server; a remote deployment is
configured with a fixed ``host:port``; a Unix socket lives at a known
path.

Because KCMCP v1 has no authentication, a client trusts whatever answers
at the endpoint.  The :msg:`HELLO` handshake is the sanity check: before
sending a problem the client confirms it received a well-formed
:msg:`HELLO` carrying a ``kcmcp`` version it can speak and the
capabilities it needs.  That establishes "this is a KCMCP server I can
use", but not *which* server -- identity rests on the transport.  A Unix
socket guarded by filesystem permissions scopes who can reach it; a
loopback TCP ephemeral port has a squatting / race window (another local
process may connect, or bind the port before the server does).  Prefer
the Unix socket where the trust boundary matters.


.. _kcmcp-lifecycle:

Connection lifecycle
--------------------

A KCMCP session uses **one connection for its entire life**: the client
opens the socket once -- a single TCP or Unix-socket connect -- and that
same connection carries the whole exchange.  It is **not** a fresh
connection, nor a fresh handshake, per request.  Reconnecting per job
would re-pay the connect and the :msg:`HELLO` round-trip and, worse,
discard the warm cross-query cache that motivates having a server at all.

A session proceeds as:

#. **Connect.** The client opens the socket to the server's endpoint.
#. **Handshake.** Each side sends exactly one :msg:`HELLO`
   (``request_id = 0``) and reads the other's, fixing the protocol
   version and the capabilities for the rest of the connection.  It is
   never repeated.
#. **Work.** The client issues any number of request/response cycles on
   the same connection -- a :msg:`REQUEST` answered by a :msg:`RESULT` or
   an :msg:`ERROR`, optionally preceded by :msg:`PROGRESS` frames --
   correlating each by ``request_id``.  A job in progress can be aborted
   with a :msg:`CANCEL` carrying its ``request_id``.
#. **Idle.** Between jobs, and throughout a long-running compile, the
   connection simply stays open with no data flowing.  :msg:`PING` /
   :msg:`PONG` keep it alive on a link (for example a remote TCP path
   through a NAT or stateful firewall) that would otherwise drop an idle
   connection; a local Unix-socket or loopback connection needs no such
   keepalive.
#. **Close.** Either side ends the session with a :msg:`BYE` and closes
   the socket.

While the server is computing a :msg:`REQUEST` it keeps reading the
connection, so it can act on control frames **mid-job** -- aborting on
:msg:`CANCEL`, answering a :msg:`PING` with :msg:`PONG`, and emitting
:msg:`PROGRESS` -- rather than going deaf until the :msg:`RESULT` is
ready.  A server that cannot interleave computation with this socket
servicing says so by omitting ``cancel`` (and ``progress``) from its
advertised ``features``; for such a server a running job is
uninterruptible, and the client falls back to transport-level keepalive
and its own timeout.  How the server schedules work **across**
connections -- serialising it or running several jobs at once -- is, by
contrast, left to the implementation (see *Transport*).

If the connection drops unexpectedly -- reset, idle timeout, or a server
crash -- any in-flight job is abandoned: the client reconnects,
re-handshakes, and resubmits.  For ProvSQL the supervisor background
worker respawns a crashed server, so the next connect lands on a fresh
process (with an empty cache).


Frame format
------------

Every message in either direction is a **frame**: a fixed 10-byte header
followed by an opaque, length-delimited payload.  All multi-byte
integers are **unsigned big-endian** (network byte order).

.. code-block:: text

   offset  size   field
   ------  -----  -----------------------------------------------------
     0     u8     type          message kind (see table below)
     1     u8     flags         bit0 MORE       payload continues in the
                                                next frame (same type and
                                                request_id)
                                bit1 COMPRESSED payload is zstd-compressed
                                bits2-7         reserved, MUST be 0
     2     u32    request_id    client-chosen correlation id;
                                0 = connection-scoped (no correlation)
     6     u32    payload_len   number of payload bytes that follow,
                                bounded by the advertised max_payload
    10     ...    payload[payload_len]

Carrying ``request_id`` in the *header* (rather than the payload) lets
:msg:`CANCEL`, :msg:`PROGRESS` and :msg:`RESULT` frames reference an outstanding
job without parsing its body, and leaves room to add request pipelining
later without a format change.  The ``MORE`` flag lets a large payload
(a multi-megabyte d-DNNF :msg:`RESULT`, or an equally large
:msg:`REQUEST` problem) stream as several frames sharing one
``request_id``; the receiver concatenates their payloads in order until
it sees a frame with ``MORE`` clear.  ``MORE`` works in **either
direction** and is the mechanism for sending a payload larger than the
receiver's per-frame limit: the sender splits it into frames each within
that limit.

A receiver MUST reject (with an :msg:`ERROR` frame, code ``7``) any frame
whose ``payload_len`` exceeds the ``max_payload`` it advertised at
handshake, rather than attempting to allocate it.  To guarantee a usable
baseline, every implementation MUST accept a single frame whose payload
is up to **1 MiB** (1048576 bytes); accordingly a server's advertised
``max_payload`` MUST be at least 1 MiB.  A sender MUST NOT exceed the
receiver's limit -- the advertised ``max_payload``, or the 1 MiB floor
for a peer that advertises none (i.e. the client) -- and splits anything
larger across ``MORE``-flagged frames within that limit.

KCMCP v1 negotiates no payload compression, so a receiver need not support
the ``COMPRESSED`` flag (bit 1).  A sender MAY nonetheless set it
optimistically: a receiver that cannot decode the payload drains it -- its
length is bounded by ``max_payload``, so the stream stays synchronised --
and replies with an :msg:`ERROR` of code ``9`` on the same connection,
rather than mis-reading the bytes as plain.  The sender then retries the
frame uncompressed; the :msg:`ERROR` is the fallback signal, so the
exchange doubles as a (coarse) compression negotiation.  A future revision
may instead advertise compression support as a ``feature``, letting a
client skip the trial.

Message types
^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 12 18 70

   * - Value
     - Name
     - Meaning
   * - ``0x00``
     - :msg:`HELLO`
     - Handshake; sent once by each side (``request_id = 0``).
   * - ``0x01``
     - :msg:`REQUEST`
     - A counting or compilation job.
   * - ``0x02``
     - :msg:`RESULT`
     - A successful answer to a :msg:`REQUEST`.
   * - ``0x03``
     - :msg:`ERROR`
     - A job-level or protocol-level failure.
   * - ``0x04``
     - :msg:`PROGRESS`
     - Optional liveness / log update for a long-running job.
   * - ``0x05``
     - :msg:`CANCEL`
     - Abort the job named by ``request_id``.
   * - ``0x06`` / ``0x07``
     - :msg:`PING` / :msg:`PONG`
     - Connection keepalive.
   * - ``0x08``
     - :msg:`BYE`
     - Graceful connection close.

A server MUST reply to an unknown ``type`` with an :msg:`ERROR` frame
(code ``1``) and keep the connection open; it MUST NOT terminate.


.. _kcmcp-hello:

Handshake: ``HELLO``
--------------------

Immediately after the connection is established each side sends exactly
one :msg:`HELLO` frame with ``request_id = 0``.  The payload is **UTF-8
JSON**: control metadata stays human-readable and freely extensible,
while only the heavy problem and result payloads are binary.

The client announces the protocol version it understands:

.. code-block:: json

   { "kcmcp": [1, 0], "client": "ProvSQL/1.8.0" }

Client ``HELLO`` fields:

.. list-table::
   :header-rows: 1
   :widths: 20 24 56

   * - Field
     - Type
     - Meaning
   * - ``kcmcp``
     - two-element array ``[major, minor]`` of integers
     - **Required.** The protocol version the client implements:
       ``major`` is bumped only for breaking changes, ``minor`` for
       backward-compatible revisions. The server selects a version no
       newer than this (see the server ``kcmcp`` field below).
   * - ``client``
     - string, ``name/version`` or bare ``name``
     - *Optional.* Client identifier, surfaced in the server's logs and
       diagnostics only. Either a ``name/version`` string (e.g.
       ``ProvSQL/1.8.0``) or a bare ``name`` with no ``/`` when no version
       is reported. Never used for negotiation, so the server treats any
       value (or its absence) identically.

The server replies with the negotiated version and a **capability
descriptor** -- the object that makes the protocol generic:

.. code-block:: json

   { "kcmcp": 1,
     "engine": "example-kc/1.0",
     "max_payload": 268435456,
     "operations":     ["count", "wmc", "compile"],
     "input_formats":  ["dimacs-cnf", "circuit-bcs12"],
     "output_formats": { "count":   ["decimal", "rational"],
                         "compile": ["ddnnf-nnf"] },
     "features":       ["cancel", "progress", "persistent-cache"] }

Server ``HELLO`` fields (the capability descriptor):

.. list-table::
   :header-rows: 1
   :widths: 20 24 56

   * - Field
     - Type
     - Meaning
   * - ``kcmcp``
     - integer
     - **Required.** The major protocol version the server selected for
       this connection; never greater than the client's ``major``. A
       client that cannot speak it closes the connection and falls back.
       Because a ``major`` bump is a breaking change, there is no shared
       version when the client's ``major`` *exceeds* every ``major`` the
       server implements: the server does **not** silently downgrade but
       replies with an :msg:`ERROR` of code ``8`` and closes, and the client
       falls back.
   * - ``engine``
     - string, ``name/version`` or bare ``name``
     - *Optional.* Identifies the **backend** knowledge compiler / model
       counter the server drives -- not the KCMCP server process itself
       (the two coincide only when the server is the engine). For the
       client's logs and diagnostics only; not used for negotiation.
       Either a ``name/version`` string (e.g. ``example-kc/1.0``) or a
       bare ``name`` with no ``/``.
   * - ``max_payload``
     - integer (bytes)
     - *Optional*; an implementation-defined default (at least the 1 MiB
       floor) applies when absent. The largest ``payload_len`` the server
       will accept in a single frame; it MUST be at least **1 MiB**
       (1048576). A client MUST NOT send a frame exceeding it (splitting
       larger problems with ``MORE``), and the server rejects an oversize
       frame with an :msg:`ERROR` of code ``7``.
   * - ``operations``
     - array of strings
     - **Required.** The operation names the server can perform, drawn
       from the operation registry (``count``, ``wmc``, ``compile``,
       ...). Any subset is valid, including a single entry.
   * - ``input_formats``
     - array of strings
     - **Required.** The problem input-format names the server accepts,
       drawn from the input-format registry (``dimacs-cnf``,
       ``circuit-bcs12``, ...).
   * - ``output_formats``
     - object mapping each operation name to an array of strings
     - **Required.** For every operation the server offers, the
       output-format names valid for it. Its keys are a subset of
       ``operations``; its values are drawn from the output-format
       registry (``decimal``, ``rational``, ``ddnnf-nnf``, ...).
   * - ``features``
     - array of strings
     - *Optional.* Protocol capabilities the server implements beyond the
       mandatory core: currently ``cancel`` (reads and honours a
       :msg:`CANCEL` while a job is running, which requires servicing the
       connection mid-job), ``progress`` (may emit :msg:`PROGRESS` frames
       during a job), and ``persistent-cache`` (retains its solver cache
       across requests). A client uses only the features listed here; an
       absent feature is treated as unsupported -- in particular, without
       ``cancel`` a running job cannot be aborted.

A client and a server interoperate when, for some operation the client
needs, that operation is in ``operations``, the input encoding the client
will send is in ``input_formats``, and the output encoding it wants is in
``output_formats`` for that operation. Both directions are **extensible**:
a receiver MUST ignore any field it does not recognise, so later versions
can add fields without a version bump (see *Forward compatibility* under
`Design notes and non-goals`_).

No entry is required: the ``operations``, ``input_formats`` and
``output_formats`` sets are each free.  A counter that lists just
``"count"`` over ``"dimacs-cnf"`` and a knowledge compiler that lists
just ``"compile"`` over some circuit format are each fully conformant --
a client simply uses whichever advertises a combination it can work
with, and otherwise falls back.


.. _kcmcp-request:

``REQUEST`` payload
-------------------

.. code-block:: text

   offset  size   field
   ------  -----  -----------------------------------------------------
     0     u8     operation      0 count  1 wmc  2 compile
                                 3 enumerate  4 sample  ...
     1     u8     input_format   0 dimacs-cnf  1 circuit-bcs12  2 aig ...
     2     u8     output_format  0 decimal 1 rational 2 double 3 bigint
                                 4 ddnnf-nnf 5 sdd 6 obdd ...
                                 (one shared code space; an operation
                                  accepts only the subset listed for it)
     3     u8     reserved (0)
     4     u16    options_len
     6     ...    options[options_len]   UTF-8 JSON object (see Options)
    ...    ...    problem[ rest ]        the formula bytes;
                                         length = payload_len - 6 - options_len

The ``problem`` block is **exactly the bytes a backend already parses**
with **no** ``\0`` or ``z`` **sentinel needed**.  (A backend MAY still
tolerate a trailing sentinel for source-compatibility with existing
DIMACS parsers; it just is not the framing mechanism.)

Format-orthogonal knobs -- weights, the projection set, a time budget, an
RNG seed -- live in the ``options`` block rather than being smuggled into
``problem``, so the problem stays a clean formula.

Options
^^^^^^^

The ``options`` block is a **UTF-8 JSON object** -- the same encoding as
the :msg:`HELLO` descriptor and the :msg:`RESULT` ``meta`` -- and an empty
block (``options_len = 0``) is equivalent to ``{}``.  **A receiver MUST
ignore any member it does not recognise**: this is how method-specific
extensions ride along safely -- the server applies the members it knows
and silently drops the rest, so a client may always send an option a
given backend may or may not act on.

The basic members, which every implementation interprets identically:

.. list-table::
   :header-rows: 1
   :widths: 14 20 18 48

   * - Member
     - JSON type
     - Applies to
     - Meaning
   * - ``timeout_ms``
     - integer
     - all operations
     - Wall-clock budget in milliseconds for the job. ``0`` or absent
       means no limit. On exceeding it the server stops and returns an
       :msg:`ERROR` of code ``4``.
   * - ``progress_every_ms``
     - integer
     - servers advertising ``progress``
     - Minimum interval in milliseconds between :msg:`PROGRESS` frames for
       the job. ``0`` emits one on every internal progress check; absent
       leaves the cadence to the server. Ignored by a server that does not
       advertise the ``progress`` feature.
   * - ``seed``
     - integer
     - randomised operations
     - Seed for the engine's RNG (e.g. ``sample`` or approximate
       counting). Absent leaves the choice to the engine, which may then
       be non-deterministic.
   * - ``projset``
     - array of integers
     - ``count``, ``wmc``
     - The projection (relevant) variable set, as positive variable
       indices: the count ranges over assignments to these variables
       only. Absent means no projection, i.e. the ordinary count over all
       variables.
   * - ``weights``
     - object: literal name to number
     - ``wmc``
     - Per-literal weights, keyed by the DIMACS literal as a string (a
       signed, non-zero integer: ``"5"`` and ``"-5"`` are variable 5 set
       true and false). A literal with no entry defaults to weight ``1``.
       For many weights a weighted input format carries them more
       compactly.

An example block:

.. code-block:: json

   { "timeout_ms": 30000,
     "projset": [3, 7, 9],
     "weights": { "5": 0.3, "-5": 0.7 },
     "seed": 42 }

**Engine-specific tuning** -- a backend's solver, preprocessing,
branching / scoring / phase heuristics, cache strategy, and the like --
is carried as **namespaced** members, so it never collides with the basic
keys and is dropped by other engines under the ignore-unknown rule.  By
convention such options live in an object keyed by the engine name (the
``name`` part of the server's :msg:`HELLO` ``engine`` field): a server
applies the sub-object matching its own engine and ignores the rest.
This lets a client drive every knob the backend exposes.  For a
d4-family backend, for instance, the important ``d4v2`` command-line
options map directly onto members of a ``d4`` object (its ``--input-type``
and ``--dump-file`` are already covered by ``input_format`` and the
``compile`` operation, and its preprocessing timeout by ``timeout_ms``):

.. code-block:: json

   { "timeout_ms": 30000,
     "d4": { "preproc": "vivification",
             "solver": "glucose",
             "scoring-method": "vsads",
             "branching-heuristic": "hybrid-partial-classic",
             "phase-heuristic": "polarity",
             "cache-method": "list",
             "float": false } }


.. _kcmcp-result:

``RESULT`` payload
------------------

A :msg:`RESULT` carries the same ``request_id`` as its :msg:`REQUEST`.

.. code-block:: text

   offset  size   field
   ------  -----  -----------------------------------------------------
     0     u8     result_format  echoes the requested output_format
     1     u8     reserved (0)
     2     u16    meta_len
     4     ...    meta[meta_len]   UTF-8 JSON statistics
    ...    ...    result[ rest ]   encoded per result_format
                                   (see Output formats)

The ``meta`` channel is where the protocol earns its generality.  An
**exact** counter or compiler returns timing and structural statistics;
an **approximate** counter returns the same frame shape with a
confidence interval instead:

.. code-block:: json

   { "time_ms": 41, "nodes": 1278, "cache_hits": 904, "exact": true }

.. code-block:: json

   { "time_ms": 5200, "exact": false, "ci": [1.01e6, 1.07e6], "delta": 0.05 }

Clients that do not care about ``meta`` ignore it and read only
``result``.  Because ``result`` is length-framed (and ``MORE``-chunked
when large), a fixed-size read buffer can never truncate it.


.. _kcmcp-registries:

Operation and format registries
--------------------------------

The ``operation``, ``input_format`` and ``output_format`` bytes of a
:msg:`REQUEST`, and the matching ``operations`` / ``input_formats`` /
``output_formats`` names in the :msg:`HELLO` descriptor, are drawn from
these shared registries -- the numeric codes are the on-wire ``REQUEST``
values, the names are the ``HELLO`` strings.  KCMCP v1 specifies the
entries below; any other value is reserved for future use, and a peer
that does not recognise one rejects the request with an :msg:`ERROR`
(code ``1`` for an operation, ``2`` for a format).

Operations
^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 8 16 76

   * - Code
     - Name
     - Meaning
   * - ``0``
     - ``count``
     - Unweighted model count: the number of satisfying assignments of
       the input problem, a non-negative integer. A projection set, if
       any, is given in ``options``.
   * - ``1``
     - ``wmc``
     - Weighted model count: the sum, over satisfying assignments, of the
       product of per-literal weights. Weights come from ``options`` (or
       from the problem itself for a weighted input format).
   * - ``2``
     - ``compile``
     - Knowledge compilation: transform the input into a tractable target
       representation, selected by ``output_format``, and return that
       representation rather than a number.

``enumerate`` and ``sample`` appear in the byte-layout example as
reserved codes; v1 does not specify them.

Input formats
^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 8 18 44 30

   * - Code
     - Name
     - Meaning
     - Reference
   * - ``0``
     - ``dimacs-cnf``
     - A CNF in DIMACS form, optionally carrying the weight
       (``c p weight``) and projection (``c p show``) lines used for
       ``wmc`` and projected counting. The interoperability baseline.
     - `Model Counting Competition format <https://mccompetition.org/assets/files/mccomp_format_24.pdf>`_
   * - ``1``
     - ``circuit-bcs12``
     - A Boolean circuit in d4's BC-S1.2 form (AND / OR / identity gates
       over input literals); ProvSQL emits this to preserve circuit
       structure instead of flattening to CNF.
     - `jm62300/d4 <https://github.com/jm62300/d4>`_

``aig`` appears in the byte-layout example as a reserved code; v1 does
not specify it.

Output formats
^^^^^^^^^^^^^^

``output_format`` is a **single code space shared by every operation**:
each code below is globally unique and is *not* reused across operations,
so a ``result_format`` byte names exactly one encoding regardless of the
operation that produced it. An operation accepts only the subset that
makes sense for it -- the number encodings ``0``--``3`` for ``count`` and
``wmc``, the compiled representations ``4``--... for ``compile``. The
``result`` bytes of a :msg:`RESULT` are encoded exactly as the code
prescribes, and ``result_format`` echoes the code so the client knows how
to decode them.

For ``count`` and ``wmc`` the ``result`` is a number; its wire encoding is
specified precisely (text is US-ASCII, binary is big-endian / network byte
order, consistent with the frame header):

.. list-table::
   :header-rows: 1
   :widths: 6 14 80

   * - Code
     - Name
     - Wire encoding
   * - ``0``
     - ``decimal``
     - **Text.** A base-10 real, matching
       ``[-]?digit+ ( '.' digit+ )? ( [eE] [-+]? digit+ )?`` in US-ASCII.
       Precision is engine-dependent, so not necessarily exact for
       ``wmc`` (use ``rational`` or ``bigint`` when exactness matters).
       The interoperability baseline.
   * - ``1``
     - ``rational``
     - **Text.** The exact value as ``numerator/denominator``: two base-10
       integers separated by a single ``/``, the denominator strictly
       positive, no spaces (an integer count uses denominator ``1``).
       Lossless.
   * - ``2``
     - ``double``
     - **Binary.** The value as one IEEE-754 ``binary64``, exactly 8
       bytes, big-endian. Compact and fixed-width but lossy.
   * - ``3``
     - ``bigint``
     - **Binary.** An exact non-negative integer (e.g. an unweighted
       ``count`` exceeding 64 bits) as its raw magnitude bytes,
       big-endian (most-significant byte first), no leading ``0x00``
       except the single byte ``0x00`` for the value zero. Length is the
       frame's.

For ``compile`` the ``result`` is a compiled representation:

.. list-table::
   :header-rows: 1
   :widths: 6 16 48 30

   * - Code
     - Name
     - Wire encoding
     - Reference
   * - ``4``
     - ``ddnnf-nnf``
     - **Text.** A d-DNNF in the c2d / d4 NNF format (one node or arc per
       line); the form ProvSQL parses back for linear-time probability
       evaluation.
     - `c2d <http://reasoning.cs.ucla.edu/c2d/>`_, `jm62300/d4 <https://github.com/jm62300/d4>`_

``sdd`` (``5``) and ``obdd`` (``6``) appear in the byte-layout example as
reserved codes; v1 does not specify them.


Errors, cancellation, and liveness
-----------------------------------

.. _kcmcp-error:

``ERROR``
^^^^^^^^^

Payload: ``u16 code`` followed by a UTF-8 message.  Defined codes:

.. list-table::
   :header-rows: 1
   :widths: 12 88

   * - Code
     - Meaning
   * - ``1``
     - unsupported operation or unknown frame ``type``
   * - ``2``
     - unsupported input / output format
   * - ``3``
     - parse error in the ``problem`` block
   * - ``4``
     - time budget exceeded
   * - ``5``
     - cancelled at client request
   * - ``6``
     - internal engine error
   * - ``7``
     - payload over ``max_payload`` / out of memory
   * - ``8``
     - unsupported protocol version (client requires a ``major`` the server
       does not implement; see *Handshake*)
   * - ``9``
     - ``COMPRESSED`` payload the server cannot decode (compression is not
       negotiated in v1)

Crucially, sending an :msg:`ERROR` never terminates the **process**: the
``accept`` loop keeps running for new connections.  Whether the **same
connection** survives depends on the error's level:

- **Recoverable** errors leave the byte stream synchronised, so the server
  returns the :msg:`ERROR` and keeps serving the connection.  These are a
  malformed or unsupported :msg:`REQUEST` (codes ``1``--``6``, the offending
  frame having been read in full) and a ``COMPRESSED`` payload the server
  cannot decode (code ``9``): its length is bounded by ``max_payload``, so
  the server drains it and the peer retries uncompressed in-band.
- **Fatal** (framing- or handshake-level) errors leave the stream
  unresynchronisable, so the server MAY return the :msg:`ERROR` and then
  close that connection: a frame larger than ``max_payload`` (code ``7``,
  undrainable by definition), an unsupported protocol version (code ``8``,
  at the handshake), or any otherwise unreadable / desynchronised frame.
  The client then reconnects and re-handshakes.

.. _kcmcp-cancel:

``CANCEL``
^^^^^^^^^^

A :msg:`CANCEL` frame whose ``request_id`` names the running job asks the
server to abort it.  The server invokes its engine's interrupt hook and
replies with :msg:`ERROR` code ``5``.  Honouring it requires the server
to keep reading the connection while the job computes; a server that
cannot does not advertise the ``cancel`` feature, and its jobs run
uninterruptibly to completion.  This is the wire-level counterpart
of the cancel-polling the ProvSQL client must already do:
the socket client honours ``CHECK_FOR_INTERRUPTS()`` and
``statement_timeout`` while waiting, mirroring the cancel discipline in
:cfile:`external_tool.cpp`.

.. _kcmcp-progress:

``PROGRESS``
^^^^^^^^^^^^

A server that advertised ``progress`` MAY emit :msg:`PROGRESS` frames
(carrying the running job's ``request_id``) while it works on a job.
:msg:`PROGRESS` is first of all a **liveness signal**, not a quantified
completion measure: for the search-based counting and knowledge
compilation these backends do, there is no reliable "percent done", so
the protocol does **not** mandate one.  Its payload is a **free-form
UTF-8 JSON object**, engine-defined; the client treats it as **opaque**
-- surfacing it for display or logging, and using its *arrival* to
confirm the job is alive and to re-arm its deadline -- and MUST NOT
require any particular member.  An empty object ``{}`` is a valid bare
heartbeat.

A server MAY include any of these **optional, well-known** members, which
a client MAY surface if present but MUST NOT depend on:

.. list-table::
   :header-rows: 1
   :widths: 16 16 68

   * - Member
     - JSON type
     - Meaning
   * - ``phase``
     - string
     - A coarse, engine-defined stage label (e.g. ``"preprocess"``,
       ``"compile"``), shown to the user verbatim.
   * - ``elapsed_ms``
     - integer
     - Wall-clock milliseconds since the job started; monotonic.
   * - ``message``
     - string
     - A human-readable status line.
   * - ``fraction``
     - number in [0, 1]
     - A **best-effort** completion estimate.  Advisory only: it is often
       unavailable or non-monotonic for these problems, so it may be
       absent and a client must never treat it as authoritative.

A server SHOULD rate-limit :msg:`PROGRESS` (for example at most one every
few seconds) so it stays a heartbeat rather than a flood.

.. _kcmcp-liveness:

``PING`` / ``PONG`` / ``BYE``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:msg:`PING` / :msg:`PONG` keep an idle warm connection alive.  :msg:`BYE`
closes it gracefully.


A worked exchange
-----------------

One connection that counts, then -- after an idle keepalive -- compiles,
the CNF :math:`x_1 \vee x_2` over three variables
(``p cnf 3 1`` / ``1 2 0``).
Every frame is shown in full -- the 10-byte header fields followed by the
complete payload -- with no elision.  The model count and the d-DNNF are
the **actual output of a d4v2 backend** on this input (``s 6`` and a
three-node d-DNNF); integer header fields are big-endian, and
``payload_len`` is the exact byte count of the payload shown.

.. code-block:: text

   C -> S  HELLO     type=0x00 flags=0x00 request_id=0  payload_len=40
           payload (JSON, 40 bytes):
             {"kcmcp":[1,0],"client":"ProvSQL/1.8.0"}

   S -> C  HELLO     type=0x00 flags=0x00 request_id=0  payload_len=219
           payload (JSON, 219 bytes, one line):
             {"kcmcp":1,"engine":"example-kc/1.0","max_payload":1048576,"operations":["count","compile"],"input_formats":["dimacs-cnf"],"output_formats":{"count":["decimal"],"compile":["ddnnf-nnf"]},"features":["cancel","progress"]}

   C -> S  REQUEST   type=0x01 flags=0x00 request_id=1  payload_len=24
           payload (24 bytes):
             operation     = 0   (count)
             input_format  = 0   (dimacs-cnf)
             output_format = 0   (decimal)
             reserved      = 0
             options_len   = 2
             options       = {}                          (2 bytes)
             problem       = "p cnf 3 1\n1 2 0\n"         (16 bytes)

   S -> C  RESULT    type=0x02 flags=0x00 request_id=1  payload_len=31
           payload (31 bytes):
             result_format = 0   (decimal)
             reserved      = 0
             meta_len      = 26
             meta          = {"time_ms":2,"exact":true}
             result        = "6"                          (1 byte, the count)

   (the connection sits idle; the client sends a keepalive)

   C -> S  PING      type=0x06 flags=0x00 request_id=0  payload_len=0
           (no payload)

   S -> C  PONG      type=0x07 flags=0x00 request_id=0  payload_len=0
           (no payload)

   C -> S  REQUEST   type=0x01 flags=0x00 request_id=2  payload_len=24
           payload (24 bytes):
             operation     = 2   (compile)
             input_format  = 0   (dimacs-cnf)
             output_format = 4   (ddnnf-nnf)
             reserved      = 0
             options_len   = 2
             options       = {}
             problem       = "p cnf 3 1\n1 2 0\n"         (16 bytes)

   S -> C  RESULT    type=0x02 flags=0x00 request_id=2  payload_len=68
           payload (68 bytes):
             result_format = 4   (ddnnf-nnf)
             reserved      = 0
             meta_len      = 21
             meta          = {"nodes":3,"edges":3}
             result        = (43 bytes, the d-DNNF d4v2 dumped):
               o 1 0
               o 2 0
               t 3 0
               2 3 1 0
               2 3 -1 2 0
               1 2 0

   C -> S  BYE       type=0x08 flags=0x00 request_id=0  payload_len=0
           (no payload)

The same two frame types serve "count to a decimal" and "compile to a
d-DNNF"; only the ``operation`` and ``output_format`` bytes differ.
That is the central property: transport, framing, handshake, error and
cancellation are all independent of whether the backend counts or
compiles, and of which engine implements it.


ProvSQL's client and managed server
-----------------------------------

ProvSQL is a KCMCP *client*: when a knowledge-compilation request selects a
tool registered with ``kind = 'kcmcp'`` (see :doc:`the tool registry
</user/tool-registry>`), ``BooleanCircuit::compilation()`` compiles over a
socket through the in-extension client (``kcmcp_client.cpp``) instead of
spawning a CLI process.  It serialises the problem exactly as the CLI path does
(a Tseytin CNF, or a BC-S1.2 circuit when the record advertises
``circuit-bcs12``), sends one ``compile`` :msg:`REQUEST`, and parses the
``ddnnf-nnf`` :msg:`RESULT` back with the same ``parseDDNNF`` the temp-file path
uses -- so results are identical and any failure (connect, protocol, server
:msg:`ERROR`) falls back to the CLI path.

The client honours the protocol's **connection-for-life** rule: it keeps one
connection per backend, keyed by endpoint, reusing it across compilations so a
warm server's cross-query cache is not discarded (today it also just saves the
per-compile connect + handshake).  A *reused* connection that has gone stale
(the server respawned, or an idle link dropped) is transparently reconnected
once; a query cancel or ``statement_timeout`` closes the socket so the server
abandons the job, mirroring the cancel discipline of the CLI path; and an
``on_proc_exit`` hook sends :msg:`BYE` at backend exit.

A ``kcmcp`` record's ``endpoint`` is either a fixed address (``unix:/path`` or
``host:port`` -- *endpoint mode*, a server operated out of band) or the literal
``managed``, resolved at compile time to the address of a server ProvSQL itself
runs.

**Managed server.**  A dedicated supervisor background worker
(``kcmcp_supervisor.c``, registered alongside the mmap worker) owns the managed
server's lifecycle.  When :ref:`provsql.kcmcp_server <provsql-kcmcp-server>` is
non-empty it is a shell command with a ``{endpoint}`` placeholder; the worker
substitutes a Unix-socket path it chooses, forks/execs the command in its own
process group, publishes the endpoint in shared memory (read by the client for
an ``endpoint = 'managed'`` record), and supervises it -- relaunching on exit,
restarting a changed command on ``SIGHUP``, idling on its latch when the GUC is
empty, and killing the process group on shutdown.  The worker needs only
``BGWORKER_SHMEM_ACCESS`` (no database): the command is a process-global GUC and
the endpoint lives in the shared segment, which is why a cluster-level GUC -- not
a per-database registry column -- carries it.


Design notes and non-goals
--------------------------

- **No framing by sentinel.**  Length prefixes everywhere; the ``z``
  sentinel of the DIMACS dialect is tolerated inside ``problem`` but is
  never how a message boundary is found.
- **Forward compatibility.**  New operations, formats and features are
  added by extending the registries and the :msg:`HELLO` JSON.  Receivers
  reject unknown frame types and formats with an :msg:`ERROR`, never by
  crashing, so a newer client and an older server degrade predictably.
- **Concurrency.**  Per connection there is one in-flight :msg:`REQUEST`
  at a time (the ``request_id`` field is already present so that
  pipelining or multiplexing can be added later under a ``feature`` flag
  without a wire change), but the server still services control frames on
  that connection while the job computes (see `Connection lifecycle`_).
  How it schedules work *across* connections -- serialise or run
  concurrently -- is left to the implementation; the protocol does not
  constrain it.
- **Out of scope for v1:** authentication, transport encryption, and
  persistent cross-query caching.  A remote server is supported at the
  transport level, but securing the channel to it is left to the
  deployment (see *Transport*).  Persistent caching is contingent on
  upstream engine support; the warm, serialized, single-process server
  is the prerequisite that lets it be exploited for free if and when it
  lands.
