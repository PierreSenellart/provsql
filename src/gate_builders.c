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
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
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

static int token_cmp(const void *a, const void *b) {
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

/** @brief The canonical address, @c "<prefix>{sorted children}". */
static pg_uuid_t canonical_address(const char *canonical_prefix,
                                   const pg_uuid_t *children, int n) {
  StringInfoData buf;
  pg_uuid_t *sorted;

  sorted = (pg_uuid_t *)palloc(sizeof(pg_uuid_t) * (n > 0 ? n : 1));
  memcpy(sorted, children, sizeof(pg_uuid_t) * n);
  qsort(sorted, n, sizeof(pg_uuid_t), token_cmp);
  name_begin(&buf, canonical_prefix);
  if (n > 0)
    name_add_uuid_array(&buf, sorted, n);
  pfree(sorted);
  return name_end(&buf);
}

/* -------------------------------------------------------------------------
 * Planted gates
 *
 * The reachability route compiles, at plan time, a certified circuit
 * equivalent to a sum or product of correlated tokens of the working table it
 * has just filled, and wants the query to use that circuit where the generic
 * rewriting builds the sum or the product.  It pre-creates ("plants") a gate
 * at the canonical address of the multiset of tokens, and provenance_plus /
 * provenance_times return that gate when they are given that very multiset.
 *
 * Which addresses are planted is known to the backend that planted them, and
 * to no one else: the functions below look them up in a table local to the
 * backend, never in the store.  This is enough, because the sum or product
 * in question is computed above a scan of the working table, a temporary
 * table: PostgreSQL runs it in the backend that owns the table, not in a
 * parallel worker, and that backend is the one which lowered the CTE.  When
 * nothing is planted, which is the case of every session that ran no
 * reachability query, the canonical address is not even computed.
 *
 * A planted gate is needed for as long as its working table exists: a cached
 * plan keeps reading the table, and is invalidated when the table is dropped,
 * so that the next execution lowers the CTE and plants again.  The entries
 * are therefore grouped by working table, and discarded with it.  Gates of
 * the store outlive an aborted transaction, so an entry never names a gate
 * that does not exist.
 * ------------------------------------------------------------------------- */

/** @brief A working table with planted gates. */
typedef struct PlantedScope {
  int id;
  Oid relid;                    /**< The table, as created by the lowering */
  char name[NAMEDATALEN];
  struct PlantedScope *next;
} PlantedScope;

typedef struct PlantedEntry {
  pg_uuid_t address;            /**< Hash key */
  int scope;
} PlantedEntry;

static HTAB *planted = NULL;
static PlantedScope *planted_scopes = NULL;
static PlantedScope *planted_current = NULL;
static int planted_next_id = 0;

/** @brief OID of the temporary table @p name of this backend, if any. */
static Oid temp_table_oid(const char *name) {
  Oid nsp = LookupExplicitNamespace("pg_temp", true);
  return OidIsValid(nsp) ? get_relname_relid(name, nsp) : InvalidOid;
}

static void planted_forget(PlantedScope *scope) {
  HASH_SEQ_STATUS seq;
  PlantedEntry *e;
  PlantedScope **link;

  if (planted != NULL) {
    hash_seq_init(&seq, planted);
    while ((e = (PlantedEntry *)hash_seq_search(&seq)) != NULL)
      if (e->scope == scope->id)
        hash_search(planted, &e->address, HASH_REMOVE, NULL);
  }
  for (link = &planted_scopes; *link != NULL; link = &(*link)->next)
    if (*link == scope) {
      *link = scope->next;
      break;
    }
  if (planted_current == scope)
    planted_current = NULL;
  pfree(scope);
}

/**
 * @brief Open the scope of the working table @p name, just (re)created:
 *        forget what was planted for the table it replaces, and for every
 *        working table that no longer exists.
 */
static PlantedScope *planted_open_scope(const char *name, bool reset) {
  PlantedScope *s, *next, *found = NULL;

  for (s = planted_scopes; s != NULL; s = next) {
    next = s->next;
    if (strcmp(s->name, name) == 0) {
      if (reset)
        planted_forget(s);
      else
        found = s;
    } else if (temp_table_oid(s->name) != s->relid)
      planted_forget(s);
  }
  if (found == NULL) {
    found = (PlantedScope *)MemoryContextAllocZero(TopMemoryContext,
                                                   sizeof(PlantedScope));
    found->id = planted_next_id++;
    strlcpy(found->name, name, NAMEDATALEN);
    found->next = planted_scopes;
    planted_scopes = found;
  }
  found->relid = temp_table_oid(name);
  return found;
}

static bool is_planted(const pg_uuid_t *address) {
  return hash_search(planted, address, HASH_FIND, NULL) != NULL;
}

pg_uuid_t provsql_plant_canonical(const char *work_name, gate_type type,
                                  const pg_uuid_t *children, int n,
                                  const pg_uuid_t *target,
                                  unsigned info1, unsigned info2) {
  PlantedScope *scope;
  PlantedEntry *e;
  pg_uuid_t address;
  bool found;

  if (type != gate_plus && type != gate_times)
    provsql_error("only plus and times gates can be planted");
  scope = work_name != NULL ? planted_open_scope(work_name, false)
                            : planted_current;
  if (scope == NULL)
    provsql_error("planting a gate outside the lowering of a recursive CTE");

  address = canonical_address(type == gate_plus ? "plus-canonical"
                                                : "times-canonical",
                              children, n);
  provsql_internal_create_gate(&address, type, 1, target);
  provsql_internal_set_infos(&address, info1, info2);

  if (planted == NULL) {
    HASHCTL ctl;
    memset(&ctl, 0, sizeof(ctl));
    ctl.keysize = sizeof(pg_uuid_t);
    ctl.entrysize = sizeof(PlantedEntry);
    ctl.hcxt = TopMemoryContext;
    planted = hash_create("ProvSQL planted gates", 256, &ctl,
                          HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
  }
  e = (PlantedEntry *)hash_search(planted, &address, HASH_ENTER, &found);
  e->scope = scope->id;
  return address;
}

PG_FUNCTION_INFO_V1(planted_scope);
/** @brief SQL entry point of @c planted_open_scope(), called by the drivers
 *  of recursive CTEs right after they (re)create the working table. */
Datum planted_scope(PG_FUNCTION_ARGS) {
  char *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
  planted_current = planted_open_scope(name, true);
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(plant_canonical);
/** @brief SQL entry point of @c provsql_plant_canonical(). */
Datum plant_canonical(PG_FUNCTION_ARGS) {
  char *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
  char *type_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
  pg_uuid_t *children, address;
  pg_uuid_t nothing;
  int n;

  memset(&nothing, 0xFF, sizeof(nothing));
  n = filtered_tokens(PG_GETARG_ARRAYTYPE_P(2), &nothing, &children);
  address = provsql_plant_canonical(
    name, strcmp(type_name, "plus") == 0 ? gate_plus :
          strcmp(type_name, "times") == 0 ? gate_times : gate_invalid,
    children, n, PG_GETARG_UUID_P(3),
    (unsigned)PG_GETARG_INT32(4), (unsigned)PG_GETARG_INT32(5));
  return uuid_result(&address);
}

/**
 * @brief A gate of type @p type over @p children, unless this backend planted
 *        one at the canonical address of their multiset.
 */
static pg_uuid_t nary_gate(gate_type type, const char *plain_prefix,
                           const char *canonical_prefix,
                           const pg_uuid_t *children, int n) {
  StringInfoData buf;
  pg_uuid_t token;

  if (planted != NULL && hash_get_num_entries(planted) > 0) {
    pg_uuid_t canonical = canonical_address(canonical_prefix, children, n);
    if (is_planted(&canonical))
      return canonical;
  }

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
