#ifndef BOOLEAN_CIRCUIT_FROM_MMAP_H
#define BOOLEAN_CIRCUIT_FROM_MMAP_H

extern "C" {
#include "provsql_utils.h"
}

#include "BooleanCircuit.h"
#include "GenericCircuit.h"

BooleanCircuit getBooleanCircuit(pg_uuid_t token);
GenericCircuit getGenericCircuit(pg_uuid_t token);

#endif /* BOOLEAN_CIRCUIT_FROM_MMAP_H */
