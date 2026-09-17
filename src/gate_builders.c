/**
 * @file gate_builders.c
 * @brief The gate-building SQL functions the query rewriter calls once per
 *        row or per group, in C.
 *
 * These functions used to be PL/pgSQL.  Each inner statement of a PL/pgSQL
 * function runs through SPI, an executor start-up and shutdown of about 2 µs,
 * and their keys went through the text form of @c uuid[] and back; at one
 * call per output row this was most of the cost of provenance tracking.  The
 * C versions build the same gates at the same addresses: a gate is addressed
 * by the version-5 UUID, in the ProvSQL namespace, of a text name
 * (@c "times{u1,u2}", ...), and the names built here are, byte for byte, the
 * ones the PL/pgSQL code built.  The regression test @c gate_builders keeps
 * the former bodies as reference functions and compares the two.
 */
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/uuid.h"

#include "provsql_mmap.h"
#include "provsql_utils.h"

/** @brief @c uuid_ns_provsql(), the namespace of every gate address. */
static const unsigned char provsql_ns[UUID_LEN] = {
  0x92, 0x0d, 0x4f, 0x02, 0x87, 0x18, 0x53, 0x19,
  0x95, 0x32, 0xd4, 0xab, 0x83, 0xa6, 0x44, 0x89
};

/**
 * @brief Start the name of a gate address.
 *
 * The buffer opens with the namespace, which the version-5 UUID hashes in
 * front of the name, so that the hash runs over the buffer as it is.
 */
static void name_begin(StringInfo buf, const char *prefix) {
  initStringInfo(buf);
  appendBinaryStringInfo(buf, (const char *)provsql_ns, UUID_LEN);
  appendStringInfoString(buf, prefix);
}

/** @brief Append a UUID in the text form of @c uuid_out. */
static void name_add_uuid(StringInfo buf, const pg_uuid_t *u) {
  static const char hex[] = "0123456789abcdef";
  char out[36];
  int i, p = 0;

  for (i = 0; i < UUID_LEN; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out[p++] = '-';
    out[p++] = hex[u->data[i] >> 4];
    out[p++] = hex[u->data[i] & 0xF];
  }
  appendBinaryStringInfo(buf, out, 36);
}

/** @brief Append the text form of a @c uuid[], @c "{u1,u2,...}". */
static void name_add_uuid_array(StringInfo buf, const pg_uuid_t *u, int n) {
  int i;

  appendStringInfoChar(buf, '{');
  for (i = 0; i < n; ++i) {
    if (i > 0)
      appendStringInfoChar(buf, ',');
    name_add_uuid(buf, &u[i]);
  }
  appendStringInfoChar(buf, '}');
}

/** @brief The version-5 UUID of the name accumulated in @p buf. */
static pg_uuid_t name_end(StringInfo buf) {
  unsigned char digest[20];
  pg_uuid_t u;

  provsql_sha1((const unsigned char *)buf->data, buf->len, digest);
  memcpy(u.data, digest, UUID_LEN);
  u.data[6] = (unsigned char)((u.data[6] & 0x0F) | 0x50);
  u.data[8] = (unsigned char)((u.data[8] & 0x3F) | 0x80);
  pfree(buf->data);
  return u;
}

/** @brief Address of a gate named by a constant (@c "zero", @c "one"). */
static pg_uuid_t constant_address(const char *name) {
  StringInfoData buf;

  name_begin(&buf, name);
  return name_end(&buf);
}

static const pg_uuid_t *address_of_zero(void) {
  static pg_uuid_t u;
  static bool known = false;
  if (!known) { u = constant_address("zero"); known = true; }
  return &u;
}

static const pg_uuid_t *address_of_one(void) {
  static pg_uuid_t u;
  static bool known = false;
  if (!known) { u = constant_address("one"); known = true; }
  return &u;
}

static int uuid_cmp(const void *a, const void *b) {
  return memcmp(a, b, UUID_LEN);
}

static Datum uuid_result(const pg_uuid_t *u) {
  pg_uuid_t *r = (pg_uuid_t *)palloc(UUID_LEN);
  memcpy(r, u, UUID_LEN);
  return UUIDPGetDatum(r);
}

/**
 * @brief The elements of a @c uuid[] that are neither NULL nor @p neutral,
 *        in order.  @return their number; @p *out is palloc'd.
 */
