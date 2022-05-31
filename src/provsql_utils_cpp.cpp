#include "utils/uuid.h"
#include "provsql_utils.h"
#include "provsql_utils_cpp.h"
#include "Circuit.h"

using namespace std;

string uuid2string(pg_uuid_t uuid) {
/* copied with small changes from uuid.c */
  static const char hex_chars[] = "0123456789abcdef";
  string result;

  for (int i = 0; i < UUID_LEN; i++)
  {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      result += '-';

    int hi = uuid.data[i] >> 4;
    int lo = uuid.data[i] & 0x0F;

    result+=hex_chars[hi];
    result+=hex_chars[lo];
  }

  return result;
}

pg_uuid_t string2uuid(const string &source)
/* copied with small changes from uuid.c */
{
  const char *src = source.c_str();
  pg_uuid_t uuid;
  bool  braces = false;
  int i;

  if (src[0] == '{')
  {
    src++;
    braces = true;
  }

  for (i = 0; i < UUID_LEN; i++)
  {
    char str_buf[3];

    if (src[0] == '\0' || src[1] == '\0')
      goto syntax_error;
    memcpy(str_buf, src, 2);
    if (!isxdigit((unsigned char) str_buf[0]) ||
        !isxdigit((unsigned char) str_buf[1]))
      goto syntax_error;

    str_buf[2] = '\0';
    uuid.data[i] = (unsigned char) strtoul(str_buf, NULL, 16);
    src += 2;
    if (src[0] == '-' && (i % 2) == 1 && i < UUID_LEN - 1)
      src++;
  }

  if (braces)
  {
    if (*src != '}')
      goto syntax_error;
    src++;
  }

  if (*src != '\0')
    goto syntax_error;

  return uuid;

syntax_error:
  ereport(ERROR,
      (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
       errmsg("invalid input syntax for type %s: \"%s\"",
         "uuid", src)));
}

string UUIDDatum2string(Datum token)
{
  pg_uuid_t *uuid = DatumGetUUIDP(token);
  return uuid2string(*uuid);
}
