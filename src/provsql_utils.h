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
  unsigned char data[UUID_LEN];
};
#endif /* PG_VERSION_NUM */

#include "postgres_ext.h"
#include "nodes/pg_list.h"

/** Possible gate type in the provenance circuit. */
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
  gate_cmp,      ///< Currently unused, meant for comparison of aggregate values
  gate_delta,    ///< δ-semiring operator (see Amsterdamer, Deutch, Tannen, PODS 2011)
  gate_value,    ///< Scalar value (for aggregate provenance)
  gate_mulinput, ///< Multivalued input (for Boolean provenance)
  gate_update,   ///< Update operation
  gate_invalid,  ///< Invalid gate type
  nb_gate_types  ///< Total number of gate types
} gate_type;

/** Structure to store the value of various constants. This is needed to
 * uniquely identify types, functions, etc., in PostgreSQL through their
 * Object Identifier Types (OIDs). */
typedef struct constants_t {
  Oid OID_SCHEMA_PROVSQL; ///< OID of the provsql SCHEMA
  Oid OID_TYPE_GATE_TYPE; ///< OID of the provenance_gate TYPE
  Oid OID_TYPE_AGG_TOKEN; ///< OID of the agg_token TYPE
  Oid OID_TYPE_UUID; ///< OID of the uuid TYPE
  Oid OID_TYPE_UUID_ARRAY; ///< OID of the uuid[] TYPE
  Oid OID_TYPE_INT; ///< OID of the INT TYPE
  Oid OID_TYPE_INT_ARRAY; ///< OID of the INT[] TYPE
  Oid OID_TYPE_FLOAT; ///< OID of the FLOAT TYPE
  Oid OID_TYPE_VARCHAR; ///< OID of the VARCHAR TYPE
  Oid OID_FUNCTION_ARRAY_AGG; ///< OID of the array_agg FUNCTION
  Oid OID_FUNCTION_PROVENANCE_PLUS; ///< OID of the provenance_plus FUNCTION
  Oid OID_FUNCTION_PROVENANCE_TIMES; ///< OID of the provenance_times FUNCTION
  Oid OID_FUNCTION_PROVENANCE_MONUS; ///< OID of the provenance_monus FUNCTION
  Oid OID_FUNCTION_PROVENANCE_PROJECT; ///< OID of the provenance_project FUNCTION
  Oid OID_FUNCTION_PROVENANCE_EQ; ///< OID of the provenance_eq FUNCTION
  Oid OID_FUNCTION_PROVENANCE; ///< OID of the provenance FUNCTION
  Oid GATE_TYPE_TO_OID[nb_gate_types]; ///< Array of the OID of each provenance_gate ENUM value
  Oid OID_FUNCTION_PROVENANCE_DELTA; ///< OID of the provenance_delta FUNCTION
  Oid OID_FUNCTION_PROVENANCE_AGGREGATE; ///< OID of the provenance_aggregate FUNCTION
  Oid OID_FUNCTION_PROVENANCE_SEMIMOD; ///< OID of the provenance_semimod FUNCTION
  Oid OID_FUNCTION_GATE_ZERO; ///< OID of the provenance_zero FUNCTION
  Oid OID_OPERATOR_NOT_EQUAL_UUID; ///< OID of the <> operator on UUIDs FUNCTION
  Oid OID_FUNCTION_NOT_EQUAL_UUID; ///< OID of the = operator on UUIDs FUNCTION
  bool ok; ///< true if constants were loaded
} constants_t;

/** Structure to store the value of various constants for a specific
 * database. */
typedef struct database_constants_t {
  Oid database; //< OID of the database
  constants_t constants; //< Constants for this database
} database_constants_t;

constants_t get_constants(bool failure_if_not_possible);

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

#endif /* PROVSQL_UTILS_H */
