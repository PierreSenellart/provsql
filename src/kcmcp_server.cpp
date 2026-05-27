/**
 * @file kcmcp_server.cpp
 * @brief Implementation of the tdkc KCMCP reference server (see
 *        kcmcp_server.h and doc/source/dev/kc-server-protocol.rst).
 */
#include "kcmcp_server.h"
#include "kcmcp_protocol.h"
#include "dimacs_cnf.h"
#include "tdkc_interrupt.h"

#include "BooleanCircuit.h"
#include "dDNNF.h"
#include "dDNNFTreeDecompositionBuilder.h"
#include "TreeDecomposition.h"
#include "Circuit.hpp"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

extern "C" {
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
}

using namespace kcmcp;
using steady = std::chrono::steady_clock;

namespace {

// Thrown from the poll hook to abort a running build.
struct Cancelled {};
struct TimedOut {};

constexpr uint32_t SERVER_MAX_PAYLOAD = 256u * 1024 * 1024;  // advertised
constexpr uint32_t CLIENT_FLOOR       = 1u * 1024 * 1024;    // client's 1 MiB
constexpr long     KCMCP_MAJOR        = 1;   // protocol major this server speaks

// Context of the request currently being served, consulted by
// provsql_tdkc_poll() (which the build loops call via CHECK_FOR_INTERRUPTS).
struct ActiveJob {
  Connection *conn = nullptr;
  uint32_t request_id = 0;
  bool cancel = false;
  steady::time_point start;
  steady::time_point last_progress;
  steady::time_point deadline;
  bool has_deadline = false;
  std::chrono::milliseconds progress_interval{2000};
};
ActiveJob *g_job = nullptr;

// The major version from a client HELLO's "kcmcp":[major,minor]; -1 if the
// field is absent or unparseable (treated leniently as compatible).
long client_major(const std::string &hello_json)
{
  try {
    boost::property_tree::ptree pt;
    std::istringstream is(hello_json);
    boost::property_tree::read_json(is, pt);
    auto kc = pt.get_child_optional("kcmcp");
    if (!kc)
      return -1;
    for (const auto &elem : *kc)        // JSON array: elements have empty keys
      return elem.second.get_value<long>();   // first element is the major
    return -1;
  } catch (...) {
    return -1;
  }
}

std::string server_hello()
{
  std::ostringstream o;
  o << "{\"kcmcp\":1,\"engine\":\"tdkc\",\"max_payload\":" << SERVER_MAX_PAYLOAD
    << ",\"operations\":[\"compile\",\"wmc\"]"
    << ",\"input_formats\":[\"dimacs-cnf\"]"
    << ",\"output_formats\":{\"compile\":[\"ddnnf-nnf\"],\"wmc\":[\"decimal\"]}"
    << ",\"features\":[\"cancel\",\"progress\"]}";
  return o.str();
}

// Read an integer option from the REQUEST options JSON, returning @p def when
// the blob is empty, malformed, or lacks the key (ignore-unknown semantics).
long option_long(const std::string &options, const char *key, long def)
{
  if (options.empty())
    return def;
  try {
    boost::property_tree::ptree pt;
    std::istringstream is(options);
    boost::property_tree::read_json(is, pt);
    return pt.get<long>(key, def);
  } catch (...) {
    return def;  // a malformed options block is treated as {}
  }
}

void send_error(Connection &conn, uint32_t rid, ErrorCode code,
                const std::string &msg)
{
  conn.send(Type::ERROR, rid, build_error(code, msg));
}

// Map an input gate's UUID ("v<n>") back to its CNF variable index.
int var_of_input_uuid(const std::string &u)
{
  if (u.size() > 1 && u[0] == 'v') {
    try { return std::stoi(u.substr(1)); } catch (...) {}
  }
  return -1;  // unknown -> toNNF falls back to the gate id
}

void handle_request(Connection &conn, const Message &msg)
{
  Request req;
  if (!parse_request(msg.payload, req)) {
    send_error(conn, msg.request_id, ErrorCode::PARSE, "malformed REQUEST payload");
    return;
  }
  if (req.operation != Operation::COMPILE && req.operation != Operation::WMC) {
    send_error(conn, msg.request_id, ErrorCode::UNSUPPORTED_OPERATION,
               std::string("unsupported operation '") + operation_name(req.operation)
               + "'; this engine offers compile and wmc");
    return;
  }
  if (req.input_format != InputFormat::DIMACS_CNF) {
    send_error(conn, msg.request_id, ErrorCode::UNSUPPORTED_FORMAT,
               std::string("unsupported input '") + input_format_name(req.input_format)
               + "'; this engine reads dimacs-cnf");
    return;
  }
  const bool weighted = (req.operation == Operation::WMC);
  const OutputFormat want = weighted ? OutputFormat::DECIMAL : OutputFormat::DDNNF_NNF;
  if (req.output_format != want) {
    send_error(conn, msg.request_id, ErrorCode::UNSUPPORTED_FORMAT,
               std::string("operation '") + operation_name(req.operation)
               + "' produces '" + output_format_name(want) + "' here");
    return;
  }

  BooleanCircuit c;
  gate_t root;
  try {
    root = parse_dimacs_cnf(req.problem, c, weighted);
  } catch (const std::exception &e) {
    send_error(conn, msg.request_id, ErrorCode::PARSE, e.what());
    return;
  }

  ActiveJob job;
  job.conn = &conn;
  job.request_id = msg.request_id;
  job.start = job.last_progress = steady::now();
  long timeout_ms = option_long(req.options, "timeout_ms", 0);
  if (timeout_ms > 0) {
    job.deadline = job.start + std::chrono::milliseconds(timeout_ms);
    job.has_deadline = true;
  }
  // PROGRESS cadence, in milliseconds (default 2000); 0 = emit on every poll.
  job.progress_interval =
      std::chrono::milliseconds(std::max(0L, option_long(req.options,
                                                         "progress_every_ms", 2000)));
  g_job = &job;

  try {
    TreeDecomposition td(c);
    auto dnnf = dDNNFTreeDecompositionBuilder{c, root, td}.build();
    g_job = nullptr;

    std::ostringstream meta;
    if (req.operation == Operation::COMPILE) {
      std::string nnf = dnnf.toNNF(var_of_input_uuid);
      meta << "{\"treewidth\":" << td.getTreewidth()
           << ",\"nodes\":" << dnnf.getNbGates() << ",\"exact\":true}";
      conn.send(Type::RESULT, msg.request_id,
                build_result(OutputFormat::DDNNF_NNF, meta.str(), nnf));
    } else {
      double p = dnnf.probabilityEvaluation();
      meta << "{\"treewidth\":" << td.getTreewidth() << ",\"exact\":true}";
      std::ostringstream val;
      val << std::setprecision(15) << p;
      conn.send(Type::RESULT, msg.request_id,
                build_result(OutputFormat::DECIMAL, meta.str(), val.str()));
    }
  } catch (const Cancelled &) {
    g_job = nullptr;
    send_error(conn, msg.request_id, ErrorCode::CANCELLED, "cancelled at client request");
  } catch (const TimedOut &) {
    g_job = nullptr;
    send_error(conn, msg.request_id, ErrorCode::TIMEOUT,
               "time budget exceeded (timeout_ms)");
  } catch (const TreeDecompositionException &) {
    g_job = nullptr;
    send_error(conn, msg.request_id, ErrorCode::INTERNAL,
               "treewidth exceeds the supported bound");
  } catch (const std::exception &e) {
    g_job = nullptr;
    send_error(conn, msg.request_id, ErrorCode::INTERNAL, e.what());
  }
}

void run_session(int fd)
{
  Connection conn(fd, SERVER_MAX_PAYLOAD, CLIENT_FLOOR);

  // Handshake: read the client HELLO, reply with ours.
  Message m;
  try {
    if (!conn.recv(m))
      return;
  } catch (...) {
    return;
  }
  if (m.type != Type::HELLO) {
    try {
      send_error(conn, m.request_id, ErrorCode::UNSUPPORTED_OPERATION,
                 "expected HELLO as the first frame");
    } catch (...) {}
    return;
  }
  // A major bump is a breaking change: if the client requires a newer major
  // than we implement there is no shared version, so we do not silently
  // downgrade -- we reject and let the client fall back.
  long major = client_major(m.payload);
  if (major > KCMCP_MAJOR) {
    try {
      send_error(conn, 0, ErrorCode::UNSUPPORTED_VERSION,
                 "this server speaks KCMCP major " + std::to_string(KCMCP_MAJOR)
                 + "; client requires major " + std::to_string(major));
    } catch (...) {}
    return;
  }
  try {
    conn.send(Type::HELLO, 0, server_hello());
  } catch (...) {
    return;
  }

  // Request/response loop on the same connection.
  for (;;) {
    try {
      if (!conn.recv(m))
        return;  // clean close
    } catch (const ProtocolError &e) {
      // Non-fatal (e.g. an undecodable but length-bounded COMPRESSED frame):
      // the stream stayed synchronised, so answer -- echoing the offending
      // frame's request_id -- and keep serving.  Fatal: the stream is
      // desynchronised, so close after the ERROR.
      try { send_error(conn, e.fatal ? 0 : m.request_id, e.code, e.what()); }
      catch (...) { return; }
      if (e.fatal)
        return;
      continue;
    } catch (...) {
      return;
    }
    switch (m.type) {
      case Type::REQUEST:
        handle_request(conn, m);
        break;
      case Type::PING:
        conn.send(Type::PONG, m.request_id);
        break;
      case Type::CANCEL:
        break;  // no job is running between requests
      case Type::BYE:
        return;
      default:
        send_error(conn, m.request_id, ErrorCode::UNSUPPORTED_OPERATION,
                   "unexpected frame type");
    }
  }
}

int listen_unix(const std::string &path)
{
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) { perror("socket"); return -1; }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    fprintf(stderr, "tdkc: socket path too long\n");
    return -1;
  }
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  ::unlink(path.c_str());  // clear a stale socket file
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    perror("bind"); return -1;
  }
  return fd;
}

