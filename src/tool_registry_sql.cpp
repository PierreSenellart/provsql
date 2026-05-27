/**
 * @file tool_registry_sql.cpp
 * @brief SQL surface for the external-tool registry (@ref ToolRegistry.h).
 *
 * Exposes the in-memory catalog to SQL:
 *
 * - @c tool_registry_list(): set-returning, backs the read-only
 *   @c provsql.tools view; reports each record plus an @c available flag
 *   computed with the same @c find_external_tool the dispatchers use.
 * - @c tool_registry_register() / @c tool_registry_unregister() /
 *   @c tool_registry_set_enabled() / @c tool_registry_set_preference():
 *   mutators, **superuser-only**.
 *
 * @par Security
 * A CLI tool record names an executable that ProvSQL runs as the PostgreSQL
 * OS user, so editing a record is equivalent to OS-level trust on the
 * server account (the same trust as setting @c provsql.tool_search_path or
 * dropping a binary on it).  The mutators therefore refuse non-superusers;
 * the read-only listing is unrestricted, like @c tool_available.
 *
 * @par Lifetime
 * @par Persistence
 * The compiled-in defaults live in C (@ref ToolRegistry.h); admin changes are
 * persisted in the @c provsql.tool_overrides table and overlaid on the seed
 * by @ref provsql_sync_tool_registry, which every registry-consuming SQL
 * function calls so changes are seen across sessions and backends.  An empty
 * overrides table is exactly the compiled defaults; the table being absent
 * (an extension older than 1.8.0) is treated the same way.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "executor/spi.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

PG_FUNCTION_INFO_V1(tool_registry_list);
PG_FUNCTION_INFO_V1(tool_registry_register);
PG_FUNCTION_INFO_V1(tool_registry_unregister);
PG_FUNCTION_INFO_V1(tool_registry_set_enabled);
PG_FUNCTION_INFO_V1(tool_registry_set_preference);
}

#include "ToolRegistry.h"
#include "tool_registry_sync.h"
#include "external_tool.h"
#include "provsql_error.h"

#include <functional>
#include <string>
#include <vector>

namespace {

/// Read a SQL text into a std::string.
std::string text_to_string(text *t)
{
  return std::string(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
}

/// Build a text[] Datum from a vector of strings (never NULL elements).
Datum string_vector_to_text_array(const std::vector<std::string> &v)
{
  if (v.empty())
    return PointerGetDatum(construct_empty_array(TEXTOID));

  std::vector<Datum> elems;
  elems.reserve(v.size());
  for (const auto &s : v)
    elems.push_back(PointerGetDatum(cstring_to_text_with_len(s.data(),
                                                             s.size())));

  ArrayType *arr = construct_array(elems.data(),
                                   static_cast<int>(elems.size()),
                                   TEXTOID, -1, false, TYPALIGN_INT);
  return PointerGetDatum(arr);
}

/// Decode a (non-NULL) text[] argument into a vector of strings, dropping
/// NULL elements.
std::vector<std::string> text_array_to_string_vector(ArrayType *arr)
{
  std::vector<std::string> out;
  Datum *elems;
  bool *nulls;
  int n;
  deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT,
                    &elems, &nulls, &n);
  for (int i = 0; i < n; ++i) {
    if (nulls[i])
      continue;
    out.push_back(text_to_string(DatumGetTextPP(elems[i])));
  }
  return out;
}

/// Reject non-superusers from a registry mutator.
void require_superuser(const char *fn)
{
  if (!superuser())
    provsql_error("%s: must be superuser (a tool record can run arbitrary "
                  "commands as the PostgreSQL OS user)", fn);
}

/// A tool is available iff its binary (when set) and every dependency
/// resolve on the backend's PATH.
bool tool_available(const provsql::ToolRecord &rec)
{
  if (!rec.binary.empty() && find_external_tool(rec.binary).empty())
    return false;
  for (const std::string &dep : rec.dependencies)
    if (find_external_tool(dep).empty())
      return false;
  return true;
}

// ---- provsql.tool_overrides persistence (caller manages SPI_connect) ----

/// Read a text[] column of the current SPI tuple as a vector<string>.
std::vector<std::string> spi_text_array(HeapTuple t, TupleDesc td, int col)
{
  bool isnull;
  Datum d = SPI_getbinval(t, td, col, &isnull);
  if (isnull)
    return {};
  return text_array_to_string_vector(DatumGetArrayTypeP(d));
}

/// Read a text column of the current SPI tuple ("" when NULL).
std::string spi_text(HeapTuple t, TupleDesc td, int col)
{
  char *s = SPI_getvalue(t, td, col);
  return s ? std::string(s) : std::string();
}

/// True iff provsql.tool_overrides exists (an extension < 1.8.0 lacks it).
/// to_regclass returns NULL rather than erroring on a missing relation.
bool overrides_table_exists()
{
  if (SPI_execute("SELECT to_regclass('provsql.tool_overrides') IS NOT NULL",
                  true, 1) != SPI_OK_SELECT || SPI_processed != 1)
    return false;
  bool isnull;
  Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                          1, &isnull);
  return !isnull && DatumGetBool(d);
}

/// Upsert a complete record (removed=false) into provsql.tool_overrides.
void upsert_override(const provsql::ToolRecord &rec)
{
  Oid types[12] = {TEXTOID, TEXTOID, TEXTOID, TEXTARRAYOID, TEXTARRAYOID,
                   TEXTOID, TEXTOID, INT4OID, BOOLOID, TEXTARRAYOID,
                   TEXTOID, TEXTOID};
  Datum vals[12] = {
    CStringGetTextDatum(rec.name.c_str()),
    CStringGetTextDatum(rec.kind.c_str()),
    CStringGetTextDatum(rec.binary.c_str()),
    string_vector_to_text_array(rec.operations),
    string_vector_to_text_array(rec.input_formats),
    CStringGetTextDatum(rec.output_format.c_str()),
    CStringGetTextDatum(rec.parser.c_str()),
    Int32GetDatum(rec.preference),
    BoolGetDatum(rec.enabled),
    string_vector_to_text_array(rec.dependencies),
    CStringGetTextDatum(rec.argtpl.c_str()),
    CStringGetTextDatum(rec.argtpl_circuit.c_str()),
  };
  SPI_execute_with_args(
    "INSERT INTO provsql.tool_overrides "
    "(name, removed, kind, executable, operations, input_formats, "
    " output_format, parser, preference, enabled, dependencies, argtpl, "
    " argtpl_circuit) "
    "VALUES ($1, false, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12) "
    "ON CONFLICT (name) DO UPDATE SET "
    "  removed=false, kind=$2, executable=$3, operations=$4, "
    "  input_formats=$5, output_format=$6, parser=$7, preference=$8, "
    "  enabled=$9, dependencies=$10, argtpl=$11, argtpl_circuit=$12",
    12, types, vals, NULL, false, 0);
}

/// Tombstone a name (removed=true) so the seeded default, if any, is hidden.
void tombstone_override(const std::string &name)
{
  Oid types[1] = {TEXTOID};
  Datum vals[1] = {CStringGetTextDatum(name.c_str())};
  SPI_execute_with_args(
    "INSERT INTO provsql.tool_overrides (name, removed) VALUES ($1, true) "
    "ON CONFLICT (name) DO UPDATE SET removed=true, kind=NULL, "
    "  executable=NULL, operations=NULL, input_formats=NULL, "
    "  output_format=NULL, parser=NULL, preference=NULL, enabled=NULL, "
    "  dependencies=NULL, argtpl=NULL, argtpl_circuit=NULL",
    1, types, vals, NULL, false, 0);
}

} // namespace

/**
 * @brief Rebuild the in-memory registry as "compiled seed overlaid with the
 *        provsql.tool_overrides rows".  See @ref tool_registry_sync.h.
 */
