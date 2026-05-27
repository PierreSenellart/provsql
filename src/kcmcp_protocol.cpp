/**
 * @file kcmcp_protocol.cpp
 * @brief Implementation of the KCMCP wire codec (see kcmcp_protocol.h).
 */
#include "kcmcp_protocol.h"

#include <cerrno>
#include <cstring>

extern "C" {
#include <unistd.h>
}

namespace kcmcp {

const char *operation_name(Operation op)
{
  switch (op) {
    case Operation::COUNT:   return "count";
    case Operation::WMC:     return "wmc";
    case Operation::COMPILE: return "compile";
  }
  return "?";
}

const char *input_format_name(InputFormat fmt)
{
  switch (fmt) {
    case InputFormat::DIMACS_CNF:    return "dimacs-cnf";
    case InputFormat::CIRCUIT_BCS12: return "circuit-bcs12";
  }
  return "?";
}

const char *output_format_name(OutputFormat fmt)
{
  switch (fmt) {
    case OutputFormat::DECIMAL:   return "decimal";
    case OutputFormat::RATIONAL:  return "rational";
    case OutputFormat::DOUBLE:    return "double";
    case OutputFormat::BIGINT:    return "bigint";
    case OutputFormat::DDNNF_NNF: return "ddnnf-nnf";
  }
  return "?";
}

namespace {

constexpr size_t HEADER_LEN = 10;

void put_u32(unsigned char *p, uint32_t v)
{
  p[0] = (v >> 24) & 0xff;
  p[1] = (v >> 16) & 0xff;
  p[2] = (v >> 8) & 0xff;
  p[3] = v & 0xff;
}

uint32_t get_u32(const unsigned char *p)
{
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
       | (uint32_t(p[2]) << 8)  | uint32_t(p[3]);
}

/// Read exactly @p n bytes.  Returns false on a clean EOF before any byte was
/// read (so the caller can distinguish "peer closed" from "truncated frame").
bool read_exact(int fd, void *buf, size_t n, bool &eof_at_start)
{
  eof_at_start = false;
  size_t got = 0;
  unsigned char *p = static_cast<unsigned char *>(buf);
  while (got < n) {
    ssize_t r = ::read(fd, p + got, n - got);
    if (r == 0) {
      if (got == 0) { eof_at_start = true; return false; }
      throw std::runtime_error("KCMCP: truncated frame (peer closed mid-message)");
    }
    if (r < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("KCMCP: read failed: ") + strerror(errno));
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

void write_all(int fd, const void *buf, size_t n)
{
  size_t sent = 0;
  const unsigned char *p = static_cast<const unsigned char *>(buf);
  while (sent < n) {
    ssize_t w = ::write(fd, p + sent, n - sent);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("KCMCP: write failed: ") + strerror(errno));
    }
    sent += static_cast<size_t>(w);
  }
}

} // namespace

bool Connection::recv(Message &out)
{
  out.payload.clear();
  bool first = true;
  bool compressed = false;
  for (;;) {
    unsigned char hdr[HEADER_LEN];
    bool eof_at_start;
    if (!read_exact(fd_, hdr, HEADER_LEN, eof_at_start)) {
      if (eof_at_start && first)
        return false;  // clean close at a message boundary
      throw std::runtime_error("KCMCP: truncated frame header");
    }
    Type type = static_cast<Type>(hdr[0]);
    uint8_t flags = hdr[1];
    uint32_t request_id = get_u32(hdr + 2);
    uint32_t payload_len = get_u32(hdr + 6);

    if (payload_len > recv_max_)
      throw ProtocolError(ErrorCode::PAYLOAD_TOO_LARGE,
        "KCMCP: frame payload " + std::to_string(payload_len)
        + " exceeds max_payload " + std::to_string(recv_max_));

    // KCMCP v1 negotiates no compression, so a COMPRESSED payload cannot be
    // decoded.  Its length is bounded by max_payload, so we still read it to
    // keep the stream synchronised, then report a *non-fatal* error: the
    // caller answers with an ERROR (code 9) and keeps serving, letting the
    // peer retry uncompressed on the same connection.
    if (flags & FLAG_COMPRESSED)
      compressed = true;

    if (first) {
      out.type = type;
      out.request_id = request_id;
      first = false;
    } else if (type != out.type || request_id != out.request_id) {
      throw std::runtime_error("KCMCP: interleaved MORE frames");
    }

    if (payload_len > 0) {
      size_t base = out.payload.size();
      out.payload.resize(base + payload_len);
      bool eof2;
      if (!read_exact(fd_, &out.payload[base], payload_len, eof2))
        throw std::runtime_error("KCMCP: truncated frame payload");
    }
    if (!(flags & FLAG_MORE))
      break;
  }
  if (compressed)
    throw ProtocolError(ErrorCode::COMPRESSION_UNSUPPORTED,
      "KCMCP: COMPRESSED payloads are not supported by this server",
      /*fatal=*/false);
  return true;
}

void Connection::send(Type type, uint32_t request_id, const std::string &payload)
{
  const size_t chunk = send_max_ ? send_max_ : payload.size() + 1;
  size_t off = 0;
  do {
    size_t n = payload.size() - off;
    if (n > chunk) n = chunk;
    bool more = (off + n) < payload.size();
    unsigned char hdr[HEADER_LEN];
    hdr[0] = static_cast<uint8_t>(type);
    hdr[1] = more ? FLAG_MORE : 0;
    put_u32(hdr + 2, request_id);
    put_u32(hdr + 6, static_cast<uint32_t>(n));
    write_all(fd_, hdr, HEADER_LEN);
    if (n > 0)
      write_all(fd_, payload.data() + off, n);
    off += n;
  } while (off < payload.size());
}

bool parse_request(const std::string &payload, Request &out)
{
  if (payload.size() < 6)
    return false;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(payload.data());
  out.operation     = static_cast<Operation>(p[0]);
  out.input_format  = static_cast<InputFormat>(p[1]);
  out.output_format = static_cast<OutputFormat>(p[2]);
  // p[3] reserved
  uint16_t options_len = (uint16_t(p[4]) << 8) | uint16_t(p[5]);
  if (6u + options_len > payload.size())
    return false;
  out.options = payload.substr(6, options_len);
  out.problem = payload.substr(6 + options_len);
  return true;
}

std::string build_result(OutputFormat fmt, const std::string &meta_json,
                         const std::string &result)
{
  std::string out;
  out.push_back(static_cast<char>(fmt));
  out.push_back(0);  // reserved
  uint16_t meta_len = static_cast<uint16_t>(meta_json.size());
  out.push_back(static_cast<char>((meta_len >> 8) & 0xff));
  out.push_back(static_cast<char>(meta_len & 0xff));
  out += meta_json;
  out += result;
  return out;
}

std::string build_error(ErrorCode code, const std::string &message)
{
  std::string out;
  uint16_t c = static_cast<uint16_t>(code);
  out.push_back(static_cast<char>((c >> 8) & 0xff));
  out.push_back(static_cast<char>(c & 0xff));
  out += message;
  return out;
}

} // namespace kcmcp
