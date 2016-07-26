#ifndef PROVSQL_UTILS_H
#define PROVSQL_UTILS_H

#include "postgres_ext.h"

typedef struct constants_t {
  Oid PROVENANCE_TOKEN_OID;
  Oid UUID_OID;
  Oid UUID_ARRAY_OID;
  Oid PROVENANCE_AND_OID;
  Oid PROVENANCE_AGG_OID;
  Oid PROVENANCE_OID;
} constants_t;

void initialize_constants(constants_t *constants);
#endif /* PROVSQL_UTILS_H */