void provsql_sync_tool_registry()
{
  provsql::ToolRegistry &reg = provsql::tool_registry();
  reg.reset();  // back to the compiled-in defaults

  if (SPI_connect() != SPI_OK_CONNECT)
    return;  // cannot read; the seed stands
  if (overrides_table_exists()) {
    if (SPI_execute(
          "SELECT name, removed, kind, executable, operations, input_formats, "
          " output_format, parser, preference, enabled, dependencies, argtpl, "
          " argtpl_circuit FROM provsql.tool_overrides", true, 0)
        == SPI_OK_SELECT) {
      TupleDesc td = SPI_tuptable->tupdesc;
      for (uint64 i = 0; i < SPI_processed; ++i) {
        HeapTuple t = SPI_tuptable->vals[i];
        std::string name = spi_text(t, td, 1);
        bool isnull;
        Datum rd = SPI_getbinval(t, td, 2, &isnull);
        if (!isnull && DatumGetBool(rd)) {  // tombstone
          reg.remove(name);
          continue;
        }
        provsql::ToolRecord rec;
        rec.name          = name;
        rec.kind          = spi_text(t, td, 3);
        rec.binary        = spi_text(t, td, 4);
        rec.operations    = spi_text_array(t, td, 5);
        rec.input_formats = spi_text_array(t, td, 6);
        rec.output_format = spi_text(t, td, 7);
        rec.parser        = spi_text(t, td, 8);
        Datum pd = SPI_getbinval(t, td, 9, &isnull);
        rec.preference = isnull ? 0 : DatumGetInt32(pd);
        Datum ed = SPI_getbinval(t, td, 10, &isnull);
        rec.enabled = isnull ? true : DatumGetBool(ed);
        rec.dependencies   = spi_text_array(t, td, 11);
        rec.argtpl         = spi_text(t, td, 12);
        rec.argtpl_circuit = spi_text(t, td, 13);
        reg.upsert(rec);
      }
    }
  }
  SPI_finish();
}

