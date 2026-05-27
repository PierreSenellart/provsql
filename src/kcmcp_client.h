/**
 * @file kcmcp_client.h
 * @brief In-extension KCMCP client: compile a Boolean problem on a warm,
 *        socket-attached knowledge compiler instead of spawning a CLI tool.
 *
 * Speaks the protocol defined in @c doc/source/dev/kc-server-protocol.rst,
 * over the shared @c kcmcp_protocol codec.  Used by
 * @c BooleanCircuit::compilation() when the selected compile tool is a
 * registry record of @c kind @c "kcmcp".  The reference server is
 * @c tdkc @c --kcmcp (see @c kcmcp_server.h).
 */
#ifndef PROVSQL_KCMCP_CLIENT_H
#define PROVSQL_KCMCP_CLIENT_H

#include <cstdint>
#include <string>

namespace provsql {

/**
 * @brief Compile @p problem on a KCMCP server and return its d-DNNF NNF text.
 *
 * Connects to @p endpoint (@c "unix:/path" or @c "host:port"), performs the
 * HELLO handshake, issues one @c compile REQUEST for @p problem in the given
 * @p input_format (0 = @c dimacs-cnf, 1 = @c circuit-bcs12) wanting
 * @c ddnnf-nnf output, and returns the RESULT's NNF text verbatim (parsed by
 * @c BooleanCircuit::parseDDNNF, exactly as the CLI temp-file path is).  A
 * fresh connection per call.
 *
 * Honours PostgreSQL query-cancel / @c statement_timeout while waiting: a
 * pending cancel closes the socket (so the server abandons the job) and is
 * then raised, mirroring the cancel discipline in @c external_tool.cpp.
 * Throws @c std::runtime_error on any connect / protocol / server-ERROR
 * failure so the caller can fall back to the CLI path.
 */
std::string kcmcp_compile(const std::string &endpoint,
                          uint8_t input_format,
                          const std::string &problem);

}  // namespace provsql

#endif  // PROVSQL_KCMCP_CLIENT_H
