#ifndef AGG_TOKEN_H
#define AGG_TOKEN_H

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

typedef struct agg_token{
  unsigned char tok[2*UUID_LEN+5];
  unsigned char val[80];
} agg_token;

#endif /* AGG_TOKEN_H */