int listen_tcp(const std::string &host, const std::string &port)
{
  struct addrinfo hints, *res = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
    fprintf(stderr, "tdkc: cannot resolve %s:%s\n", host.c_str(), port.c_str());
    return -1;
  }
  int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) { perror("socket"); freeaddrinfo(res); return -1; }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (::bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
    perror("bind"); freeaddrinfo(res); ::close(fd); return -1;
  }
  freeaddrinfo(res);
  // Report the actual port (so an ephemeral "host:0" is discoverable).
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&ss), &sl) == 0) {
    char hbuf[NI_MAXHOST], pbuf[NI_MAXSERV];
    if (::getnameinfo(reinterpret_cast<sockaddr *>(&ss), sl, hbuf, sizeof(hbuf),
                      pbuf, sizeof(pbuf), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
      fprintf(stderr, "tdkc: bound %s:%s\n", hbuf, pbuf);
  }
  return fd;
}

} // namespace

// Build-loop interrupt hook (declared in tdkc_interrupt.h).  Services the
// active connection mid-job and aborts the build on cancel/timeout.
void provsql_tdkc_poll()
{
  ActiveJob *job = g_job;
  if (!job)
    return;

  struct pollfd pfd;
  pfd.fd = job->conn->fd();
  pfd.events = POLLIN;
  pfd.revents = 0;
  if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
    Message m;
    try {
      if (job->conn->recv(m)) {
        if (m.type == Type::CANCEL && m.request_id == job->request_id)
          job->cancel = true;
        else if (m.type == Type::PING)
          job->conn->send(Type::PONG, m.request_id);
        // other frames mid-job are ignored (one in-flight request)
      } else {
        job->cancel = true;  // peer closed: abandon the job
      }
    } catch (...) {
      job->cancel = true;
    }
  }

  auto now = steady::now();
  if (job->cancel)
    throw Cancelled{};
  if (job->has_deadline && now >= job->deadline)
    throw TimedOut{};
  if (now - job->last_progress >= job->progress_interval) {
    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - job->start).count();
    try {
      job->conn->send(Type::PROGRESS, job->request_id,
                      "{\"phase\":\"compile\",\"elapsed_ms\":" + std::to_string(ms) + "}");
    } catch (...) {
      job->cancel = true;
      throw Cancelled{};
    }
    job->last_progress = now;
  }
}

int kcmcp_serve(const std::string &endpoint)
{
  ::signal(SIGPIPE, SIG_IGN);  // a peer vanishing mid-send must not kill us

  int lfd;
  if (endpoint.rfind("unix:", 0) == 0) {
    lfd = listen_unix(endpoint.substr(5));
  } else {
    auto colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
      fprintf(stderr, "tdkc: endpoint must be unix:/path or host:port\n");
      return 1;
    }
    lfd = listen_tcp(endpoint.substr(0, colon), endpoint.substr(colon + 1));
  }
  if (lfd < 0)
    return 1;
  if (::listen(lfd, 16) < 0) {
    perror("listen"); ::close(lfd); return 1;
  }
  fprintf(stderr, "tdkc: KCMCP server listening on %s\n", endpoint.c_str());
  fflush(stderr);

  for (;;) {
    int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) {
      if (errno == EINTR) continue;
      perror("accept");
      break;
    }
    run_session(cfd);
    ::close(cfd);
    g_job = nullptr;  // defensive: never leak a stale job across sessions
  }
  ::close(lfd);
  return 0;
}
