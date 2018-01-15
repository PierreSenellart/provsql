#include "utils/uuid.h"
#include "provsql_utils_cpp.h"
#include "Circuit.h"

using namespace std;

/* copied with small changes from uuid.c */
string UUIDDatum2string(Datum token)
{
  pg_uuid_t *uuid = DatumGetUUIDP(token);
  static const char hex_chars[] = "0123456789abcdef";
  string result;

  for (int i = 0; i < UUID_LEN; i++)
  {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      result += '-';

    int hi = uuid->data[i] >> 4;
    int lo = uuid->data[i] & 0x0F;

    result+=hex_chars[hi];
    result+=hex_chars[lo];
  }

  return result;
}
