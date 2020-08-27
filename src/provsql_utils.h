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

typedef struct constants_t {
  Oid OID_SCHEMA_PROVSQL;
  Oid OID_TYPE_PROVENANCE_TOKEN;
  Oid OID_TYPE_UUID;
  Oid OID_TYPE_UUID_ARRAY;
  Oid OID_TYPE_INT;
  Oid OID_TYPE_INT_ARRAY;
  Oid OID_FUNCTION_ARRAY_AGG;
  Oid OID_FUNCTION_PROVENANCE_PLUS;
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
