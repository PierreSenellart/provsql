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
#include "parser/parse_coerce.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/uuid.h"

#include "agg_token.h"
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

/** @brief The UUID @p text (one of the constants of @c provsql_utils.h). */
static pg_uuid_t constant_uuid(const char *text) {
  return *DatumGetUUIDP(DirectFunctionCall1(uuid_in, CStringGetDatum(text)));
}

static const pg_uuid_t *address_of_zero(void) {
  static pg_uuid_t u;
  static bool known = false;
  if (!known) { u = constant_uuid(PROVSQL_GATE_ZERO_UUID); known = true; }
  return &u;
}

static const pg_uuid_t *address_of_one(void) {
  static pg_uuid_t u;
  static bool known = false;
  if (!known) { u = constant_uuid(PROVSQL_GATE_ONE_UUID); known = true; }
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
  provsql_internal_create_gate_with(&address, type, 1, target,
                                    true, info1, info2, NULL);

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

/* -------------------------------------------------------------------------
 * Values
 *
 * A value gate is addressed by "value" followed by CAST(val AS varchar), and
 * carries that text as its extra.  The cast is resolved as the parser resolves
 * it (a cast function, the output function, or nothing for a text-like type),
 * once per call site: true::varchar is 'true' where the output function of
 * boolean writes 't'.
 * ------------------------------------------------------------------------- */

typedef struct VarcharCast {
  Oid type;
  CoercionPathType path;
  FmgrInfo fn;
  int nargs;
} VarcharCast;

/** @brief @c CAST(val AS varchar) of argument @p argno, as a C string. */
static char *varchar_of_argument(FunctionCallInfo fcinfo, int argno) {
  VarcharCast *cast = (VarcharCast *)fcinfo->flinfo->fn_extra;
  Oid type = get_fn_expr_argtype(fcinfo->flinfo, argno);
  Datum val = PG_GETARG_DATUM(argno);

  if (!OidIsValid(type))
    provsql_error("cannot determine the type of the value");
  if (cast == NULL || cast->type != type) {
    Oid funcid = InvalidOid;

    if (cast == NULL)
      cast = (VarcharCast *)MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
                                                   sizeof(VarcharCast));
    cast->path = find_coercion_pathway(VARCHAROID, type, COERCION_EXPLICIT,
                                       &funcid);
    if (cast->path == COERCION_PATH_FUNC) {
      fmgr_info_cxt(funcid, &cast->fn, fcinfo->flinfo->fn_mcxt);
      cast->nargs = get_func_nargs(funcid);
    } else if (cast->path != COERCION_PATH_RELABELTYPE) {
      bool is_varlena;
      cast->path = COERCION_PATH_COERCEVIAIO;
      getTypeOutputInfo(type, &funcid, &is_varlena);
      fmgr_info_cxt(funcid, &cast->fn, fcinfo->flinfo->fn_mcxt);
    }
    cast->type = type;
    fcinfo->flinfo->fn_extra = cast;
  }

  switch (cast->path) {
  case COERCION_PATH_FUNC: {
    Datum r;
    if (cast->nargs == 1)
      r = FunctionCall1(&cast->fn, val);
    else if (cast->nargs == 2)
      r = FunctionCall2(&cast->fn, val, Int32GetDatum(-1));
    else
      r = FunctionCall3(&cast->fn, val, Int32GetDatum(-1), BoolGetDatum(true));
    return text_to_cstring(DatumGetTextPP(r));
  }
  case COERCION_PATH_RELABELTYPE:
    return text_to_cstring(DatumGetTextPP(val));
  default:
    return OutputFunctionCall(&cast->fn, val);
  }
}

/* The value gates this backend has written.  The same few values come back
 * row after row (the 1 of every count), and a gate of the store stays as it
 * is once written, so there is nothing to repeat.  Emptied when it grows
 * large, and by circuit_cleanup, which may remove gates. */
static HTAB *written_values = NULL;
#define WRITTEN_VALUES_MAX 65536

