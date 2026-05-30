/**
 * @file kcmcp_client.cpp
 * @brief Implementation of the in-extension KCMCP client (see kcmcp_client.h).
 */
extern "C" {
#include "postgres.h"
#include "miscadmin.h"
#include "storage/ipc.h"          /* on_proc_exit */

#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
}

// PostgreSQL's elog.h defines ERROR (and other log levels) as macros, which
// would clobber the kcmcp::Type::ERROR enumerator below.  We use the provsql
// error macros, not bare elog levels, in this file, so dropping ERROR is safe.
#undef ERROR

#include "kcmcp_client.h"
#include "kcmcp_protocol.h"
#include "provsql_config.h"

#include <stdexcept>
#include <string>

using namespace kcmcp;

namespace {

// Largest RESULT (compiled d-DNNF) we will accept from the server.
constexpr uint32_t CLIENT_RECV_MAX = 256u * 1024 * 1024;
// Split our outbound problem into MORE frames at the 1 MiB interoperability
// floor, so any conformant server accepts it without advertising a larger
// max_payload (which we do not parse from its HELLO).
constexpr uint32_t CLIENT_SEND_MAX = 1u * 1024 * 1024;

// Connect to "unix:/path" or "host:port"; returns a connected fd or -1.
int connect_endpoint(const std::string &endpoint)
{
#ifdef PROVSQL_NO_SUBPROCESS
  /* No sockets in the WASM sandbox: report "no connection" so the KCMCP
     path falls back to the CLI compilers (also absent) and ultimately to
     the in-process tree-decomposition compiler.  A remote KCMCP-over-
     WebSocket transport is a separate, opt-in addition. */
  (void) endpoint;
  return -1;
#else
  if (endpoint.rfind("unix:", 0) == 0) {
    std::string path = endpoint.substr(5);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      ::close(fd);
      return -1;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      ::close(fd);
      return -1;
    }
    return fd;
  }

  auto colon = endpoint.rfind(':');
  if (colon == std::string::npos)
    return -1;
  std::string host = endpoint.substr(0, colon), port = endpoint.substr(colon + 1);
  struct addrinfo hints, *res = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
    return -1;
  int fd = -1;
  for (auto *ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
      continue;
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
      break;
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
#endif /* PROVSQL_NO_SUBPROCESS */
}

uint16_t get_u16(const std::string &s, size_t off)
{
  return (uint16_t(static_cast<unsigned char>(s[off])) << 8)
       | uint16_t(static_cast<unsigned char>(s[off + 1]));
}

// A job-level ERROR frame from the server (codes 1-6): a valid response on a
// healthy, synchronised connection, distinct from an I/O / protocol failure --
// so the caller propagates it without dropping or retrying the connection.
struct ServerError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// --- Per-backend cached connection ---------------------------------------
// KCMCP mandates one connection for the session's life so the server's warm
// cross-query cache is not discarded; today it also saves the per-compile
// connect + HELLO round-trip.  A backend is single-threaded and compiles one
// circuit at a time, so a single cached connection (not a pool) suffices.
int g_fd = -1;                 // cached connection fd, or -1 when none
std::string g_endpoint;        // endpoint g_fd is connected to
uint32_t g_request_id = 0;     // monotonically increasing REQUEST id
bool g_atexit_registered = false;

void close_cached()
{
  if (g_fd >= 0)
    ::close(g_fd);
  g_fd = -1;
  g_endpoint.clear();
}

// on_proc_exit hook: gracefully BYE and close the cached connection at backend
// exit.  Best-effort and must not throw (it runs during shutdown); the OS would
// close the fd regardless, this just lets the server release the session early.
void kcmcp_atexit(int code, Datum arg)
{
  (void) code;
  (void) arg;
  if (g_fd >= 0) {
    unsigned char bye[10] = { 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0 };  // BYE, no payload
    ssize_t n = ::write(g_fd, bye, sizeof(bye));
    (void) n;
    ::close(g_fd);
    g_fd = -1;
  }
}

// Block until the cached socket is readable, servicing PostgreSQL cancel /
// terminate while we wait.  A longjmp out of CHECK_FOR_INTERRUPTS() skips C++
// destructors, so -- exactly as run_in_own_pgroup does -- we detect a pending
// cancel ourselves, close the connection first (the server sees EOF and
// abandons the job, and the cache is left clean for the next statement), then
// let CHECK_FOR_INTERRUPTS() raise it.
void wait_readable_or_cancel()
{
  for (;;) {
    struct pollfd pfd;
    pfd.fd = g_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int r = ::poll(&pfd, 1, 100);
    if (r > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)))
      return;
    if (r < 0 && errno != EINTR)
      return;  // let the subsequent recv surface the error
    if (QueryCancelPending || ProcDiePending) {
      close_cached();
      CHECK_FOR_INTERRUPTS();  // raises; connection already dropped
      return;                  // unreached if it raised
    }
  }
}

