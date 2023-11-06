#ifndef CIRCUIT_FROM_SH_MEM
#define CIRCUIT_FROM_SH_MEM

extern "C" {
#include <postgres.h>
}

#include "BooleanCircuit.h"

BooleanCircuit createBooleanCircuit(pg_uuid_t token);

#endif /* CIRCUIT_FROM_SH_MEM */
