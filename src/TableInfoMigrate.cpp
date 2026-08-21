/**
 * @file TableInfoMigrate.cpp
 * @brief One-shot import of the legacy per-relation metadata file.
 *
 * Before ProvSQL 1.13.0, the per-relation TID / BID / OPAQUE metadata
 * lived in a fifth mmap file, @c provsql_table_info.mmap, alongside the
 * circuit store; it now lives in the @c provsql.table_info heap table
 * (see @c table_info.c).  @c provsql.migrate_table_info() reads the old
 * file, when the database still has one, and inserts every record it
 * holds that the table does not already carry.
 *
 * The file is read directly by the calling backend, read-only: nothing
 * writes it any more, so there is no worker round-trip and no locking
 * to arrange.  Records for relations that no longer exist are skipped
 * (the old file had no way to drop an entry for a relation dropped
 * while the extension was absent).
 */
#include <string>

#include <unistd.h>

extern "C" {
#include "postgres.h"
#include "catalog/pg_type.h"
#include "common/relpath.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "provsql_utils.h"
}

#include "MMappedTableInfo.h"
#include "MMappedVector.hpp"

/** @brief Magic of the legacy @c provsql_table_info.mmap header
 *  ("PvSTblIn"), as written by ProvSQL 1.6.0 to 1.12.0. */
static constexpr uint64_t LEGACY_TABLE_INFO_MAGIC =
  uint64_t('P')       | uint64_t('v') <<  8 | uint64_t('S') << 16 | uint64_t('T') << 24 |
  uint64_t('b') << 32 | uint64_t('l') << 40 | uint64_t('I') << 48 | uint64_t('n') << 56;

/** @brief Textual label of a persisted @c provsql_table_kind value. */
static const char *kindLabel(uint8_t kind)
{
  switch(kind) {
  case PROVSQL_TABLE_TID: return "tid";
  case PROVSQL_TABLE_BID: return "bid";
  default:                return "opaque";
  }
}

extern "C" {

PG_FUNCTION_INFO_V1(migrate_table_info);

/**
 * @brief Import the legacy metadata file into @c provsql.table_info.
 * @return The number of rows inserted; 0 when there is no file to read
 *         or every record it holds is already present.
 */
Datum migrate_table_info(PG_FUNCTION_ARGS)
{
  char *rel = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);
  std::string path = std::string(DataDir) + "/" + rel
                     + "/provsql_table_info.mmap";
  pfree(rel);

  if(access(path.c_str(), R_OK) != 0)
    PG_RETURN_INT64(0);

  int64 inserted = 0;

  if(SPI_connect() != SPI_OK_CONNECT)
    provsql_error("Cannot connect to SPI while migrating provsql.table_info");

  try {
    MMappedVector<ProvenanceTableInfo> legacy(path.c_str(), true,
                                              LEGACY_TABLE_INFO_MAGIC);

    for(unsigned long i = 0; i < legacy.nbElements(); ++i) {
      const ProvenanceTableInfo &info = legacy[i];
      Oid argtypes[4] = { OIDOID, TEXTOID, INT2ARRAYOID, OIDARRAYOID };
      Datum values[4];
      Datum *elems;
      int rc;

      /* Tombstoned entries, and entries whose relation is long gone. */
      if(info.relid == InvalidOid)
        continue;
      if(!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(info.relid)))
        continue;

      values[0] = ObjectIdGetDatum(info.relid);
      values[1] = CStringGetTextDatum(kindLabel(info.kind));

      elems = (Datum *) palloc(sizeof(Datum) *
                               (info.block_key_n ? info.block_key_n : 1));
      for(uint16_t k = 0; k < info.block_key_n; ++k)
        elems[k] = Int16GetDatum(info.block_key[k]);
      values[2] = PointerGetDatum(
        construct_array(elems, info.block_key_n, INT2OID, 2, true, 's'));
      pfree(elems);

      elems = (Datum *) palloc(sizeof(Datum) *
                               (info.ancestor_n ? info.ancestor_n : 1));
      for(uint16_t k = 0; k < info.ancestor_n; ++k)
        elems[k] = ObjectIdGetDatum(info.ancestors[k]);
      values[3] = PointerGetDatum(
        construct_array(elems, info.ancestor_n, OIDOID, sizeof(Oid), true, 'i'));
      pfree(elems);

      rc = SPI_execute_with_args(
        "INSERT INTO provsql.table_info(relid, kind, block_key, ancestors) "
        "VALUES ($1::regclass, $2, $3, $4) ON CONFLICT (relid) DO NOTHING",
        4, argtypes, values, NULL, false, 0);
      if(rc != SPI_OK_INSERT)
        provsql_error("Cannot insert into provsql.table_info (SPI code %d)", rc);
      inserted += SPI_processed;
    }
  } catch(const std::exception &e) {
    SPI_finish();
    provsql_error("migrate_table_info: cannot read %s: %s",
                  path.c_str(), e.what());
  }

  SPI_finish();
  PG_RETURN_INT64(inserted);
}

}