/**
 * @brief Set-returning listing of the registry, one row per record.
 *
 * Columns: name, kind, binary, operations (text[]), input_formats (text[]),
 * output_format (text), parser (text), preference (int), enabled (bool),
 * argtpl (text), argtpl_circuit (text), available (bool).  @c operations /
 * @c input_formats /
 * @c output_format use the KCMCP registry names; @c parser is the CLI-only
 * decode tag.  @c available is true iff @c binary (when set) and every
 * dependency resolve via @c find_external_tool, so the view reflects what a
 * subsequent dispatch would actually find on the backend's PATH.
 */
extern "C" Datum
tool_registry_list(PG_FUNCTION_ARGS)
{
  // Reflect any persisted overrides (from this or another backend).
  provsql_sync_tool_registry();

  ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

  MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
  MemoryContext oldcontext = MemoryContextSwitchTo(per_query_ctx);

  TupleDesc tupdesc;
  if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("tool_registry_list: function must return a row type");
  }
  tupdesc = BlessTupleDesc(tupdesc);

  Tuplestorestate *tupstore = tuplestore_begin_heap(
    rsinfo->allowedModes & SFRM_Materialize_Random, false, work_mem);
  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;
  rsinfo->setDesc = tupdesc;

  try {
    for (const provsql::ToolRecord &rec : provsql::tool_registry().records()) {
      Datum values[12];
      bool nulls[12] = {false, false, false, false, false, false, false,
                        false, false, false, false, false};

      values[0] = PointerGetDatum(cstring_to_text_with_len(rec.name.data(),
                                                           rec.name.size()));
      values[1] = PointerGetDatum(cstring_to_text_with_len(rec.kind.data(),
                                                           rec.kind.size()));
      values[2] = PointerGetDatum(cstring_to_text_with_len(rec.binary.data(),
                                                           rec.binary.size()));
      values[3] = string_vector_to_text_array(rec.operations);
      values[4] = string_vector_to_text_array(rec.input_formats);
      values[5] = PointerGetDatum(cstring_to_text_with_len(
                    rec.output_format.data(), rec.output_format.size()));
      values[6] = PointerGetDatum(cstring_to_text_with_len(rec.parser.data(),
                                                           rec.parser.size()));
      values[7] = Int32GetDatum(rec.preference);
      values[8] = BoolGetDatum(rec.enabled);
      values[9] = PointerGetDatum(cstring_to_text_with_len(rec.argtpl.data(),
                                                           rec.argtpl.size()));
      values[10] = PointerGetDatum(cstring_to_text_with_len(
                     rec.argtpl_circuit.data(), rec.argtpl_circuit.size()));
      values[11] = BoolGetDatum(tool_available(rec));

      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  } catch (const std::exception &e) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("tool_registry_list: %s", e.what());
  } catch (...) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("tool_registry_list: unknown exception");
  }

  MemoryContextSwitchTo(oldcontext);
  PG_RETURN_NULL();
}

/**
 * @brief Register a tool, or replace the record with the same name.
 *
 * Args (in order): name text, executable text, kind text, operations text[],
 * input_formats text[], output_format text, parser text, argtpl text,
 * argtpl_circuit text, preference int, enabled bool.  A NULL @c executable
 * defaults to @c name; a NULL @c kind defaults to @c 'cli'; NULL arrays are
 * empty; NULL text fields default to empty; NULL @c preference is 0 and NULL
 * @c enabled is true.  Superuser-only.
 */
