/**
 * @file provsql_utils.h
 * @brief Core types, constants, and utilities shared across ProvSQL.
 *
 * This header is included by virtually every source file in the
 * extension.  It provides:
 * - The @c gate_type enumeration listing all circuit-gate kinds
 *   recognised by ProvSQL (input, semiring operations, aggregation, etc.)
 * - The @c constants_t structure caching PostgreSQL OIDs for the types,
 *   functions, and operators that ProvSQL installs, so that OID lookups
 *   happen once per session rather than on every query.
 * - The @c database_constants_t wrapper for per-database OID caches.
 * - Helper declarations for OID lookup and UUID manipulation.
 * - Global flags controlling interrupt handling, where-provenance, and
 *   verbosity.
 * - An implicit inclusion of @c provsql_error.h for the @c provsql_error
 *   / @c provsql_warning / @c provsql_notice / @c provsql_log macros.
 */
#ifndef PROVSQL_UTILS_H
#define PROVSQL_UTILS_H

#include "pg_config.h" // for PG_VERSION_NUM
#include "c.h" // for int16

#include "postgres.h"
#include "utils/uuid.h"

#if PG_VERSION_NUM < 100000
/// Number of bytes in a UUID
#define UUID_LEN 16

/** UUID structure. In versions of PostgreSQL < 10, pg_uuid_t is declared
 * to be an opaque struct pg_uuid_t in uuid.h, so we have to give the
 * definition of struct pg_uuid_t; this problem is resolved in PostgreSQL 10. */
struct pg_uuid_t
{
  unsigned char data[UUID_LEN]; ///< Raw 16-byte UUID storage
};
#endif /* PG_VERSION_NUM */

#include "postgres_ext.h"
#include "nodes/pg_list.h"

/**
 * @brief Possible gate types in the provenance circuit.
 *
 * @warning ON-DISK ABI: this enum's integer values are stored in the
 * @c gates.mmap backing file (see @c MMappedCircuit).  Reordering,
 * inserting, or renumbering existing members will silently invalidate
 * every existing installation's persistent circuit.  New gate types
 * must be appended **at the end**, just before @c gate_invalid.  If an
 * existing gate type ever needs to be removed or renumbered, the mmap
 * format must gain a version header and a migration path.
 */
typedef enum gate_type {
  gate_input,    ///< Input (variable) gate of the circuit
  gate_plus,     ///< Semiring plus
  gate_times,    ///< Semiring times
  gate_monus,    ///< M-Semiring monus
  gate_project,  ///< Project gate (for where provenance)
  gate_zero,     ///< Semiring zero
  gate_one,      ///< Semiring one
  gate_eq,       ///< Equijoin gate (for where provenance)
  gate_agg,      ///< Aggregation operator (for aggregate provenance)
  gate_semimod,  ///< Semimodule scalar multiplication (for aggregate provenance)
  gate_cmp,      ///< Comparison of aggregate values (HAVING-clause provenance)
  gate_delta,    ///< δ-semiring operator (see Amsterdamer, Deutch, Tannen, PODS 2011)
  gate_value,    ///< Scalar value (for aggregate provenance)
  gate_mulinput, ///< Multivalued input (for Boolean provenance)
  gate_update,   ///< Update operation
  gate_rv,       ///< Continuous random-variable leaf (extra encodes distribution)
  gate_arith,    ///< n-ary arithmetic gate over scalar-valued children (info1 holds operator tag)
  gate_invalid,  ///< Invalid gate type
  nb_gate_types  ///< Total number of gate types
} gate_type;

/**
 * @brief Arithmetic operator tags used by @c gate_arith.
 *
 * Stored in the gate's @c info1 field.  Local enum (not a PostgreSQL
 * operator OID) because arithmetic in the sampler / evaluator is just
 * C++ doubles, with no need to dispatch through the PG catalog.
 *
 * @warning ON-DISK ABI: like @c gate_type, these integer values are
 * persisted (in @c info1).  Reordering or renumbering existing tags
 * will silently invalidate every existing installation's persistent
 * circuit.  New tags must be appended at the end.
 */
typedef enum provsql_arith_op {
  PROVSQL_ARITH_PLUS  = 0, ///< n-ary, sum of children
  PROVSQL_ARITH_TIMES = 1, ///< n-ary, product of children
  PROVSQL_ARITH_MINUS = 2, ///< binary, child0 - child1
  PROVSQL_ARITH_DIV   = 3, ///< binary, child0 / child1
  PROVSQL_ARITH_NEG   = 4  ///< unary, -child0
} provsql_arith_op;

/** Names of gate types */
extern const char *gate_type_name[];

/** Structure to store the value of various constants. This is needed to
 * uniquely identify types, functions, etc., in PostgreSQL through their
 * Object Identifier Types (OIDs). */
