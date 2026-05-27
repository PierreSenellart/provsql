/**
 * @file kcmcp_server.h
 * @brief The @c tdkc KCMCP reference server.
 *
 * Runs @c tdkc as a KCMCP-compliant engine (see
 * doc/source/dev/kc-server-protocol.rst): it accepts connections on a Unix or
 * TCP endpoint, advertises @c compile (to @c ddnnf-nnf) and @c wmc (to
 * @c decimal) over @c dimacs-cnf input, and serves each request by building a
 * d-DNNF through ProvSQL's in-process tree decomposition.  It is a reference /
 * conformance implementation of the protocol; ProvSQL itself keeps tree
 * decomposition in-process rather than talking to it over a socket.
 */
#ifndef PROVSQL_KCMCP_SERVER_H
#define PROVSQL_KCMCP_SERVER_H

#include <string>

/**
 * @brief Serve KCMCP on @p endpoint until terminated.
 * @param endpoint  @c "unix:/path/to.sock" or @c "host:port" (e.g.
 *                  @c "127.0.0.1:9000").
 * @return process exit code (non-zero on a setup failure).
 */
int kcmcp_serve(const std::string &endpoint);

#endif