extern "C" Datum
tool_registry_register(PG_FUNCTION_ARGS)
{
  require_superuser("register_tool");

  if (PG_ARGISNULL(0))
    provsql_error("register_tool: name must not be NULL");

  try {
    provsql::ToolRecord rec;
    rec.name = text_to_string(PG_GETARG_TEXT_PP(0));
    rec.binary = PG_ARGISNULL(1) ? rec.name
                                 : text_to_string(PG_GETARG_TEXT_PP(1));
    rec.kind = PG_ARGISNULL(2) ? std::string("cli")
                               : text_to_string(PG_GETARG_TEXT_PP(2));
    if (!PG_ARGISNULL(3))
      rec.operations = text_array_to_string_vector(PG_GETARG_ARRAYTYPE_P(3));
    if (!PG_ARGISNULL(4))
      rec.input_formats = text_array_to_string_vector(PG_GETARG_ARRAYTYPE_P(4));
    if (!PG_ARGISNULL(5))
      rec.output_format = text_to_string(PG_GETARG_TEXT_PP(5));
    if (!PG_ARGISNULL(6))
      rec.parser = text_to_string(PG_GETARG_TEXT_PP(6));
    if (!PG_ARGISNULL(7))
      rec.argtpl = text_to_string(PG_GETARG_TEXT_PP(7));
    if (!PG_ARGISNULL(8))
      rec.argtpl_circuit = text_to_string(PG_GETARG_TEXT_PP(8));
    rec.preference = PG_ARGISNULL(9) ? 0 : PG_GETARG_INT32(9);
    rec.enabled = PG_ARGISNULL(10) ? true : PG_GETARG_BOOL(10);

    if (rec.name.empty())
      provsql_error("register_tool: name must not be empty");

    // Persist the full record (create or replace) in the overrides table.
    if (SPI_connect() != SPI_OK_CONNECT)
      provsql_error("register_tool: SPI_connect failed");
    upsert_override(rec);
    SPI_finish();
  } catch (const std::exception &e) {
    provsql_error("register_tool: %s", e.what());
  }

  PG_RETURN_VOID();
}

/**
 * @brief Remove a tool record.  Errors if no tool of that name is currently
 * effective, so a typo fails loudly rather than silently doing nothing.
 * A removed seeded default is recorded as a tombstone; the change persists.
 */
extern "C" Datum
tool_registry_unregister(PG_FUNCTION_ARGS)
{
  require_superuser("unregister_tool");
  std::string name = text_to_string(PG_GETARG_TEXT_PP(0));

  provsql_sync_tool_registry();
  if (provsql::tool_registry().find(name) == nullptr)
    provsql_error("unregister_tool: no tool named '%s' is registered",
                  name.c_str());

  if (SPI_connect() != SPI_OK_CONNECT)
    provsql_error("unregister_tool: SPI_connect failed");
  tombstone_override(name);
  SPI_finish();
  PG_RETURN_VOID();
}

/// Persist a single-field change to an existing tool: load the effective
/// record, apply @p mutate, and write the full record back.  Errors on an
/// unknown tool name.
static void persist_tool_change(const char *fn, const std::string &name,
                                const std::function<void(provsql::ToolRecord&)> &mutate)
{
  provsql_sync_tool_registry();
  const provsql::ToolRecord *cur = provsql::tool_registry().find(name);
  if (cur == nullptr)
    provsql_error("%s: no tool named '%s' is registered", fn, name.c_str());
  provsql::ToolRecord rec = *cur;
  mutate(rec);
  if (SPI_connect() != SPI_OK_CONNECT)
    provsql_error("%s: SPI_connect failed", fn);
  upsert_override(rec);
  SPI_finish();
}

/** @brief Enable or disable a tool.  Errors on an unknown tool name. */
extern "C" Datum
tool_registry_set_enabled(PG_FUNCTION_ARGS)
{
  require_superuser("set_tool_enabled");
  std::string name = text_to_string(PG_GETARG_TEXT_PP(0));
  bool enabled = PG_GETARG_BOOL(1);
  persist_tool_change("set_tool_enabled", name,
                      [enabled](provsql::ToolRecord &r) { r.enabled = enabled; });
  PG_RETURN_VOID();
}

/** @brief Set a tool's preference.  Errors on an unknown tool name. */
extern "C" Datum
tool_registry_set_preference(PG_FUNCTION_ARGS)
{
  require_superuser("set_tool_preference");
  std::string name = text_to_string(PG_GETARG_TEXT_PP(0));
  int preference = PG_GETARG_INT32(1);
  persist_tool_change("set_tool_preference", name,
                      [preference](provsql::ToolRecord &r) { r.preference = preference; });
  PG_RETURN_VOID();
}