typedef struct constants_t {
  Oid OID_SCHEMA_PROVSQL; ///< OID of the provsql SCHEMA
  Oid OID_TYPE_GATE_TYPE; ///< OID of the provenance_gate TYPE
  Oid OID_TYPE_AGG_TOKEN; ///< OID of the agg_token TYPE
  Oid OID_TYPE_UUID; ///< OID of the uuid TYPE
  Oid OID_TYPE_UUID_ARRAY; ///< OID of the uuid[] TYPE
  Oid OID_TYPE_BOOL; ///< OID of the BOOL TYPE
  Oid OID_TYPE_INT; ///< OID of the INT TYPE
  Oid OID_TYPE_INT_ARRAY; ///< OID of the INT[] TYPE
  Oid OID_TYPE_FLOAT; ///< OID of the FLOAT TYPE
  Oid OID_TYPE_VARCHAR; ///< OID of the VARCHAR TYPE
  Oid OID_TYPE_TSTZMULTIRANGE; ///< OID of the tstzmultirange TYPE (PG14+, InvalidOid otherwise)
  Oid OID_TYPE_NUMMULTIRANGE; ///< OID of the nummultirange TYPE (PG14+, InvalidOid otherwise)
  Oid OID_TYPE_INT4MULTIRANGE; ///< OID of the int4multirange TYPE (PG14+, InvalidOid otherwise)
  Oid OID_FUNCTION_ARRAY_AGG; ///< OID of the array_agg FUNCTION
  Oid OID_FUNCTION_PROVENANCE_PLUS; ///< OID of the provenance_plus FUNCTION
  Oid OID_FUNCTION_PROVENANCE_TIMES; ///< OID of the provenance_times FUNCTION
  Oid OID_FUNCTION_PROVENANCE_MONUS; ///< OID of the provenance_monus FUNCTION
  Oid OID_FUNCTION_PROVENANCE_PROJECT; ///< OID of the provenance_project FUNCTION
  Oid OID_FUNCTION_PROVENANCE_EQ;///< OID of the provenance_eq FUNCTION
  Oid OID_FUNCTION_PROVENANCE_CMP; ///< OID of the provenance_cmp FUNCTION
  Oid OID_FUNCTION_PROVENANCE; ///< OID of the provenance FUNCTION
  Oid GATE_TYPE_TO_OID[nb_gate_types]; ///< Array of the OID of each provenance_gate ENUM value
  Oid OID_FUNCTION_PROVENANCE_DELTA; ///< OID of the provenance_delta FUNCTION
  Oid OID_FUNCTION_PROVENANCE_AGGREGATE; ///< OID of the provenance_aggregate FUNCTION
  Oid OID_FUNCTION_PROVENANCE_SEMIMOD; ///< OID of the provenance_semimod FUNCTION
  Oid OID_FUNCTION_GATE_ZERO; ///< OID of the provenance_zero FUNCTION
  Oid OID_FUNCTION_GATE_ONE; ///< OID of the provenance_one FUNCTION
  Oid OID_OPERATOR_NOT_EQUAL_UUID; ///< OID of the <> operator on UUIDs FUNCTION
  Oid OID_FUNCTION_NOT_EQUAL_UUID; ///< OID of the = operator on UUIDs FUNCTION
  Oid OID_FUNCTION_AGG_TOKEN_UUID; ///< OID of the agg_token_uuid FUNCTION
  bool ok; ///< true if constants were loaded
} constants_t;




/** Structure to store the value of various constants for a specific
 * database. */
typedef struct database_constants_t {
  Oid database;          ///< OID of the database these constants belong to
  constants_t constants; ///< Cached OID constants for this database
} database_constants_t;

/**
 * @brief Retrieve the cached OID constants for the current database.
 *
 * On first call (or after a cache miss) this function looks up the OIDs
 * of all ProvSQL-specific types, functions, and operators in the system
 * catalogs and stores them in a per-database cache.  Subsequent calls
 * return the cached values without touching the catalogs.
 *
 * @param failure_if_not_possible  If @c true, call @c provsql_error when
 *        the ProvSQL schema cannot be found (e.g. the extension is not
 *        installed in the current database).  If @c false, return a
 *        constants_t with @c ok==false instead of aborting.
 * @return A @c constants_t whose @c ok field is @c true on success.
 */
constants_t get_constants(bool failure_if_not_possible);

/**
 * @brief Find the equality operator OID for two given types.
 *
 * Searches @c pg_operator for the @c = operator that accepts
 * @p ltypeId on the left and @p rtypeId on the right.
 *
 * @param ltypeId  OID of the left operand type.
 * @param rtypeId  OID of the right operand type.
 * @return The operator OID, or @c InvalidOid if none is found.
 */
Oid find_equality_operator(Oid ltypeId, Oid rtypeId);

/** Global variable that becomes true if this particular backend received
 * an interrupt signal. */
extern bool provsql_interrupted;

/** Global variable that indicates if where-provenance support has been
 * activated through the provsql.where_provenance run-time configuration
 * parameter. */
extern bool provsql_where_provenance;

/** Global variable that indicates the verbosity level set by the
 * provsql.verbose_level run-time configuration parameter was set */
extern int provsql_verbose;

/** Global flag controlling agg_token text output: when true,
 * agg_token_out emits the underlying provenance UUID instead of the
 * default "value (*)" display string. Driven by the
 * provsql.aggtoken_text_as_uuid GUC. */
extern bool provsql_aggtoken_text_as_uuid;

/** Colon-separated list of directories prepended to PATH when ProvSQL
 * spawns external tools (d4, c2d, minic2d, dsharp, weightmc, graph-easy),
 * set by the provsql.tool_search_path run-time configuration parameter.
 * NULL or empty means rely on the server's PATH alone. */
extern char *provsql_tool_search_path;

#include "provsql_error.h"

#endif /* PROVSQL_UTILS_H */
