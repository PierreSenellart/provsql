#ifndef PROVSQL_UTILS_H
#define PROVSQL_UTILS_H

#include "postgres_ext.h"
#include "nodes/pg_list.h"

typedef struct constants_t {
  Oid OID_SCHEMA_PROVSQL;
  Oid OID_TYPE_PROVENANCE_TOKEN;
  Oid OID_TYPE_UUID;
  Oid OID_TYPE_UUID_ARRAY;
  Oid OID_TYPE_INT;
  Oid OID_TYPE_INT_ARRAY;
  Oid OID_FUNCTION_PROVENANCE_AGG_PLUS;
  Oid OID_FUNCTION_PROVENANCE_TIMES;
  Oid OID_FUNCTION_PROVENANCE_MONUS;
  Oid OID_FUNCTION_PROVENANCE_PROJECT;
  Oid OID_FUNCTION_PROVENANCE_EQ;
  Oid OID_FUNCTION_PROVENANCE;
} constants_t;

bool initialize_constants(constants_t *constants);
Oid find_equality_operator(Oid ltypeId, Oid rtypeId);

extern bool provsql_shared_library_loaded;
extern bool provsql_interrupted;
extern bool provsql_where_provenance;

#endif /* PROVSQL_UTILS_H */
