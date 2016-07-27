#ifndef PROVSQL_UTILS_H
#define PROVSQL_UTILS_H

#include "postgres_ext.h"

typedef struct constants_t {
  Oid OID_SCHEMA_PROVSQL;
  Oid OID_TYPE_PROVENANCE_TOKEN;
  Oid OID_TYPE_UUID;
  Oid OID_TYPE_UUID_ARRAY;
  Oid OID_FUNCTION_PROVENANCE_AND;
  Oid OID_FUNCTION_PROVENANCE_AGG;
  Oid OID_FUNCTION_PROVENANCE;
} constants_t;

bool initialize_constants(constants_t *constants);
#endif /* PROVSQL_UTILS_H */