void provsql_gate_builders_forget(void) {
  if (written_values != NULL) {
    hash_destroy(written_values);
    written_values = NULL;
  }
  while (planted_scopes != NULL)
    planted_forget(planted_scopes);
}

/** @brief The value gate of the text @p str, at the address named @p name
 *         (@c "value" followed by @p str, or @c "null"). */
static pg_uuid_t value_gate(const char *prefix, const char *suffix,
                            const char *extra) {
  StringInfoData buf;
  pg_uuid_t token;
  bool found;

  name_begin(&buf, prefix);
  appendStringInfoString(&buf, suffix);
  token = name_end(&buf);

  if (written_values != NULL
      && hash_get_num_entries(written_values) >= WRITTEN_VALUES_MAX) {
    hash_destroy(written_values);
    written_values = NULL;
  }
  if (written_values == NULL) {
    HASHCTL ctl;
    memset(&ctl, 0, sizeof(ctl));
    ctl.keysize = sizeof(pg_uuid_t);
    ctl.entrysize = sizeof(pg_uuid_t);
    ctl.hcxt = TopMemoryContext;
    written_values = hash_create("ProvSQL value gates written", 256, &ctl,
                                 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
  }
  if (hash_search(written_values, &token, HASH_FIND, NULL) == NULL) {
    provsql_internal_create_gate_with(&token, gate_value, 0, NULL,
                                      false, 0, 0, extra);
    hash_search(written_values, &token, HASH_ENTER, &found);
  }
  return token;
}

/** @brief The semimod gate @p value_token ⊗ @p token. */
static Datum semimod_gate(const pg_uuid_t *value_token, const pg_uuid_t *token) {
  StringInfoData buf;
  pg_uuid_t semimod, children[2];

  name_begin(&buf, "semimod");
  name_add_uuid(&buf, value_token);
  name_add_uuid(&buf, token);
  semimod = name_end(&buf);
  children[0] = *token;
  children[1] = *value_token;
  provsql_internal_create_gate(&semimod, gate_semimod, 2, children);
  return uuid_result(&semimod);
}

static const pg_uuid_t *semimod_token_argument(FunctionCallInfo fcinfo) {
  if (PG_ARGISNULL(1))
    provsql_error("provenance_semimod: the provenance token must not be NULL");
  return PG_GETARG_UUID_P(1);
}

PG_FUNCTION_INFO_V1(provenance_semimod);
/**
 * @brief The semimodule gate @c val ⊗ @c token of an aggregated row.
 *
 * A NULL value does not take part in the aggregate (SQL aggregates skip NULL
 * inputs; @c count(*) passes a constant 1): no gate, NULL is returned and
 * @c provenance_aggregate drops it.
 */
Datum provenance_semimod(PG_FUNCTION_ARGS) {
  const pg_uuid_t *token;
  pg_uuid_t value_token;
  char *str;

  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();
  token = semimod_token_argument(fcinfo);
  str = varchar_of_argument(fcinfo, 0);
  value_token = value_gate("value", str, str);
  return semimod_gate(&value_token, token);
}

PG_FUNCTION_INFO_V1(provenance_semimod_nullable);
/**
 * @brief @c provenance_semimod for the aggregates that see their NULL inputs
 *        (@c array_agg, @c json_agg, ...): a NULL value gives a gate over the
 *        constant value gate @c gate_null().
 */
Datum provenance_semimod_nullable(PG_FUNCTION_ARGS) {
  const pg_uuid_t *token = semimod_token_argument(fcinfo);
  pg_uuid_t value_token;

  if (PG_ARGISNULL(0))
    value_token = value_gate("null", "", "NULL");
  else {
    char *str = varchar_of_argument(fcinfo, 0);
    value_token = value_gate("value", str, str);
  }
  return semimod_gate(&value_token, token);
}

static bool same_token(const pg_uuid_t *a, const pg_uuid_t *b);

PG_FUNCTION_INFO_V1(provenance_semimod_nested);
/**
 * @brief The semimodule gate of a row whose value is itself an aggregate
 *        result: @c semimod(the inner aggregate's gate, @p token).
 *
 * The contribution of an outer aggregate that reads an aggregate result of
 * another kind (an avg of a count, a max of a sum, an aggregate of an
 * arithmetic expression over aggregates).  Such a value is not one value of
 * the database but one per possible world, so the M side of the semimod is
 * the inner aggregate's own gate rather than a @c gate_value: an evaluator
 * that reads a value per world (the sampler) resolves it, and the closed
 * forms, which read the M side as a constant, decline it.
 */
Datum provenance_semimod_nested(PG_FUNCTION_ARGS) {
  const pg_uuid_t *token;
  agg_token *aggtok;
  pg_uuid_t inner;

  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();
  token = semimod_token_argument(fcinfo);
  aggtok = (agg_token *)PG_GETARG_POINTER(0);
  inner = *DatumGetUUIDP(DirectFunctionCall1(uuid_in,
                                             CStringGetDatum(aggtok->tok)));
  return semimod_gate(&inner, token);
}

PG_FUNCTION_INFO_V1(provenance_semimod_flat);
/**
 * @brief The contributions @c semimod(v_i, token ⊗ k_i) of the aggregate
 *        result @c val, whose gate aggregates the contributions
 *        @c semimod(v_i, k_i), for an aggregate of the same kind over it.
 *
 * @c token ⊗ (⊕ k_i ⊗ v_i) = ⊕ (token ⊗ k_i) ⊗ v_i: the outer aggregate of
 * the rows of the groups, in every semiring.  An empty array for a NULL
 * value, which the outer aggregate skips.
 */
Datum provenance_semimod_flat(PG_FUNCTION_ARGS) {
  const pg_uuid_t *token = semimod_token_argument(fcinfo);
  agg_token *aggtok;
  pg_uuid_t agg, *children = NULL, *out;
  unsigned n = 0, i;
  gate_type type;

  if (PG_ARGISNULL(0))
    PG_RETURN_ARRAYTYPE_P(construct_empty_array(UUIDOID));
  aggtok = (agg_token *)PG_GETARG_POINTER(0);
  agg = *DatumGetUUIDP(DirectFunctionCall1(uuid_in,
                                           CStringGetDatum(aggtok->tok)));
  type = provsql_fetch_gate(&agg, &n, &children);
  if (type != gate_agg)
    provsql_error("provenance_semimod_flat: not the result of an aggregate");

  out = (pg_uuid_t *)palloc(sizeof(pg_uuid_t) * (n > 0 ? n : 1));
  for (i = 0; i < n; ++i) {
    unsigned m = 0;
    pg_uuid_t *wires = NULL, scaled, pair[2];

    if (provsql_fetch_gate(&children[i], &m, &wires) != gate_semimod || m != 2)
      provsql_error("provenance_semimod_flat: unexpected contribution");
    pair[0] = *token;
    pair[1] = wires[0];
    if (same_token(token, address_of_one()))
      scaled = wires[0];
    else if (same_token(&wires[0], address_of_one()))
      scaled = *token;
    else
      scaled = nary_gate(gate_times, "times", "times-canonical", pair, 2);
    out[i] = *DatumGetUUIDP(semimod_gate(&wires[1], &scaled));
    free(wires);
  }
  free(children);
  {
    Datum *elems = (Datum *)palloc(sizeof(Datum) * (n > 0 ? n : 1));
    for (i = 0; i < n; ++i)
      elems[i] = UUIDPGetDatum(&out[i]);
    PG_RETURN_ARRAYTYPE_P(construct_array(elems, (int)n, UUIDOID, UUID_LEN,
                                          false, 'c'));
  }
}

PG_FUNCTION_INFO_V1(provenance_aggregate);
/**
 * @brief The @c agg gate of a group, paired with the aggregate's value.
 *
 * The address hashes everything the gate records: the aggregate function
 * and the scalar flag (info1 and the high bit of info2), the children, then
 * the result type (the low bits of info2) and the value text (the extra),
 * after a colon each.  A gate's infos and text are written once, so what is
 * written has to follow from the address: @c SUM(x) and @c AVG(x) over the
 * same children, a scalar and a grouped aggregate, @c array_agg over
 * integers and over their texts (same children, another result type), or a
 * floating-point sum whose rounding depends on the plan, are all different
 * gates.  NULL children are rows whose value was NULL; they are dropped.
 * No child gives 𝟘 for a group, and an agg gate without children for a
 * scalar aggregation, whose value over no row is still defined.
 */
Datum provenance_aggregate(PG_FUNCTION_ARGS) {
  int32 aggfnoid, aggtype;
  bool is_scalar = !PG_ARGISNULL(4) && PG_GETARG_BOOL(4);
  pg_uuid_t *tokens = NULL, agg, nothing;
  char *val = NULL;
  int n = 0;
  agg_token *result;

  if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    provsql_error("provenance_aggregate: the aggregate function and its "
                  "result type must not be NULL");
  aggfnoid = PG_GETARG_INT32(0);
  aggtype = PG_GETARG_INT32(1);
  if (!PG_ARGISNULL(2))
    val = varchar_of_argument(fcinfo, 2);
  if (!PG_ARGISNULL(3)) {
    memset(&nothing, 0xFF, sizeof(nothing));
    n = filtered_tokens(PG_GETARG_ARRAYTYPE_P(3), &nothing, &tokens);
  }

  /* A group without rows is no group; a scalar aggregation over no row has a
   * value (0 for a count), that of an agg gate without children. */
  if (n == 0 && !is_scalar)
    agg = *address_of_zero();
  else {
    StringInfoData buf;

    name_begin(&buf, "agg");
    appendStringInfo(&buf, "%d", aggfnoid);
    name_add_uuid_array(&buf, tokens, n);
    if (is_scalar)
      appendStringInfoChar(&buf, 'S');
    appendStringInfo(&buf, ":%d", aggtype);
    if (val != NULL)
      appendStringInfo(&buf, ":%s", val);
    agg = name_end(&buf);
    provsql_internal_create_gate_with(&agg, gate_agg, (unsigned)n, tokens,
                                      true, (unsigned)aggfnoid,
                                      is_scalar ? ((unsigned)aggtype | 0x80000000u)
                                                : (unsigned)aggtype,
                                      val);
  }

  /* A NULL value over no row at all is SQL's NULL: there is nothing to
   * keep.  Over rows absent from the database as it is (the displayed value
   * reads only the present ones), the group exists in other worlds: the
   * token keeps its circuit, with a NULL value (an empty val). */
  if (val == NULL && n == 0)
    PG_RETURN_NULL();
  result = (agg_token *)palloc0(sizeof(agg_token));
  {
    StringInfoData t;
    initStringInfo(&t);
    name_add_uuid(&t, &agg);
    memcpy(result->tok, t.data, 36);
    pfree(t.data);
  }
  if (val != NULL)
    agg_token_set_value(result, val, strlen(val));
  PG_RETURN_POINTER(result);
}

static bool same_token(const pg_uuid_t *a, const pg_uuid_t *b) {
  return memcmp(a->data, b->data, UUID_LEN) == 0;
}

PG_FUNCTION_INFO_V1(provenance_monus);
/**
 * @brief @c token1 ⊖ @c token2.
 *
 * A NULL second argument is the row without a match in the outer join of the
 * difference: nothing to subtract, and X ⊖ 𝟘 = X.  (Not the NULL ≡ 𝟙 of
 * @c provenance_times: each combinator reads NULL as its own neutral.)
 * X ⊖ X = 𝟘 and 𝟘 ⊖ X = 𝟘 are applied as well.
 */
Datum provenance_monus(PG_FUNCTION_ARGS) {
  const pg_uuid_t *token1, *token2;
  StringInfoData buf;
  pg_uuid_t monus, children[2];

  if (PG_ARGISNULL(0))
    ereport(ERROR,
            (errmsg("provenance_monus is called with first argument NULL")));
  token1 = PG_GETARG_UUID_P(0);
  if (PG_ARGISNULL(1))
    return uuid_result(token1);
  token2 = PG_GETARG_UUID_P(1);

  if (same_token(token1, token2) || same_token(token1, address_of_zero()))
    return uuid_result(address_of_zero());
  if (same_token(token2, address_of_zero()))
    return uuid_result(token1);

  name_begin(&buf, "monus");
  name_add_uuid(&buf, token1);
  name_add_uuid(&buf, token2);
  monus = name_end(&buf);
  children[0] = *token1;
  children[1] = *token2;
  provsql_internal_create_gate(&monus, gate_monus, 2, children);
  return uuid_result(&monus);
}

PG_FUNCTION_INFO_V1(provenance_delta);
/**
 * @brief δ(@c token).  A NULL token is an untracked source, 𝟙, and δ(𝟙) = 𝟙;
 *        δ(𝟘) = 𝟘.
 */
Datum provenance_delta(PG_FUNCTION_ARGS) {
  const pg_uuid_t *token;
  StringInfoData buf;
  pg_uuid_t delta;

  if (PG_ARGISNULL(0))
    return uuid_result(address_of_one());
  token = PG_GETARG_UUID_P(0);
  if (same_token(token, address_of_zero()) || same_token(token, address_of_one()))
    return uuid_result(token);

  name_begin(&buf, "delta");
  name_add_uuid(&buf, token);
  delta = name_end(&buf);
  provsql_internal_create_gate(&delta, gate_delta, 1, token);
  return uuid_result(&delta);
}

PG_FUNCTION_INFO_V1(provenance_cmp);
/**
 * @brief The comparison gate @c left @c op @c right of a HAVING condition.
 *
 * A comparison with a NULL operand is unknown in every possible world: the
 * row is annotated 𝟘.  Hence not strict: a NULL result would read as the
 * neutral of ⊗ and turn "unknown" into "certainly true".
 */
Datum provenance_cmp(PG_FUNCTION_ARGS) {
  const pg_uuid_t *left, *right;
  Oid op;
  StringInfoData buf;
  pg_uuid_t cmp, children[2];

  if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
    return uuid_result(address_of_zero());
  left = PG_GETARG_UUID_P(0);
  op = PG_GETARG_OID(1);
  right = PG_GETARG_UUID_P(2);

  name_begin(&buf, "cmp");
  name_add_uuid(&buf, left);
  appendStringInfo(&buf, "%u", op);
  name_add_uuid(&buf, right);
  cmp = name_end(&buf);
  children[0] = *left;
  children[1] = *right;
  provsql_internal_create_gate_with(&cmp, gate_cmp, 2, children,
                                    true, (unsigned)op, 0, NULL);
  return uuid_result(&cmp);
}

PG_FUNCTION_INFO_V1(annotate);
/**
 * @brief A transparent @c annotation gate over @c token, carrying @c extra.
 *
 * The address hashes @c extra along with the child: two annotations of one
 * token with different texts are different gates.  NULL on a NULL token.
 */
Datum annotate(PG_FUNCTION_ARGS) {
  const pg_uuid_t *token;
  char *extra = NULL;
  StringInfoData buf;
  pg_uuid_t annotated;

  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();
  token = PG_GETARG_UUID_P(0);
  if (!PG_ARGISNULL(1))
    extra = text_to_cstring(PG_GETARG_TEXT_PP(1));

  name_begin(&buf, "annotation");
  name_add_uuid(&buf, token);
  if (extra != NULL)
    appendStringInfoString(&buf, extra);
  annotated = name_end(&buf);
  provsql_internal_create_gate_with(&annotated, gate_annotation, 1, token,
                                    false, 0, 0, extra);
  return uuid_result(&annotated);
}

PG_FUNCTION_INFO_V1(inversion_free_key);
/**
 * @brief The order key of an input on the inversion-free route,
 *        @c "K<factor> <bytes of root>:<root><bytes of sec>:<sec>".  Strict.
 */
Datum inversion_free_key(PG_FUNCTION_ARGS) {
  text *root = PG_GETARG_TEXT_PP(0), *sec = PG_GETARG_TEXT_PP(1);
  StringInfoData buf;

  initStringInfo(&buf);
  appendStringInfo(&buf, "K%d %d:", PG_GETARG_INT32(2),
                   (int)VARSIZE_ANY_EXHDR(root));
  appendBinaryStringInfo(&buf, VARDATA_ANY(root), VARSIZE_ANY_EXHDR(root));
  appendStringInfo(&buf, "%d:", (int)VARSIZE_ANY_EXHDR(sec));
  appendBinaryStringInfo(&buf, VARDATA_ANY(sec), VARSIZE_ANY_EXHDR(sec));
  PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

/* -------------------------------------------------------------------------
 * The other content-addressed builders: assumption wrappers, where-provenance
 * gates, arithmetic.  Each records what its address hashes, so the gate goes
 * with its infos or text in one unanswered message.
 * ------------------------------------------------------------------------- */

/** @brief The @c assumed wrapper of @p token under @p assumption, with
 *         @p info1 as the route tag (0 for none). */
static pg_uuid_t assumed_gate(const pg_uuid_t *token, const char *assumption,
                              unsigned info1) {
  StringInfoData buf;
  pg_uuid_t wrapped;

  if (strcmp(assumption, "boolean") != 0 && strcmp(assumption, "absorptive") != 0)
    ereport(ERROR, (errmsg("provenance_assume: unknown assumption %s", assumption)));
  name_begin(&buf, "assumed");
  appendStringInfoString(&buf, assumption);
  name_add_uuid(&buf, token);
  wrapped = name_end(&buf);
  provsql_internal_create_gate_with(&wrapped, gate_assumed, 1, token,
                                    info1 != 0, info1, 0, assumption);
  return wrapped;
}

PG_FUNCTION_INFO_V1(provenance_assume);
/** @brief Wrap @c token in the assumption marker @c assumption
 *         (@c 'boolean' or @c 'absorptive').  NULL on a NULL token. */
Datum provenance_assume(PG_FUNCTION_ARGS) {
  pg_uuid_t wrapped;

  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();
  if (PG_ARGISNULL(1))
    ereport(ERROR, (errmsg("provenance_assume: unknown assumption NULL")));
  wrapped = assumed_gate(PG_GETARG_UUID_P(0),
                         text_to_cstring(PG_GETARG_TEXT_PP(1)), 0);
  return uuid_result(&wrapped);
}

PG_FUNCTION_INFO_V1(assume_boolean);
/** @brief The Boolean-assumption wrapper the safe-query rewriter puts on
 *         every per-row root, tagged @c PROVSQL_ROUTE_SQ_REWRITE. */
Datum assume_boolean(PG_FUNCTION_ARGS) {
  pg_uuid_t wrapped;

  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();
  wrapped = assumed_gate(PG_GETARG_UUID_P(0), "boolean",
                         PROVSQL_ROUTE_SQ_REWRITE);
  return uuid_result(&wrapped);
}

PG_FUNCTION_INFO_V1(provenance_project);
/**
 * @brief The where-provenance @c project gate of @c token: its text lists,
 *        for each output position, the input position it comes from (NULL
 *        for a position given as 0), as @c '{{in,out},...}'.
 */
Datum provenance_project(PG_FUNCTION_ARGS) {
  const pg_uuid_t *token;
  ArrayType *positions;
  Datum *elems;
  bool *nulls;
  int n, i;
  StringInfoData buf, extra;
  pg_uuid_t project;

  if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    provsql_error("provenance_project: NULL argument");
  token = PG_GETARG_UUID_P(0);
  positions = PG_GETARG_ARRAYTYPE_P(1);
  deconstruct_array(positions, INT4OID, sizeof(int32), true, 'i',
                    &elems, &nulls, &n);

  /* The address hashes the array as its text form, '{p1,p2,...}'. */
  name_begin(&buf, "project");
  name_add_uuid(&buf, token);
  appendStringInfoChar(&buf, '{');
  initStringInfo(&extra);
  appendStringInfoChar(&extra, '{');
  for (i = 0; i < n; ++i) {
    if (i > 0) {
      appendStringInfoChar(&buf, ',');
      appendStringInfoChar(&extra, ',');
    }
    if (nulls[i]) {
      appendStringInfoString(&buf, "NULL");
      appendStringInfo(&extra, "{NULL,%d}", i + 1);
    } else {
      int32 pos = DatumGetInt32(elems[i]);
      appendStringInfo(&buf, "%d", pos);
      if (pos == 0)
        appendStringInfo(&extra, "{NULL,%d}", i + 1);
      else
        appendStringInfo(&extra, "{%d,%d}", pos, i + 1);
    }
  }
  appendStringInfoChar(&buf, '}');
  appendStringInfoChar(&extra, '}');
  project = name_end(&buf);
  provsql_internal_create_gate_with(&project, gate_project, 1, token,
                                    false, 0, 0, n > 0 ? extra.data : NULL);
  return uuid_result(&project);
}

PG_FUNCTION_INFO_V1(provenance_eq);
/** @brief The where-provenance @c eq gate of @c token, equating positions
 *         @c pos1 and @c pos2 (its infos). */
Datum provenance_eq(PG_FUNCTION_ARGS) {
  const pg_uuid_t *token;
  StringInfoData buf;
  pg_uuid_t eq;
  int32 pos1 = PG_ARGISNULL(1) ? 0 : PG_GETARG_INT32(1);
  int32 pos2 = PG_ARGISNULL(2) ? 0 : PG_GETARG_INT32(2);

  if (PG_ARGISNULL(0))
    provsql_error("provenance_eq: NULL token");
  token = PG_GETARG_UUID_P(0);
  name_begin(&buf, "eq");
  name_add_uuid(&buf, token);
  if (!PG_ARGISNULL(1))
    appendStringInfo(&buf, "%d", pos1);
  appendStringInfoChar(&buf, ',');
  if (!PG_ARGISNULL(2))
    appendStringInfo(&buf, "%d", pos2);
  eq = name_end(&buf);
  provsql_internal_create_gate_with(&eq, gate_eq, 1, token,
                                    true, (unsigned)pos1, (unsigned)pos2, NULL);
  return uuid_result(&eq);
}

PG_FUNCTION_INFO_V1(provenance_arith);
/** @brief The @c arith gate applying operator @c op (a @c provsql_arith_op,
 *         its info1) to @c children, in order. */
Datum provenance_arith(PG_FUNCTION_ARGS) {
  StringInfoData buf;
  pg_uuid_t arith, *children = NULL, nothing;
  int n = 0;
  int32 op = PG_ARGISNULL(0) ? 0 : PG_GETARG_INT32(0);

  name_begin(&buf, "arith");
  if (!PG_ARGISNULL(0))
    appendStringInfo(&buf, "%d", op);
  if (!PG_ARGISNULL(1)) {
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(1);
    if (array_contains_nulls(arr))
      provsql_error("provenance_arith: children array must not contain NULL elements");
    memset(&nothing, 0xFF, sizeof(nothing));
    n = filtered_tokens(arr, &nothing, &children);
    name_add_uuid_array(&buf, children, n);
  }
  arith = name_end(&buf);
  provsql_internal_create_gate_with(&arith, gate_arith, (unsigned)n, children,
                                    true, (unsigned)op, 0, NULL);
  return uuid_result(&arith);
}
