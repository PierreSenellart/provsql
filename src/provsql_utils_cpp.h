#ifndef PROVSQL_UTILS_CPP_H
#define PROVSQL_UTILS_CPP_H

extern "C" {
#include "postgres.h"
}

#include <string>

std::string UUIDDatum2string(Datum token);

#endif
