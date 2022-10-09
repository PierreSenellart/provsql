#ifndef PROVSQL_UTILS_H
#define PROVSQL_UTILS_H

#include "pg_config.h" // for PG_VERSION_NUM
#include "c.h" // for int16

#include "utils/uuid.h"

#if PG_VERSION_NUM < 100000
/* In versions of PostgreSQL < 10, pg_uuid_t is declared to be an opaque
 * struct pg_uuid_t in uuid.h, so we have to give the definition of
 * struct pg_uuid_t; this problem is resolved in PostgreSQL 10 */
#define UUID_LEN 16
struct pg_uuid_t
{
  unsigned char data[UUID_LEN];
};
#endif /* PG_VERSION_NUM */

#include "postgres_ext.h"
#include "nodes/pg_list.h"

typedef enum gate_type {
  gate_input, gate_plus, gate_times, gate_monus, gate_project, gate_zero, gate_one, gate_eq, gate_agg, gate_semimod, gate_cmp, gate_delta, gate_value, gate_mulinput, nb_gate_types
} gate_type;

typedef struct constants_t {
  Oid OID_SCHEMA_PROVSQL;
  Oid OID_TYPE_GATE_TYPE;
  Oid OID_TYPE_AGG_TOKEN;
  Oid OID_TYPE_UUID;
  Oid OID_TYPE_UUID_ARRAY;
  Oid OID_TYPE_INT;
  Oid OID_TYPE_INT_ARRAY;
  Oid OID_TYPE_FLOAT;
  Oid OID_TYPE_VARCHAR;
  Oid OID_FUNCTION_ARRAY_AGG;
  Oid OID_FUNCTION_PROVENANCE_PLUS;
  Oid OID_FUNCTION_PROVENANCE_TIMES;
  Oid OID_FUNCTION_PROVENANCE_MONUS;
  Oid OID_FUNCTION_PROVENANCE_PROJECT;
  Oid OID_FUNCTION_PROVENANCE_EQ;
  Oid OID_FUNCTION_PROVENANCE;
  Oid GATE_TYPE_TO_OID[nb_gate_types];
  Oid OID_FUNCTION_PROVENANCE_DELTA;
  Oid OID_FUNCTION_PROVENANCE_AGGREGATE;
  Oid OID_FUNCTION_PROVENANCE_SEMIMOD;
  Oid OID_FUNCTION_GATE_ZERO;
  Oid OID_OPERATOR_NOT_EQUAL_UUID;
  Oid OID_FUNCTION_NOT_EQUAL_UUID;
  bool ok;
} constants_t;

Oid find_equality_operator(Oid ltypeId, Oid rtypeId);

extern bool provsql_interrupted;
extern bool provsql_where_provenance;
extern int provsql_verbose;

constants_t initialize_constants(bool failure_if_not_possible);

#endif /* PROVSQL_UTILS_H */
