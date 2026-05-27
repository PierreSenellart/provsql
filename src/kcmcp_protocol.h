/**
 * @file kcmcp_protocol.h
 * @brief Wire codec for KCMCP, the Knowledge Compiler / Model Counter
 *        Protocol (see doc/source/dev/kc-server-protocol.rst).
 *
 * A small, dependency-free C++ layer over a connected stream socket: the
 * 10-byte frame header, MORE-chunked message reassembly, and pack/unpack of
 * the REQUEST / RESULT / ERROR payloads and the operation / format / error
 * registries.  It is used by the @c tdkc reference server and is written to
 * be reusable by a future ProvSQL client.  No PostgreSQL dependency.
 */
#ifndef PROVSQL_KCMCP_PROTOCOL_H
#define PROVSQL_KCMCP_PROTOCOL_H

#include <cstdint>
#include <stdexcept>
#include <string>

namespace kcmcp {

/// Frame type (header byte 0).
enum class Type : uint8_t {
  HELLO    = 0x00,
  REQUEST  = 0x01,
  RESULT   = 0x02,
  ERROR    = 0x03,
  PROGRESS = 0x04,
  CANCEL   = 0x05,
  PING     = 0x06,
  PONG     = 0x07,
  BYE      = 0x08,
};

/// Frame flags (header byte 1).
enum Flag : uint8_t {
  FLAG_MORE       = 0x01,  ///< payload continues in the next frame
  FLAG_COMPRESSED = 0x02,  ///< payload is zstd-compressed (unused here)
};

/// Operation registry (REQUEST byte 0 / HELLO @c operations names).
enum class Operation : uint8_t { COUNT = 0, WMC = 1, COMPILE = 2 };

/// Input-format registry (REQUEST byte 1).
enum class InputFormat : uint8_t { DIMACS_CNF = 0, CIRCUIT_BCS12 = 1 };

/// Output-format registry (REQUEST byte 2 / RESULT byte 0; one shared space).
enum class OutputFormat : uint8_t {
  DECIMAL = 0, RATIONAL = 1, DOUBLE = 2, BIGINT = 3, DDNNF_NNF = 4 };

/// ERROR codes.
enum class ErrorCode : uint16_t {
  UNSUPPORTED_OPERATION   = 1,  ///< unsupported operation or unknown frame type
  UNSUPPORTED_FORMAT      = 2,
  PARSE                   = 3,
  TIMEOUT                 = 4,
  CANCELLED               = 5,
  INTERNAL                = 6,
  PAYLOAD_TOO_LARGE       = 7,
  UNSUPPORTED_VERSION     = 8,  ///< client requires a KCMCP major this server lacks
  COMPRESSION_UNSUPPORTED = 9,  ///< COMPRESSED frame flag set, but unsupported
};

const char *operation_name(Operation op);
const char *input_format_name(InputFormat fmt);
const char *output_format_name(OutputFormat fmt);

/// Thrown by Connection on a protocol violation that warrants an ERROR frame
/// (e.g. an oversize incoming frame); @ref code is the KCMCP error code.
/// @ref fatal is false when the stream stayed synchronised (the offending
/// message was fully consumed), so the caller may answer with an ERROR and
/// keep serving the connection; true when the stream is desynchronised and
/// the connection must be closed.
struct ProtocolError : std::runtime_error {
  ErrorCode code;
  bool fatal;
  ProtocolError(ErrorCode c, const std::string &what, bool fatal_ = true)
    : std::runtime_error(what), code(c), fatal(fatal_) {}
};

/// A fully reassembled inbound message (MORE frames concatenated).
struct Message {
  Type type{};
  uint32_t request_id{};
  std::string payload;
};

/// Decoded REQUEST payload.
struct Request {
  Operation operation{};
  InputFormat input_format{};
  OutputFormat output_format{};
  std::string options;  ///< UTF-8 JSON (may be empty == {})
  std::string problem;  ///< the formula bytes
};

/**
 * @brief Framed message transport over one connected socket fd.
 *
 * @c recv_max is the largest single-frame payload this side accepts (its
 * advertised @c max_payload); @c send_max is the peer's accept limit, used to
 * split outbound payloads into MORE-flagged frames.  Blocking I/O.
 */
class Connection {
public:
  Connection(int fd, uint32_t recv_max, uint32_t send_max)
    : fd_(fd), recv_max_(recv_max), send_max_(send_max) {}

  /// Read one logical message (concatenating MORE frames).  Returns false on
  /// a clean peer close at a frame boundary.  Throws ProtocolError on an
  /// oversize frame, std::runtime_error on an I/O error or truncation.
  bool recv(Message &out);

  /// Send a message, splitting @p payload across MORE-flagged frames no larger
  /// than the peer's limit.
  void send(Type type, uint32_t request_id, const std::string &payload);
  void send(Type type, uint32_t request_id) { send(type, request_id, std::string()); }

  int fd() const { return fd_; }

private:
  int fd_;
  uint32_t recv_max_;
  uint32_t send_max_;
};

/// Decode a REQUEST payload; returns false if structurally malformed.
bool parse_request(const std::string &payload, Request &out);

/// Build a RESULT payload (result_format byte + meta JSON + result bytes).
std::string build_result(OutputFormat fmt, const std::string &meta_json,
                         const std::string &result);

/// Build an ERROR payload (u16 code + UTF-8 message).
std::string build_error(ErrorCode code, const std::string &message);

} // namespace kcmcp

#endif
