/**
 * @file provsql_mmap.c
 * @brief Background worker registration and IPC primitives for mmap-backed storage.
 *
 * Implements the PostgreSQL background worker lifecycle functions declared
 * in @c provsql_mmap.h:
 * - @c RegisterProvSQLMMapWorker(): registers the worker with the postmaster
 *   during @c _PG_init().
 * - @c provsql_mmap_worker(): worker entry point; sets up signal handlers
 *   and enters @c provsql_mmap_main_loop().
 *
 * The IPC between normal backends and the background worker is handled in
 * @c MMappedCircuit.cpp.  This file provides the PostgreSQL-specific glue
 * (background worker API, signal handling).
 *
 * Also declares the shared write buffer @c buffer[] and position counter
 * @c bufferpos used by the @c STARTWRITEM / @c ADDWRITEM / @c SENDWRITEM
 * macros in @c provsql_mmap.h.
 *
 * The gate-creation SQL functions (e.g. @c create_gate()) that backends
 * call are also implemented here; they acquire the IPC lock, write a
 * message to the background worker, and wait for an acknowledgment.
 */
#include "provsql_mmap.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"
#include "MMappedTableInfo.h"

#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <math.h>
#include <assert.h>

#include "postgres.h"
#include "postmaster/bgworker.h"
#include "catalog/pg_type.h"          /* INT2OID etc. -- the _d.h variant
                                         only exists from PG 11 onwards */
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "access/htup_details.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "circuit_cache.h"

char buffer[PIPE_BUF]={}; // flawfinder: ignore
unsigned bufferpos=0;

PGDLLEXPORT void provsql_mmap_worker(Datum ignored)
{
  BackgroundWorkerUnblockSignals();
  initialize_provsql_mmap();
  close(provsql_shared_state->pipebmw);
  close(provsql_shared_state->pipembr);
  provsql_log("%s initialized", MyBgworkerEntry->bgw_name);

  provsql_mmap_main_loop();

  destroy_provsql_mmap();
}

void RegisterProvSQLMMapWorker(void)
{
  BackgroundWorker worker;

  snprintf(worker.bgw_name, BGW_MAXLEN, "ProvSQL MMap Worker");
#if PG_VERSION_NUM >= 110000
  snprintf(worker.bgw_type, BGW_MAXLEN, "ProvSQL MMap");
#endif

  worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
  worker.bgw_start_time = BgWorkerStart_PostmasterStart;
  worker.bgw_restart_time = 1;

  snprintf(worker.bgw_library_name, BGW_MAXLEN, "provsql");
  snprintf(worker.bgw_function_name, BGW_MAXLEN, "provsql_mmap_worker");
#if PG_VERSION_NUM < 100000
  worker.bgw_main = NULL;
#endif

  worker.bgw_main_arg = (Datum) 0;
  worker.bgw_notify_pid = 0;

  RegisterBackgroundWorker(&worker);
}

PG_FUNCTION_INFO_V1(get_gate_type);
/** @brief PostgreSQL-callable wrapper for get_gate_type().
 *
 * On cache miss this fetches BOTH the gate type and its children from
 * the worker, in one critical section, then caches them together. If
 * we cached only the type (with an empty children list), a subsequent
 * get_children() call for the same token would consult the cache, find
 * the entry, and return 0 children : never querying the worker for the
 * real children. provsql.provenance_evaluate hits exactly that pattern
 * (it calls get_gate_type first, then unnest(get_children(...))) and
 * silently folds plus/times gates over an empty set.
 */
