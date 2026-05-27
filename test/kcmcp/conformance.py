#!/usr/bin/env python3
"""KCMCP conformance check for the tdkc reference server.

Launches `tdkc --kcmcp unix:<sock>`, then drives the protocol end to end:
handshake, a `compile` request (-> ddnnf-nnf), a `wmc` request (-> decimal),
PING/PONG, and two error cases (unsupported operation / output format).  No
third-party dependencies; uses only the standard library.

Usage:  conformance.py /path/to/tdkc
Exit code 0 on success, 1 on any conformance failure.
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

# Frame types
HELLO, REQUEST, RESULT, ERROR, PROGRESS, CANCEL, PING, PONG, BYE = range(9)
FLAG_MORE = 0x01
# Registries
OP_COUNT, OP_WMC, OP_COMPILE = 0, 1, 2
IN_DIMACS = 0
OUT_DECIMAL, OUT_DDNNF_NNF = 0, 4
# Error codes
ERR_UNSUPPORTED_OP, ERR_UNSUPPORTED_FMT, ERR_CANCELLED = 1, 2, 5
ERR_UNSUPPORTED_VERSION, ERR_COMPRESSION = 8, 9
FLAG_COMPRESSED = 0x02

HEADER = struct.Struct(">BBII")  # type, flags, request_id, payload_len


class Conn:
    def __init__(self, sock):
        self.s = sock

    def send(self, typ, rid, payload=b"", flags=0):
        self.s.sendall(HEADER.pack(typ, flags, rid, len(payload)) + payload)

    def _readn(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.s.recv(n - len(buf))
            if not chunk:
                raise EOFError("server closed")
            buf += chunk
        return buf

    def recv(self):
        """Return (type, request_id, payload) reassembling MORE frames."""
        payload = b""
        while True:
            typ, flags, rid, plen = HEADER.unpack(self._readn(HEADER.size))
            payload += self._readn(plen) if plen else b""
            if not (flags & FLAG_MORE):
                return typ, rid, payload


def request_payload(op, infmt, outfmt, options, problem):
    opt = options.encode()
    return (struct.pack("BBBB", op, infmt, outfmt, 0)
            + struct.pack(">H", len(opt)) + opt + problem.encode())


def chain_cnf(nvars):
    """A path of binary clauses (x_i v x_{i+1}): low treewidth (so it compiles)
    but many gates, hence many interrupt-check points during the build."""
    lines = ["p cnf %d %d" % (nvars, nvars - 1)]
    lines += ["%d %d 0" % (i, i + 1) for i in range(1, nvars)]
    return "\n".join(lines) + "\n"


def expect(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)
    print("ok:", msg)


def connect(sockpath):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sockpath)
    return s, Conn(s)


def main():
    if len(sys.argv) != 2:
        print("usage: conformance.py /path/to/tdkc", file=sys.stderr)
        return 2
    tdkc = sys.argv[1]
    # Keep the socket path short: a sockaddr_un sun_path is only 104 bytes on
    # macOS/BSD (108 on Linux), and macOS's default $TMPDIR (/var/folders/...)
    # easily overruns it.  Prefer /tmp, which is short on every POSIX platform.
    base = "/tmp" if os.path.isdir("/tmp") else None
    d = tempfile.mkdtemp(prefix="kcmcp_", dir=base)
    sockpath = os.path.join(d, "s.sock")

    proc = subprocess.Popen([tdkc, "--kcmcp", "unix:" + sockpath],
                            stderr=subprocess.PIPE)
    try:
        # Wait for the listening line (or the socket to appear).
        deadline = time.time() + 10
        while time.time() < deadline and not os.path.exists(sockpath):
            time.sleep(0.02)
        expect(os.path.exists(sockpath), "server created the socket")

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sockpath)
        c = Conn(s)

        # --- handshake ---
        c.send(HELLO, 0, json.dumps({"kcmcp": [1, 0], "client": "conf/1"}).encode())
        typ, rid, payload = c.recv()
        expect(typ == HELLO and rid == 0, "server replies with HELLO")
        desc = json.loads(payload)
        expect(desc.get("kcmcp") == 1, "negotiated kcmcp version 1")
        expect(set(["compile", "wmc"]) <= set(desc.get("operations", [])),
               "advertises compile + wmc")
        expect(desc.get("output_formats", {}).get("compile") == ["ddnnf-nnf"],
               "compile -> ddnnf-nnf advertised")
        expect("cancel" in desc.get("features", []) and "progress" in desc["features"],
               "advertises cancel + progress features")
        expect(desc.get("max_payload", 0) >= 1 << 20, "max_payload >= 1 MiB")

        # --- compile: (x1 v x2) -> a d-DNNF in NNF text ---
        cnf = "p cnf 2 1\n1 2 0\n"
        c.send(REQUEST, 1, request_payload(OP_COMPILE, IN_DIMACS, OUT_DDNNF_NNF, "", cnf))
        typ, rid, payload = c.recv()
        expect(typ == RESULT and rid == 1, "compile returns a RESULT")
        result_format = payload[0]
        meta_len = struct.unpack(">H", payload[2:4])[0]
        meta = json.loads(payload[4:4 + meta_len])
        result = payload[4 + meta_len:]
        expect(result_format == OUT_DDNNF_NNF, "result_format is ddnnf-nnf")
        expect(result.startswith(b"nnf "), "compile result is NNF text")
        expect("treewidth" in meta, "compile meta carries treewidth")

        # --- wmc: P(x1 v x2), w(x1)=0.3, w(x2)=0.4  ->  0.58 ---
        wcnf = ("p cnf 2 1\n"
                "c p weight 1 0.3 0\nc p weight -1 0.7 0\n"
                "c p weight 2 0.4 0\nc p weight -2 0.6 0\n"
                "1 2 0\n")
        c.send(REQUEST, 2, request_payload(OP_WMC, IN_DIMACS, OUT_DECIMAL, "", wcnf))
        typ, rid, payload = c.recv()
        expect(typ == RESULT and rid == 2, "wmc returns a RESULT")
        meta_len = struct.unpack(">H", payload[2:4])[0]
        value = float(payload[4 + meta_len:])
        expect(payload[0] == OUT_DECIMAL, "wmc result_format is decimal")
        expect(abs(value - 0.58) < 1e-9, f"wmc value 0.58 (got {value})")

        # --- ping / pong ---
        c.send(PING, 7)
        typ, rid, _ = c.recv()
        expect(typ == PONG and rid == 7, "PING answered with PONG (same id)")

        # --- error: unsupported operation (count) ---
        c.send(REQUEST, 3, request_payload(OP_COUNT, IN_DIMACS, OUT_DECIMAL, "", cnf))
        typ, rid, payload = c.recv()
        code = struct.unpack(">H", payload[:2])[0]
        expect(typ == ERROR and code == ERR_UNSUPPORTED_OP,
               "count -> ERROR code 1 (unsupported operation)")

        # --- error: unsupported output for compile (decimal) ---
        c.send(REQUEST, 4, request_payload(OP_COMPILE, IN_DIMACS, OUT_DECIMAL, "", cnf))
        typ, rid, payload = c.recv()
        code = struct.unpack(">H", payload[:2])[0]
        expect(typ == ERROR and code == ERR_UNSUPPORTED_FMT,
               "compile->decimal -> ERROR code 2 (unsupported format)")

        # server stays up after errors: one more good request
        c.send(REQUEST, 5, request_payload(OP_COMPILE, IN_DIMACS, OUT_DDNNF_NNF, "", cnf))
        typ, rid, payload = c.recv()
        expect(typ == RESULT and rid == 5, "server keeps serving after errors")

        # --- progress: progress_every_ms=0 makes the server emit a PROGRESS on
        #     the first interrupt poll, so we observe it deterministically
        #     without depending on a multi-second build. ---
        c.send(REQUEST, 10, request_payload(OP_COMPILE, IN_DIMACS, OUT_DDNNF_NNF,
               '{"progress_every_ms":0}', chain_cnf(20)))
        saw_progress = False
        while True:
            typ, rid, payload = c.recv()
            if typ == PROGRESS:
                prog = json.loads(payload)
                expect(rid == 10 and "phase" in prog,
                       "PROGRESS frame carries the request id and a phase")
                saw_progress = True
                continue
            break
        expect(saw_progress, "progress_every_ms=0 yields at least one PROGRESS frame")
        expect(typ == RESULT and rid == 10, "the progress run still ends in a RESULT")

        # --- cancel: pipeline CANCEL right behind the REQUEST so it is already
        #     buffered when the build's first interrupt poll runs; the build is
        #     aborted regardless of how fast it would otherwise complete. ---
        c.send(REQUEST, 11, request_payload(OP_COMPILE, IN_DIMACS, OUT_DDNNF_NNF,
               "", chain_cnf(60)))
        c.send(CANCEL, 11)
        while True:
            typ, rid, payload = c.recv()
            if typ == PROGRESS:   # tolerate a stray progress frame before the abort
                continue
            break
        code = struct.unpack(">H", payload[:2])[0]
        expect(typ == ERROR and rid == 11, "a cancelled request is answered with ERROR")
        expect(code == ERR_CANCELLED,
               f"cancel -> ERROR code {ERR_CANCELLED} (cancelled); got {code}")

        # server still serves after a cancel
        c.send(REQUEST, 12, request_payload(OP_COMPILE, IN_DIMACS, OUT_DDNNF_NNF, "", cnf))
        typ, rid, payload = c.recv()
        expect(typ == RESULT and rid == 12, "server keeps serving after a cancel")

        # --- compression: the server cannot decode a COMPRESSED payload, but it
        #     is length-bounded, so the server drains it, answers ERROR 9
        #     (echoing the request id), and keeps the connection open -- the
        #     client falls back to an uncompressed retry in-band. ---
        c.send(REQUEST, 13,
               request_payload(OP_COMPILE, IN_DIMACS, OUT_DDNNF_NNF, "", cnf),
               flags=FLAG_COMPRESSED)
        typ, rid, payload = c.recv()
        code = struct.unpack(">H", payload[:2])[0]
        expect(typ == ERROR and rid == 13 and code == ERR_COMPRESSION,
               f"COMPRESSED frame -> ERROR code {ERR_COMPRESSION} on id 13 (got {code}/{rid})")
        c.send(REQUEST, 14,  # uncompressed retry on the same connection
               request_payload(OP_COMPILE, IN_DIMACS, OUT_DDNNF_NNF, "", cnf))
        typ, rid, payload = c.recv()
        expect(typ == RESULT and rid == 14,
               "uncompressed retry succeeds in-band after ERROR 9")

        c.send(BYE, 0)
        s.close()

        # --- version: a client requiring a newer (breaking) major is rejected
        #     at the handshake with ERROR code 8, on a fresh connection. ---
        s2, c2 = connect(sockpath)
        c2.send(HELLO, 0, json.dumps({"kcmcp": [2, 0], "client": "conf/1"}).encode())
        typ, rid, payload = c2.recv()
        code = struct.unpack(">H", payload[:2])[0]
        expect(typ == ERROR and code == ERR_UNSUPPORTED_VERSION,
               f"client HELLO major 2 -> ERROR code {ERR_UNSUPPORTED_VERSION} (got {code})")
        s2.close()
        print("\nKCMCP conformance: PASS")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
