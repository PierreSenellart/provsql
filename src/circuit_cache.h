#ifndef CIRCUIT_CACHE_H
#define CIRCUIT_CACHE_H

#include "provsql_utils.h"

bool circuit_cache_create_gate(pg_uuid_t token, gate_type type, unsigned nb_children, pg_uuid_t *children);
unsigned circuit_cache_get_children(pg_uuid_t token, pg_uuid_t **children);
gate_type circuit_cache_get_type(pg_uuid_t token);

#endif /* CIRCUIT_CACHE_H */
