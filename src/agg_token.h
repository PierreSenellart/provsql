#ifndef AGG_TOKEN_H
#define AGG_TOKEN_H

#include "provsql_utils.h"

typedef struct agg_token{
  unsigned char tok[2*UUID_LEN+5];
  unsigned char val[80];
} agg_token;

#endif /* AGG_TOKEN_H */
