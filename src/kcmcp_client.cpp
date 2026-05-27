/**
 * @file kcmcp_client.cpp
 * @brief Implementation of the in-extension KCMCP client (see kcmcp_client.h).
 */
extern "C" {
#include "postgres.h"
#include "miscadmin.h"

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
}

// Block until @p fd is readable, servicing PostgreSQL cancel/terminate while
// we wait.  A longjmp out of CHECK_FOR_INTERRUPTS() would skip the C++
// destructors that close the socket, so -- exactly as run_in_own_pgroup does
// -- we detect a pending cancel ourselves, close the socket first (the server
// sees EOF and abandons the job), then let CHECK_FOR_INTERRUPTS() raise it.
void wait_readable_or_cancel(int &fd)
{
  for (;;) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int r = ::poll(&pfd, 1, 100);
    if (r > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)))
      return;
    if (r < 0 && errno != EINTR)
      return;  // let the subsequent recv surface the error
    if (QueryCancelPending || ProcDiePending) {
      ::close(fd);
      fd = -1;
      CHECK_FOR_INTERRUPTS();  // raises; socket already closed
      return;                  // unreached if it raised
    }
  }
}

uint16_t get_u16(const std::string &s, size_t off)
{
  return (uint16_t(static_cast<unsigned char>(s[off])) << 8)
       | uint16_t(static_cast<unsigned char>(s[off + 1]));
}

}  // namespace

namespace provsql {

std::string kcmcp_compile(const std::string &endpoint, uint8_t input_format,
                          const std::string &problem)
{
  // SIGPIPE would otherwise kill the backend if the server vanishes mid-send.
  ::signal(SIGPIPE, SIG_IGN);

  int fd = connect_endpoint(endpoint);
  if (fd < 0)
    throw std::runtime_error("cannot connect to KCMCP endpoint '" + endpoint + "'");
  // Closes the socket on any C++ exception path; the cancel path closes it
  // explicitly before raising (see wait_readable_or_cancel).
  struct FdGuard {
    int &fd;
    ~FdGuard() { if (fd >= 0) { ::close(fd); fd = -1; } }
  } guard{fd};

  Connection conn(fd, CLIENT_RECV_MAX, CLIENT_SEND_MAX);

  // Handshake: announce our version, read the server's HELLO (or an ERROR,
  // e.g. an unsupported protocol version).
  conn.send(Type::HELLO, 0, "{\"kcmcp\":[1,0],\"client\":\"ProvSQL\"}");
  Message m;
  if (!conn.recv(m))
    throw std::runtime_error("KCMCP server closed during handshake");
  if (m.type == Type::ERROR)
    throw std::runtime_error("KCMCP handshake refused: "
                             + (m.payload.size() > 2 ? m.payload.substr(2) : ""));
  if (m.type != Type::HELLO)
    throw std::runtime_error("KCMCP: expected HELLO from server");

  // One compile REQUEST: operation=2 (compile), output=4 (ddnnf-nnf), no
  // options; payload = header bytes + the problem.
  std::string req;
  req.push_back(static_cast<char>(2));             // operation: compile
  req.push_back(static_cast<char>(input_format));  // 0 dimacs-cnf / 1 circuit-bcs12
  req.push_back(static_cast<char>(4));             // output_format: ddnnf-nnf
  req.push_back(0);                                // reserved
  req.push_back(0);                                // options_len hi
  req.push_back(0);                                // options_len lo
  req += problem;
  conn.send(Type::REQUEST, 1, req);

  // Read frames until the RESULT, skipping PROGRESS heartbeats; honour
  // cancel/timeout while the server computes.
  for (;;) {
    wait_readable_or_cancel(fd);
    if (!conn.recv(m))
      throw std::runtime_error("KCMCP server closed before RESULT");
    if (m.type == Type::PROGRESS)
      continue;
    if (m.type == Type::ERROR) {
      uint16_t code = m.payload.size() >= 2 ? get_u16(m.payload, 0) : 0;
      std::string msg = m.payload.size() > 2 ? m.payload.substr(2) : "";
      throw std::runtime_error("KCMCP server error " + std::to_string(code)
                               + ": " + msg);
    }
    if (m.type == Type::RESULT)
      break;
    throw std::runtime_error("KCMCP: unexpected frame type in reply");
  }

  // RESULT payload: result_format u8, reserved u8, meta_len u16, meta, result.
  if (m.payload.size() < 4)
    throw std::runtime_error("KCMCP: truncated RESULT");
  uint8_t result_format = static_cast<unsigned char>(m.payload[0]);
  if (result_format != 4)
    throw std::runtime_error("KCMCP: server returned a non-ddnnf-nnf result");
  uint16_t meta_len = get_u16(m.payload, 2);
  if (4u + meta_len > m.payload.size())
    throw std::runtime_error("KCMCP: malformed RESULT meta");

  std::string nnf = m.payload.substr(4 + meta_len);
  conn.send(Type::BYE, 0);
  return nnf;
}

}  // namespace provsql