Datum get_gate_type(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  gate_type type;
  constants_t constants=get_constants(true);
  unsigned nb_children = 0;
  pg_uuid_t *children = NULL;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  type = circuit_cache_get_type(*token);
  if(type!=gate_invalid)
    PG_RETURN_INT32(constants.GATE_TYPE_TO_OID[type]); ;

  /* Type fetch (message 't'). */
  STARTWRITEM();
  ADDWRITEM("t", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(type, gate_type)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate on pipe (message type t)");
  }

  /* Children fetch (message 'c'), batched in the same critical
   * section so the cache entry below is complete. Skipped when the
   * token is unknown (worker reports gate_invalid). */
  if(type != gate_invalid) {
    STARTWRITEM();
    ADDWRITEM("c", char);
    ADDWRITEM(&MyDatabaseId, Oid);
    ADDWRITEM(token, pg_uuid_t);

    if(!SENDWRITEM() || !READB(nb_children, unsigned)) {
      provsql_shmem_unlock();
      provsql_error("Cannot communicate on pipe (message type c during get_gate_type)");
    }

    if(nb_children > 0) {
      char *p;
      ssize_t actual_read, remaining_size;

      children = calloc(nb_children, sizeof(pg_uuid_t));
      p = (char*)children;
      remaining_size = nb_children * sizeof(pg_uuid_t);
      while((actual_read = read(provsql_shared_state->pipembr, p, remaining_size)) < remaining_size) {
        if(actual_read <= 0) {
          provsql_shmem_unlock();
          provsql_error("Cannot read children from pipe (during get_gate_type)");
        } else {
          remaining_size -= actual_read;
          p += actual_read;
        }
      }
    }
  }

  provsql_shmem_unlock();

  /* Skip caching the gate_input lazy default: MMappedCircuit::getGateType
   * returns gate_input both for real input gates and for tokens that are
   * not yet in the mapping. Caching the latter would poison subsequent
   * create_gate() calls in this session (the cache hit would short-circuit
   * the worker IPC, dropping the gate). The cost is one extra IPC per
   * lookup of a real input gate -- acceptable. */
  if(!(type == gate_input && nb_children == 0))
    circuit_cache_create_gate(*token, type, nb_children, children);
  if(children) free(children);
  PG_RETURN_INT32(constants.GATE_TYPE_TO_OID[type]);
}

