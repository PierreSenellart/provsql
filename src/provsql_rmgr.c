/**
 * @file provsql_rmgr.c
 * @brief WAL-logging the circuit store through a custom resource manager.
 *
 * The circuit store is not a set of relations, so PostgreSQL's own
 * machinery does not carry it: it is invisible to crash recovery,
 * streaming replication and PITR.  From PostgreSQL 15 an extension can
 * register a resource manager of its own and write WAL records the
 * startup process will replay, which is the one mechanism that brings a
 * non-relation file under WAL without putting it in shared buffers.
 *
 * The design is the one the store's shape makes natural.  Every mutation
 * is already a self-describing message -- opcode, database, payload --
 * so a record is just that message, and replay is feeding it back to the
 * worker.  Records are not tied to the commit: like an index page split
 * they are applied whether or not the transaction commits, which is
 * exactly the semantics gate creation already has, and replay is
 * idempotent because creating a gate that exists is a no-op and writing
 * a probability a gate already holds is a no-op too.
 *
 * **What it is for.**  Streaming replication and PITR: a standby's
 * startup process applies these records to its own store, so a replica
 * can carry provenance.  Crash recovery does *not* depend on them, and
 * that is deliberate: @c provsql.wal_logging requires
 * @c provsql.synchronous_commit, so a transaction cannot commit until
 * its store writes are on disk, and the store on disk is therefore never
 * behind the WAL.  That requirement is what removes the checkpoint gap
 * an asynchronous store would open -- WAL before the last checkpoint's
 * redo pointer is recycled, so a store that lagged across a whole
 * checkpoint could not be repaired by replay.
 *
 * **What it is not.**  A hot-standby backend must not write to the store,
 * so provenance queries on a standby only work for gates that already
 * exist there -- and almost every provenance query creates gates, reads
 * included.  Making them work needs a session-local overlay store, which
 * is a different piece of work.
 *
 * Off by default: it changes what a cluster writes to its WAL, and a
 * replica that has never seen these records is better off without them.
 */
#include "postgres.h"

#include "provsql_config.h"

#if PG_VERSION_NUM >= 150000 && !defined(PROVSQL_INPROCESS_STORE)

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "miscadmin.h"
#include "utils/builtins.h"

#include "provsql_mmap.h"
#include "provsql_rmgr.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

bool provsql_wal_logging = false;

/** True while this process is replaying a ProvSQL WAL record, which is
 *  the one case where writing to the store during recovery is right. */
static bool in_redo = false;

/**
 * @brief Resource-manager id.
 *
 * PostgreSQL reserves 128-255 for extensions, and one cluster cannot
 * load two extensions claiming the same id: replay would hand one
 * extension's records to the other.  Which ids are taken is kept at
 * https://wiki.postgresql.org/wiki/CustomWALResourceManagers, where 151
 * is reserved for ProvSQL; keep this in step with that page.
 *
 * @c RM_EXPERIMENTAL_ID (128) is what the page asks unreleased work to
 * use, and is where this started; a released extension takes an id of
 * its own.
 */
#define RM_PROVSQL_ID 151

/** @brief The only record kind: a store message to replay verbatim.
 *
 *  The low four bits of the info byte belong to PostgreSQL, so the tag
 *  lives in the high nibble; the message's own opcode is the first byte
 *  of the payload, which is all replay needs. */
#define XLOG_PROVSQL_STORE 0x10

static void provsql_rmgr_redo(XLogReaderState *record)
{
  uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

  if(info != XLOG_PROVSQL_STORE)
    provsql_error("provsql resource manager: unexpected record info %u", info);

  in_redo = true;
  PG_TRY();
  {
    provsql_replay_store_message(XLogRecGetData(record),
                                 XLogRecGetDataLen(record));
  }
  PG_CATCH();
  {
    in_redo = false;
    PG_RE_THROW();
  }
  PG_END_TRY();
  in_redo = false;
}

static void provsql_rmgr_desc(StringInfo buf, XLogReaderState *record)
{
  const char *data = XLogRecGetData(record);
  uint32 len = XLogRecGetDataLen(record);

  if(len > 0)
    appendStringInfo(buf, "opcode %c, %u bytes", data[0], len);
  else
    appendStringInfoString(buf, "empty");
}

static const char *provsql_rmgr_identify(uint8 info)
{
  if((info & ~XLR_INFO_MASK) == XLOG_PROVSQL_STORE)
    return "STORE";
  return NULL;
}

static const RmgrData provsql_rmgr = {
  .rm_name     = "provsql",
  .rm_redo     = provsql_rmgr_redo,
  .rm_desc     = provsql_rmgr_desc,
  .rm_identify = provsql_rmgr_identify,
  .rm_startup  = NULL,
  .rm_cleanup  = NULL,
  .rm_mask     = NULL,
  .rm_decode   = NULL,
};

void provsql_register_rmgr(void)
{
  RegisterCustomRmgr(RM_PROVSQL_ID, &provsql_rmgr);
}

bool provsql_store_write_allowed(void)
{
  /* The refusal is tied to WAL logging, not to recovery alone.  With
     logging on, the standby's store is what replay makes it, and a
     backend writing to it would make the two diverge for good.  With
     logging off, nothing maintains the standby's store, so its backends
     have always written to it themselves -- badly (it diverges from the
     primary and the two cannot be reconciled after a promotion), but
     refusing outright would take provenance away from standbys that
     rely on it today.  The documentation says which of the two a
     deployment is choosing. */
  if(in_redo || !provsql_wal_logging)
    return true;
  return !RecoveryInProgress();
}

void provsql_wal_log_store_message(const char *data, size_t len)
{
  if(!provsql_wal_logging || in_redo || RecoveryInProgress())
    return;

  if(!provsql_synchronous_commit)
    ereport(ERROR,
            (errmsg("provsql.wal_logging requires provsql.synchronous_commit"),
             errdetail("WAL records for the circuit store are only complete "
                       "if the store on disk is never behind the WAL, which "
                       "is what the at-commit sync barrier guarantees."),
             errhint("SET provsql.synchronous_commit = on.")));

  XLogBeginInsert();
  XLogRegisterData((char *) data, (uint32) len);
  XLogInsert(RM_PROVSQL_ID, XLOG_PROVSQL_STORE);
}

#else /* PostgreSQL < 15, or the single-process build */

#include "provsql_rmgr.h"

bool provsql_wal_logging = false;

void provsql_register_rmgr(void) {}

void provsql_wal_log_store_message(const char *data, size_t len)
{
  (void) data;
  (void) len;
}

bool provsql_store_write_allowed(void)
{
  return true;
}

#endif