// Ensure g_fd is a handshaken connection to @p endpoint, reusing the cached one
// when it matches.  Throws (and leaves g_fd == -1) if it cannot connect or
// handshake.
void ensure_connection(const std::string &endpoint)
{
  if (g_fd >= 0 && g_endpoint == endpoint)
    return;  // reuse the warm connection
  close_cached();

  int fd = connect_endpoint(endpoint);
  if (fd < 0)
    throw std::runtime_error("cannot connect to KCMCP endpoint '" + endpoint + "'");
  try {
    Connection conn(fd, CLIENT_RECV_MAX, CLIENT_SEND_MAX);
    conn.send(Type::HELLO, 0, "{\"kcmcp\":[1,0],\"client\":\"ProvSQL\"}");
    Message m;
    if (!conn.recv(m))
      throw std::runtime_error("KCMCP server closed during handshake");
    if (m.type == Type::ERROR)
      throw std::runtime_error("KCMCP handshake refused: "
                               + (m.payload.size() > 2 ? m.payload.substr(2) : ""));
    if (m.type != Type::HELLO)
      throw std::runtime_error("KCMCP: expected HELLO from server");
  } catch (...) {
    ::close(fd);
    throw;
  }
  g_fd = fd;
  g_endpoint = endpoint;
}

// Issue one compile REQUEST on the cached connection and return the d-DNNF.
std::string do_compile(uint8_t input_format, const std::string &problem)
{
  Connection conn(g_fd, CLIENT_RECV_MAX, CLIENT_SEND_MAX);

  std::string req;
  req.push_back(static_cast<char>(2));             // operation: compile
  req.push_back(static_cast<char>(input_format));  // 0 dimacs-cnf / 1 circuit-bcs12
  req.push_back(static_cast<char>(4));             // output_format: ddnnf-nnf
  req.push_back(0);                                // reserved
  req.push_back(0);                                // options_len hi
  req.push_back(0);                                // options_len lo
  req += problem;
  conn.send(Type::REQUEST, ++g_request_id, req);

  // Read frames until the RESULT, skipping PROGRESS heartbeats; honour
  // cancel/timeout while the server computes.
  Message m;
  for (;;) {
    wait_readable_or_cancel();
    if (!conn.recv(m))
      throw std::runtime_error("KCMCP server closed before RESULT");
    if (m.type == Type::PROGRESS)
      continue;
    if (m.type == Type::ERROR) {
      uint16_t code = m.payload.size() >= 2 ? get_u16(m.payload, 0) : 0;
      std::string msg = m.payload.size() > 2 ? m.payload.substr(2) : "";
      throw ServerError("KCMCP server error " + std::to_string(code)
                        + ": " + msg);
    }
    if (m.type == Type::RESULT)
      break;
    throw std::runtime_error("KCMCP: unexpected frame type in reply");
  }

  // RESULT payload: result_format u8, reserved u8, meta_len u16, meta, result.
  if (m.payload.size() < 4)
    throw std::runtime_error("KCMCP: truncated RESULT");
  if (static_cast<unsigned char>(m.payload[0]) != 4)
    throw std::runtime_error("KCMCP: server returned a non-ddnnf-nnf result");
  uint16_t meta_len = get_u16(m.payload, 2);
  if (4u + meta_len > m.payload.size())
    throw std::runtime_error("KCMCP: malformed RESULT meta");
  return m.payload.substr(4 + meta_len);
}

}  // namespace

namespace provsql {

std::string kcmcp_compile(const std::string &endpoint, uint8_t input_format,
                          const std::string &problem)
{
  // SIGPIPE would otherwise kill the backend if the server vanishes mid-send.
  ::signal(SIGPIPE, SIG_IGN);
  if (!g_atexit_registered) {
    on_proc_exit(kcmcp_atexit, (Datum) 0);
    g_atexit_registered = true;
  }

  // Use the cached connection; if a *reused* one fails (server respawned or an
  // idle link dropped), reconnect once on a fresh connection and retry.  A
  // failure on a connection we just opened means the server is unreachable, so
  // we give up (the caller falls back to the CLI path).  A server ERROR frame
  // is a healthy-connection response, so it is propagated without a retry.
  for (int attempt = 0; ; ++attempt) {
    bool reusing = (g_fd >= 0 && g_endpoint == endpoint);
    try {
      ensure_connection(endpoint);
      return do_compile(input_format, problem);
    } catch (const ServerError &) {
      throw;
    } catch (const std::exception &) {
      close_cached();
      if (reusing && attempt == 0)
        continue;
      throw;
    }
  }
}

}  // namespace provsql