PG_FUNCTION_INFO_V1(create_gate);
/** @brief PostgreSQL-callable wrapper for create_gate(). */
Datum create_gate(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  Oid oid_type = PG_GETARG_INT32(1);
  ArrayType *children = PG_ARGISNULL(2)?NULL:PG_GETARG_ARRAYTYPE_P(2);
  unsigned nb_children = 0;
  gate_type type = gate_invalid;
  constants_t constants;
  pg_uuid_t *children_data;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    provsql_error("Invalid NULL value passed to create_gate");

  if(children) {
    if(ARR_NDIM(children) > 1)
      provsql_error("Invalid multi-dimensional array passed to create_gate");
    else if(ARR_NDIM(children) == 1)
      nb_children = *ARR_DIMS(children);
  }

  constants=get_constants(true);

  for(int i=0; i<nb_gate_types; ++i) {
    if(constants.GATE_TYPE_TO_OID[i]==oid_type) {
      type = i;
      break;
    }
  }
  if(type == gate_invalid) {
    provsql_error("Invalid gate type");
  }

  if(nb_children>0)
    children_data = (pg_uuid_t*) ARR_DATA_PTR(children);
  else
    children_data = NULL;

  /* Populate the per-session cache, but unconditionally fall through to
   * the worker IPC: a cache hit only proves "this token has been seen
   * in this session before" (e.g. by get_gate_type returning the
   * gate_input lazy default for an unknown token) -- not "the worker
   * already has a gate for it". Skipping the IPC on a cache hit caused
   * silently-dropped create_gate calls under concurrent backends.
   * MMappedCircuit::createGate is idempotent on already-mapped tokens. */
  circuit_cache_create_gate(*token, type, nb_children, children_data);

  STARTWRITEM();
  ADDWRITEM("C", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&type, gate_type);
  ADDWRITEM(&nb_children, unsigned);

  if(PIPE_BUF-bufferpos>nb_children*sizeof(pg_uuid_t)) {
    // Enough space in the buffer for an atomic write, no need of
    // exclusive locks

    for(int i=0; i<nb_children; ++i)
      ADDWRITEM(&children_data[i], pg_uuid_t);

    provsql_shmem_lock_shared();
    if(!SENDWRITEM()) {
      provsql_shmem_unlock();
      provsql_error("Cannot write to pipe (message type C)");
    }
    provsql_shmem_unlock();
  } else {
    // Not enough space in buffer, pipe write won't be atomic, we need to
    // make several writes and use locks
    unsigned children_per_batch = PIPE_BUF/sizeof(pg_uuid_t);

    provsql_shmem_lock_exclusive();

    if(!SENDWRITEM()) {
      provsql_shmem_unlock();
      provsql_error("Cannot write to pipe (message type C)");
    }

    for(unsigned j=0; j<1+(nb_children-1)/children_per_batch; ++j) {
      STARTWRITEM();

      for(unsigned i=j*children_per_batch; i<(j+1)*children_per_batch && i<nb_children; ++i) {
        ADDWRITEM(&children_data[i], pg_uuid_t);
      }

      if(!SENDWRITEM()) {
        provsql_shmem_unlock();
        provsql_error("Cannot write to pipe (message type C)");
      }
    }

    provsql_shmem_unlock();
  }

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_prob);
/** @brief PostgreSQL-callable wrapper for set_prob(). */
Datum set_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  double prob = PG_GETARG_FLOAT8(1);
  char result;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    provsql_error("Invalid NULL value passed to set_prob");

  STARTWRITEM();
  ADDWRITEM("P", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&prob, double);

  provsql_shmem_lock_shared();
  if(!SENDWRITEM() || !READB(result, char)) {
    provsql_shmem_unlock();
    provsql_error("Cannot write to pipe");
  }
  provsql_shmem_unlock();

  if(!result)
    provsql_error("set_prob called on non-input gate");

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_infos);
/** @brief PostgreSQL-callable wrapper for set_infos(). */
Datum set_infos(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  unsigned info1 = PG_GETARG_INT32(1);
  unsigned info2 = PG_GETARG_INT32(2);


  if(PG_ARGISNULL(1))
    info1=0;
  if(PG_ARGISNULL(2))
    info2=0;

  STARTWRITEM();
  ADDWRITEM("I", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&info1, unsigned);
  ADDWRITEM(&info2, unsigned);

  provsql_shmem_lock_shared();
  if(!SENDWRITEM()) {
    provsql_shmem_unlock();
    provsql_error("Cannot write to pipe (message type I)");
  }
  provsql_shmem_unlock();

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_extra);
/** @brief PostgreSQL-callable wrapper for set_extra(). */
Datum set_extra(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  text *data = PG_GETARG_TEXT_P(1);
  char *str=text_to_cstring(data);
  unsigned len=strlen(str);

  STARTWRITEM();
  ADDWRITEM("E", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&len, unsigned);

  assert(PIPE_BUF-bufferpos>len);
  memcpy(buffer+bufferpos, str, len), bufferpos+=len;
  pfree(str);

  provsql_shmem_lock_shared();
  if(!SENDWRITEM()) {
    provsql_shmem_unlock();
    provsql_error("Cannot write to pipe (message type E)");
  }
  provsql_shmem_unlock();

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_extra);
/** @brief PostgreSQL-callable wrapper for get_extra(). */
Datum get_extra(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  text *result;
  unsigned len;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  STARTWRITEM();
  ADDWRITEM("e", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(len, unsigned)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type e)");
  }

  result = palloc(len + VARHDRSZ);
  SET_VARSIZE(result, VARHDRSZ + len);

  if(read(provsql_shared_state->pipembr, VARDATA(result), len)<len) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type e)");
  }

  provsql_shmem_unlock();

  PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(get_nb_gates);
/** @brief PostgreSQL-callable wrapper for get_nb_gates(). */
Datum get_nb_gates(PG_FUNCTION_ARGS)
{
  unsigned long nb;

  STARTWRITEM();
  ADDWRITEM("n", char);
  ADDWRITEM(&MyDatabaseId, Oid);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(nb, unsigned long)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type n)");
  }

  provsql_shmem_unlock();

  PG_RETURN_INT64((long) nb);
}

