#ifndef PROVSQL_UTILS_CPP_H
#define PROVSQL_UTILS_CPP_H

extern "C" {
#include "postgres.h"
#include "provsql_utils.h"
}

#include <string>

std::string UUIDDatum2string(Datum token);
std::string uuid2string(pg_uuid_t uuid);
pg_uuid_t string2uuid(const std::string &source);
std::size_t hash_value(const pg_uuid_t &u);
bool operator==(const pg_uuid_t &u, const pg_uuid_t &v);

#endif