static int filtered_tokens(ArrayType *arr, const pg_uuid_t *neutral,
                           pg_uuid_t **out) {
  Datum *elems;
  bool *nulls;
  int n, i, kept = 0;

  deconstruct_array(arr, UUIDOID, UUID_LEN, false, 'c', &elems, &nulls, &n);
  *out = (pg_uuid_t *)palloc(sizeof(pg_uuid_t) * (n > 0 ? n : 1));
  for (i = 0; i < n; ++i) {
    pg_uuid_t *u;
    if (nulls[i])
      continue;
    u = DatumGetUUIDP(elems[i]);
    if (memcmp(u->data, neutral->data, UUID_LEN) == 0)
      continue;
    (*out)[kept++] = *u;
  }
  return kept;
}

/**
 * @brief A gate of type @p type over @p children, unless one was pre-created
 *        at the canonical address of their multiset.
 *
 * The canonical address, @c "<op>-canonical{sorted children}", is where the
 * reachability compiler plants a certified circuit equivalent to the product
 * or sum of correlated tokens; nothing else creates gates there, so a gate of
 * the right type at that address is a deliberate pre-creation and is returned
 * in place of an ordinary gate.
 */
static pg_uuid_t nary_gate(gate_type type, const char *plain_prefix,
                           const char *canonical_prefix,
                           const pg_uuid_t *children, int n) {
  StringInfoData buf;
  pg_uuid_t canonical, token, *sorted, *fetched = NULL;
  unsigned nb_fetched = 0;

  sorted = (pg_uuid_t *)palloc(sizeof(pg_uuid_t) * (n > 0 ? n : 1));
  memcpy(sorted, children, sizeof(pg_uuid_t) * n);
  qsort(sorted, n, sizeof(pg_uuid_t), uuid_cmp);
  name_begin(&buf, canonical_prefix);
  if (n > 0)
    name_add_uuid_array(&buf, sorted, n);
  canonical = name_end(&buf);
  pfree(sorted);

  if (provsql_fetch_gate(&canonical, &nb_fetched, &fetched) == type) {
    if (fetched)
      free(fetched);
    return canonical;
  }
  if (fetched)
    free(fetched);

  name_begin(&buf, plain_prefix);
  if (n > 0)
    name_add_uuid_array(&buf, children, n);
  token = name_end(&buf);
  provsql_internal_create_gate(&token, type, (unsigned)n, n > 0 ? children : NULL);
  return token;
}

PG_FUNCTION_INFO_V1(provenance_times);
/**
 * @brief ⊗ of a list of tokens.
 *
 * A NULL token is the token slot of an untracked source, which is certain: it
 * reads as the ⊗-neutral 𝟙 and is dropped, like @c gate_one itself.  No
 * survivor gives 𝟙, a single one is returned as it is.
 */
Datum provenance_times(PG_FUNCTION_ARGS) {
  pg_uuid_t *tokens, result;
  int n;

  if (PG_ARGISNULL(0))
    return uuid_result(address_of_one());
  n = filtered_tokens(PG_GETARG_ARRAYTYPE_P(0), address_of_one(), &tokens);
  if (n == 0)
    return uuid_result(address_of_one());
  if (n == 1)
    return uuid_result(&tokens[0]);
  result = nary_gate(gate_times, "times", "times-canonical", tokens, n);
  return uuid_result(&result);
}

PG_FUNCTION_INFO_V1(provenance_plus);
/**
 * @brief ⊕ of an array of tokens.  Strict.
 *
 * A NULL token stands for a row absent from the disjunction: it reads as the
 * ⊕-neutral 𝟘 and is dropped, like @c gate_zero itself.  No survivor gives 𝟘,
 * a single one is returned as it is.  (The PL/pgSQL version returned, for no
 * survivor, a @c plus gate without children: @c array_length of the empty
 * selection is NULL, which sent that case to the general branch.  Evaluators
 * read such a gate as 𝟘, and stored circuits may contain it.)
 */
Datum provenance_plus(PG_FUNCTION_ARGS) {
  pg_uuid_t *tokens, result;
  int n;

  n = filtered_tokens(PG_GETARG_ARRAYTYPE_P(0), address_of_zero(), &tokens);
  if (n == 0)
    return uuid_result(address_of_zero());
  if (n == 1)
    return uuid_result(&tokens[0]);
  result = nary_gate(gate_plus, "plus", "plus-canonical", tokens, n);
  return uuid_result(&result);
}