PG_FUNCTION_INFO_V1(get_children);
/** @brief PostgreSQL-callable wrapper for get_children(). */
Datum get_children(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  ArrayType *result = NULL;
  unsigned nb_children;
  pg_uuid_t *children;
  Datum *children_ptr;
  constants_t constants;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  nb_children = circuit_cache_get_children(*token, &children);

  if(!children) {
    STARTWRITEM();
    ADDWRITEM("c", char);
    ADDWRITEM(&MyDatabaseId, Oid);
    ADDWRITEM(token, pg_uuid_t);

    provsql_shmem_lock_exclusive();

    if(!SENDWRITEM()) {
      provsql_shmem_unlock();
      provsql_error("Cannot write to pipe (message type c)");
    }

    if(!READB(nb_children, unsigned)) {
      provsql_shmem_unlock();
      provsql_error("Cannot read response from pipe (message type c)");
    }

    children=calloc(nb_children, sizeof(pg_uuid_t));

    {
      char *p = (char*)children;
      ssize_t actual_read, remaining_size=nb_children*sizeof(pg_uuid_t);
      while((actual_read=read(provsql_shared_state->pipembr, p, remaining_size))<remaining_size) {
        if(actual_read<=0) {
          provsql_shmem_unlock();
          provsql_error("Cannot read from pipe (message type c)");
        } else {
          remaining_size-=actual_read;
          p+=actual_read;
        }
      }
    }
    provsql_shmem_unlock();

    /* Skip caching when the worker reports zero children: we cannot
     * distinguish a real zero-child gate (input/zero/one/...) from a
     * token unknown to the worker, and caching the latter poisons
     * subsequent create_gate() calls in this session. */
    if(nb_children > 0)
      circuit_cache_create_gate(*token, gate_invalid, nb_children, children);
  }

  children_ptr = palloc(nb_children * sizeof(Datum));
  for(unsigned i=0; i<nb_children; ++i)
    children_ptr[i] = UUIDPGetDatum(&children[i]);

  constants=get_constants(true);
  result = construct_array(
    children_ptr,
    nb_children,
    constants.OID_TYPE_UUID,
    16,
    false,
    'c');
  pfree(children_ptr);
  free(children);

  PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(get_prob);
/** @brief PostgreSQL-callable wrapper for get_prob(). */
Datum get_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  double result;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  STARTWRITEM();
  ADDWRITEM("p", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(result, double)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type p)");
  }

  provsql_shmem_unlock();

  if(isnan(result))
    PG_RETURN_NULL();
  else
    PG_RETURN_FLOAT8(result);
}

/** @brief Translate a SQL-side kind label into the persisted enum value. */
static uint8_t parse_table_kind(const char *label)
{
  if(strcmp(label, "tid") == 0) return PROVSQL_TABLE_TID;
  if(strcmp(label, "bid") == 0) return PROVSQL_TABLE_BID;
  if(strcmp(label, "opaque") == 0) return PROVSQL_TABLE_OPAQUE;
  provsql_error("set_table_info: unknown table kind '%s' (expected "
                "'tid', 'bid', or 'opaque')", label);
  return PROVSQL_TABLE_TID;  /* unreachable */
}

/** @brief Inverse of @c parse_table_kind for use by @c get_table_info. */
static const char *table_kind_label(uint8_t kind)
{
  switch(kind) {
  case PROVSQL_TABLE_TID:    return "tid";
  case PROVSQL_TABLE_BID:    return "bid";
  case PROVSQL_TABLE_OPAQUE: return "opaque";
  }
  provsql_error("get_table_info: unknown table kind value %u", kind);
  return NULL;  /* unreachable */
}

