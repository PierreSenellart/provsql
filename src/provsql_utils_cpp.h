/**
 * @file provsql_utils_cpp.h
 * @brief C++ utility functions for UUID manipulation.
 *
 * This header declares C++ helper functions that convert between
 * PostgreSQL's @c pg_uuid_t / @c Datum representations and
 * @c std::string, compute hash values for UUIDs (enabling use in
 * @c std::unordered_map and Boost containers), and define equality
 * comparison for @c pg_uuid_t values.
 *
 * These utilities are used throughout the C++ circuit and evaluation
 * code wherever UUID tokens need to be stored in STL containers or
 * compared by value.
 */
#ifndef PROVSQL_UTILS_CPP_H
#define PROVSQL_UTILS_CPP_H

extern "C" {
#include "postgres.h"
#include "provsql_utils.h"
}

#include <string>

/**
 * @brief Convert a PostgreSQL @c Datum holding a UUID to a @c std::string.
 *
 * Detoasts and formats the UUID value as the standard 36-character
 * hyphenated hex string (@c xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx).
 *
 * @param token  A @c Datum containing a @c pg_uuid_t value.
 * @return       The UUID as a @c std::string.
 */
std::string UUIDDatum2string(Datum token);

/**
 * @brief Format a @c pg_uuid_t as a @c std::string.
 *
 * @param uuid  The UUID to format.
 * @return      The UUID as a 36-character hyphenated hex @c std::string.
 */
std::string uuid2string(pg_uuid_t uuid);

/**
 * @brief Parse a UUID string into a @c pg_uuid_t.
 *
 * Accepts the standard 36-character hyphenated hex representation.
 * Throws @c std::invalid_argument if the string is malformed.
 *
 * @param source  The UUID string to parse.
 * @return        The parsed @c pg_uuid_t.
 */
pg_uuid_t string2uuid(const std::string &source);

/**
 * @brief Compute a hash value for a @c pg_uuid_t.
 *
 * Reinterprets the 16-byte UUID as two 64-bit integers and XOR-combines
 * their standard hashes.  Used by Boost's hash infrastructure so that
 * @c pg_uuid_t can be used as a key in Boost multi-index containers.
 *
 * @param u  The UUID to hash.
 * @return   A @c std::size_t hash value.
 */
std::size_t hash_value(const pg_uuid_t &u);

/**
 * @brief Test two @c pg_uuid_t values for equality.
 *
 * Compares all 16 bytes of the UUID data.
 *
 * @param u  First UUID.
 * @param v  Second UUID.
 * @return   @c true if @p u and @p v represent the same UUID.
 */
bool operator==(const pg_uuid_t &u, const pg_uuid_t &v);

#endif