PG_FUNCTION_INFO_V1(set_table_info);
/**
 * @brief PostgreSQL-callable wrapper for setTableInfo() over the IPC pipe.
 *
 * Stores per-relation provenance metadata used by the safe-query
 * optimisation.  @p relid is the @c pg_class OID of the relation;
 * @p kind is one of the textual labels @c 'tid' / @c 'bid' /
 * @c 'opaque' (see @c provsql_table_kind in @c MMappedTableInfo.h);
 * @p block_key is an @c int2 array (possibly empty) listing the
 * block-key column numbers when @p kind is @c 'bid'.
 */
Datum set_table_info(PG_FUNCTION_ARGS)
{
  Oid relid;
  text *kind_text;
  char *kind_str;
  uint8_t kind;
  ArrayType *block_key;
  uint16 block_key_n = 0;
  int16 *block_key_data = NULL;
  Size payload_size;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    provsql_error("Invalid NULL value passed to set_table_info");

  relid     = PG_GETARG_OID(0);
  kind_text = PG_GETARG_TEXT_PP(1);
  kind_str  = text_to_cstring(kind_text);
  kind      = parse_table_kind(kind_str);
  pfree(kind_str);
  block_key = PG_ARGISNULL(2) ? NULL : PG_GETARG_ARRAYTYPE_P(2);

  if(block_key) {
    if(ARR_NDIM(block_key) > 1)
      provsql_error("Invalid multi-dimensional array passed to set_table_info");
    else if(ARR_NDIM(block_key) == 1)
      block_key_n = *ARR_DIMS(block_key);
    if(block_key_n > 0)
      block_key_data = (int16 *) ARR_DATA_PTR(block_key);
  }

  if(block_key_n > PROVSQL_TABLE_INFO_MAX_BLOCK_KEY)
    provsql_error("set_table_info: block key wider than %d columns "
                  "(%u given) is not supported",
                  PROVSQL_TABLE_INFO_MAX_BLOCK_KEY, block_key_n);

  payload_size = sizeof(char) + sizeof(Oid) + sizeof(Oid) + sizeof(uint8)
                 + sizeof(uint16) + block_key_n * sizeof(int16);
  if(payload_size > PIPE_BUF)
    provsql_error("set_table_info: IPC payload exceeds PIPE_BUF");

  STARTWRITEM();
  ADDWRITEM("T", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(&relid, Oid);
  ADDWRITEM(&kind, uint8);
  ADDWRITEM(&block_key_n, uint16);
  for(uint16 i = 0; i < block_key_n; ++i)
    ADDWRITEM(&block_key_data[i], int16);

  provsql_shmem_lock_shared();
  if(!SENDWRITEM()) {
    provsql_shmem_unlock();
    provsql_error("Cannot write to pipe (message type T)");
  }
  provsql_shmem_unlock();

  /* Broadcast a relcache invalidation so every backend re-fetches on
   * next access.  Standard DDL (the ALTER TABLE in add_provenance and
   * repair_key) already does this, but set_table_info is also called
   * from DML paths that do not (INSERT INTO T SELECT, UPDATE under
   * provsql.update_provenance, ...) and the upgrade-script backfill
   * runs outside any DDL on the target relation.  Guarded by a
   * pg_class existence check so the sql_drop event-trigger path,
   * which calls remove_table_info on a relid that has just been
   * deleted from pg_class, does not raise "cache lookup failed". */
  if(SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
    CacheInvalidateRelcacheByRelid(relid);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(remove_table_info);
/** @brief PostgreSQL-callable wrapper for removeTableInfo() over the IPC pipe. */
Datum remove_table_info(PG_FUNCTION_ARGS)
{
  Oid relid;

  if(PG_ARGISNULL(0))
    provsql_error("Invalid NULL value passed to remove_table_info");

  relid = PG_GETARG_OID(0);

  STARTWRITEM();
  ADDWRITEM("D", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(&relid, Oid);

  provsql_shmem_lock_shared();
  if(!SENDWRITEM()) {
    provsql_shmem_unlock();
    provsql_error("Cannot write to pipe (message type D)");
  }
  provsql_shmem_unlock();

  /* Same guard as set_table_info: skip the broadcast when the relation
   * is already gone (typical for the sql_drop event-trigger path,
   * where pg_class no longer has a row for this OID). */
  if(SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
    CacheInvalidateRelcacheByRelid(relid);

  PG_RETURN_VOID();
}

/**
 * @brief C-callable IPC fetch for per-table provenance metadata.
 *
 * Sends an @c 's' message to the background worker and reads back the
 * response.  No caching: every call hits the worker.  Use
 * @c provsql_lookup_table_info() for the cached, planner-hot-path
 * variant.
 *
 * @param relid  pg_class OID of the relation to look up.
 * @param out    On success, filled with the stored record.
 * @return @c true if the worker returned a record, @c false otherwise.
 */
bool provsql_fetch_table_info(Oid relid, ProvenanceTableInfo *out)
{
  char found;

  STARTWRITEM();
  ADDWRITEM("s", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(&relid, Oid);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(found, char)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type s)");
  }
  if(found) {
    if(!READB(out->kind, uint8_t) || !READB(out->block_key_n, uint16)) {
      provsql_shmem_unlock();
      provsql_error("Cannot communicate with pipe (message type s)");
    }
    if(out->block_key_n > PROVSQL_TABLE_INFO_MAX_BLOCK_KEY) {
      provsql_shmem_unlock();
      provsql_error("provsql_fetch_table_info: server returned an unexpectedly wide block key");
    }
    for(uint16 i = 0; i < out->block_key_n; ++i)
      if(!READB(out->block_key[i], AttrNumber)) {
        provsql_shmem_unlock();
        provsql_error("Cannot communicate with pipe (message type s)");
      }
    out->relid = relid;
  }

  provsql_shmem_unlock();
  return found != 0;
}

PG_FUNCTION_INFO_V1(get_table_info);
/**
 * @brief PostgreSQL-callable wrapper around the cached table-info lookup.
 *
 * Returns @c NULL when no record exists for @p relid; otherwise a
 * record @c (kind text, block_key int2[]) where @c kind is one of
 * @c 'tid' / @c 'bid' / @c 'opaque'.  Goes through
 * @c provsql_lookup_table_info so repeated calls in the same session
 * do not pay for IPC.
 */
Datum get_table_info(PG_FUNCTION_ARGS)
{
  Oid relid;
  ProvenanceTableInfo info;
  TupleDesc tupdesc;
  Datum values[2];
  bool nulls[2] = {false, false};
  Datum *elems;
  ArrayType *arr;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  relid = PG_GETARG_OID(0);

  if(!provsql_lookup_table_info(relid, &info))
    PG_RETURN_NULL();

  if(get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
    provsql_error("get_table_info: expected composite return type");
  tupdesc = BlessTupleDesc(tupdesc);

  values[0] = CStringGetTextDatum(table_kind_label(info.kind));

  elems = palloc(info.block_key_n * sizeof(Datum));
  for(uint16 i = 0; i < info.block_key_n; ++i)
    elems[i] = Int16GetDatum(info.block_key[i]);
  arr = construct_array(elems, info.block_key_n, INT2OID, 2, true, 's');
  pfree(elems);
  values[1] = PointerGetDatum(arr);

  PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(get_infos);
/** @brief PostgreSQL-callable wrapper for get_infos(). */
Datum get_infos(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  unsigned info1 =0, info2 = 0;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  STARTWRITEM();
  ADDWRITEM("i", char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(info1, int) || !READB(info2, int)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type i)");
  }

  provsql_shmem_unlock();

  {
    TupleDesc tupdesc;
    Datum values[2];
    bool nulls[2] = {false, false};

    get_call_result_type(fcinfo,NULL,&tupdesc);
    tupdesc = BlessTupleDesc(tupdesc);

    values[0] = Int32GetDatum(info1);
    values[1] = Int32GetDatum(info2);

    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  }
}
