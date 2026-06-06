/**
 * @file reachability_evaluate.cpp
 * @brief SQL entry points for decomposition-aligned reachability
 *        compilation over bounded-treewidth data.
 *
 * Exposes @c ReachabilityCompiler (see @c ReachabilityCompiler.h) to SQL:
 *
 * - @c reachability_evaluate(): exact probability that the target vertex
 *   is reachable from the source vertex (two-terminal network
 *   reliability), in time linear in the number of edges for data of
 *   bounded treewidth;
 * - @c reachability_compile_stats(): same compilation, returning the
 *   probability together with structural statistics (data treewidth,
 *   number of bags, maximum DP state count, d-DNNF size) that
 *   substantiate the linear-size claim in tests and benchmarks.
 *
 * Both take the edge relation in columnar form (parallel arrays of
 * source vertices, target vertices, provenance tokens, and
 * probabilities); the user-facing wrappers in @c provsql.sql gather
 * those arrays from an arbitrary provenance-tracked edge relation.
 * Vertices are dense integer IDs (the wrappers map arbitrary vertex
 * values onto them).
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/uuid.h"

#include "provsql_utils.h"
#include "provsql_mmap.h"
#include "provsql_shmem.h"

PG_FUNCTION_INFO_V1(reachability_evaluate);
PG_FUNCTION_INFO_V1(reachability_compile_stats);
PG_FUNCTION_INFO_V1(reachability_materialize);
}

#include "c_cpp_compatibility.h"
#include "ReachabilityCompiler.h"
#include "provsql_utils_cpp.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

/**
 * @brief Validate a 1-D, NULL-free array argument and return its length.
 * @param arr   The array (may be null for "no edges").
 * @param what  Argument name for error messages.
 * @return      Number of elements (0 when @p arr is null).
 */
int checkedArrayLength(ArrayType *arr, const char *what)
{
  if (arr == NULL)
    return 0;
  if (ARR_NDIM(arr) > 1)
    provsql_error("reachability: %s must be a one-dimensional array", what);
  if (ARR_HASNULL(arr))
    provsql_error("reachability: %s must not contain NULLs", what);
  return ARR_NDIM(arr) == 0 ? 0 : ARR_DIMS(arr)[0];
}

/**
 * @brief Decode the four columnar edge arrays (arguments 0..3 of every
 *        entry point) into edge rows.
 *
 * @param fcinfo  PostgreSQL function-call info.
 * @return        The decoded edge rows.
 */
std::vector<ReachabilityCompiler::EdgeRow> edgesFromArgs(FunctionCallInfo fcinfo)
{
  ArrayType *srcs   = PG_ARGISNULL(0) ? NULL : PG_GETARG_ARRAYTYPE_P(0);
  ArrayType *dsts   = PG_ARGISNULL(1) ? NULL : PG_GETARG_ARRAYTYPE_P(1);
  ArrayType *tokens = PG_ARGISNULL(2) ? NULL : PG_GETARG_ARRAYTYPE_P(2);
  ArrayType *probs  = PG_ARGISNULL(3) ? NULL : PG_GETARG_ARRAYTYPE_P(3);

  const int n = checkedArrayLength(srcs, "sources");
  if (checkedArrayLength(dsts, "destinations") != n ||
      checkedArrayLength(tokens, "tokens") != n ||
      checkedArrayLength(probs, "probabilities") != n)
    provsql_error("reachability: edge arrays must have the same length");

  std::vector<ReachabilityCompiler::EdgeRow> rows;
  rows.reserve(n);

  if (n > 0) {
    /* All four element types are fixed-length and NULL-free (checked
     * above), so the data areas are packed and can be read directly,
     * as create_gate does for uuid[]. */
    const int32 *src_data = (const int32 *) ARR_DATA_PTR(srcs);
    const int32 *dst_data = (const int32 *) ARR_DATA_PTR(dsts);
    const pg_uuid_t *token_data = (const pg_uuid_t *) ARR_DATA_PTR(tokens);
    const float8 *prob_data = (const float8 *) ARR_DATA_PTR(probs);

    for (int i = 0; i < n; ++i) {
      ReachabilityCompiler::EdgeRow row;
      row.src = static_cast<unsigned long>(src_data[i]);
      row.dst = static_cast<unsigned long>(dst_data[i]);
      row.token = uuid2string(token_data[i]);
      row.prob = prob_data[i];
      if (row.prob < 0. || row.prob > 1.)
        provsql_error("reachability: edge probability %f out of [0,1]",
                      row.prob);
      rows.push_back(std::move(row));
    }
  }

  return rows;
}

/**
 * @brief Decode the single-target argument layout and run the compilation.
 *
 * Argument layout: @c srcs @c int[], @c dsts @c int[], @c tokens
 * @c uuid[], @c probs @c float8[], @c source @c int, @c target @c int,
 * @c directed @c boolean.
 *
 * @param fcinfo  PostgreSQL function-call info.
 * @return        The compiled d-DNNF and statistics.
 */
ReachabilityCompiler::Result compileFromArgs(FunctionCallInfo fcinfo)
{
  for (int i = 4; i < 7; ++i)
    if (PG_ARGISNULL(i))
      provsql_error("reachability: source, target and directed must not be NULL");

  auto rows = edgesFromArgs(fcinfo);
  const unsigned long source = static_cast<unsigned long>(PG_GETARG_INT32(4));
  const unsigned long target = static_cast<unsigned long>(PG_GETARG_INT32(5));
  const bool directed = PG_GETARG_BOOL(6);

  try {
    return ReachabilityCompiler::compile(rows, source, target, directed);
  } catch (TreeDecompositionException &) {
    provsql_error(
      "reachability: data treewidth exceeds the supported limit (%d)",
      TreeDecomposition::MAX_TREEWIDTH);
    throw; /* unreachable; placate the compiler */
  }
}

// ---------------------------------------------------------------------
// Content-addressed materialisation of a certified d-DNNF into the mmap
// store.
// ---------------------------------------------------------------------

/**
 * @brief Minimal SHA-1 (RFC 3174), used for RFC 4122 version-5 UUIDs.
 *
 * Self-contained so the content addressing builds on every supported
 * PostgreSQL version (the common/cryptohash SHA-1 API only appeared in
 * PostgreSQL 14).
 *
 * @param data  Input bytes.
 * @param len   Input length.
 * @param out   Output: 20-byte digest.
 */
void sha1(const unsigned char *data, std::size_t len, unsigned char out[20])
{
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                   0xC3D2E1F0u};
  const std::size_t total = ((len + 8) / 64 + 1) * 64;
  std::vector<unsigned char> buf(total, 0);
  std::copy(data, data + len, buf.begin());
  buf[len] = 0x80;
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  for (int i = 0; i < 8; ++i)
    buf[total - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));

  for (std::size_t chunk = 0; chunk < total; chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<uint32_t>(buf[chunk + 4*i]) << 24) |
             (static_cast<uint32_t>(buf[chunk + 4*i + 1]) << 16) |
             (static_cast<uint32_t>(buf[chunk + 4*i + 2]) << 8) |
             static_cast<uint32_t>(buf[chunk + 4*i + 3]);
    for (int i = 16; i < 80; ++i) {
      const uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
      w[i] = (v << 1) | (v >> 31);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
      e = d;
      d = c;
      c = (b << 30) | (b >> 2);
      b = a;
      a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  for (int i = 0; i < 5; ++i) {
    out[4*i]     = static_cast<unsigned char>(h[i] >> 24);
    out[4*i + 1] = static_cast<unsigned char>(h[i] >> 16);
    out[4*i + 2] = static_cast<unsigned char>(h[i] >> 8);
    out[4*i + 3] = static_cast<unsigned char>(h[i]);
  }
}

/**
 * @brief RFC 4122 version-5 UUID in the ProvSQL namespace.
 *
 * Same value as the SQL @c uuid_generate_v5(uuid_ns_provsql(), name),
 * so gates materialised here content-address identically to those the
 * query rewriter mints.
 *
 * @param name  Name within the namespace.
 * @return      The version-5 UUID.
 */
pg_uuid_t uuidV5(const std::string &name)
{
  // uuid_ns_provsql() = 920d4f02-8718-5319-9532-d4ab83a64489
  static const unsigned char ns[16] = {
    0x92, 0x0d, 0x4f, 0x02, 0x87, 0x18, 0x53, 0x19,
    0x95, 0x32, 0xd4, 0xab, 0x83, 0xa6, 0x44, 0x89
  };
  std::vector<unsigned char> data(16 + name.size());
  std::copy(ns, ns + 16, data.begin());
  std::copy(name.begin(), name.end(), data.begin() + 16);
  unsigned char digest[20];
  sha1(data.data(), data.size(), digest);
  pg_uuid_t u;
  std::copy(digest, digest + 16, u.data);
  u.data[6] = static_cast<unsigned char>((u.data[6] & 0x0F) | 0x50);
  u.data[8] = static_cast<unsigned char>((u.data[8] & 0x3F) | 0x80);
  return u;
}

/**
 * @brief Materialise (the reachable part of) a certified d-DNNF into the
 *        mmap store.
 *
 * Bottom-up over the gates reachable from @p roots: input gates keep
 * their existing tokens (they are the edge tuples' provenance and are
 * not touched), @c NOT @c x is stored as @c monus(one, x), AND as
 * @c times, OR as @c plus -- no new gate types.  Gate UUIDs are
 * content-addressed with the standard v5 recipes (children sorted for
 * the commutative gates), so identical sub-circuits dedup in the store
 * and re-materialising the same circuit is a no-op
 * (@c MMappedCircuit::createGate is idempotent).  Gates carrying the
 * d-DNNF certificate get it persisted via @c set_infos.
 *
 * @param dd     The certified circuit.
 * @param roots  Gates whose closure to materialise.
 * @return       Map from @p dd gates to their store UUIDs.
 */
std::unordered_map<gate_t, pg_uuid_t, hash_gate_t> materializeCertifiedDD(
  const dDNNF &dd, const std::vector<gate_t> &roots)
{
  std::unordered_map<gate_t, pg_uuid_t, hash_gate_t> uuid_of;
  /* Tokens this backend has already materialised, across calls: the
   * store is append-only and the worker pipe is ordered, so a create
   * this backend has sent once never needs re-sending -- re-running a
   * reachability query on unchanged data then skips the whole gate IPC
   * (the dominant cost of a warm materialisation; content-addressed
   * UUIDs make the key).  Per-backend, bounded: past the cap the set is
   * cleared, which only costs re-sending idempotent creates. */
  static std::unordered_set<std::string> created;
  constexpr std::size_t kCreatedCap = 4u << 20;
  if (created.size() > kCreatedCap)
    created.clear();
  pg_uuid_t one_uuid;
  bool have_one = false;

  const auto ensureOne = [&]() {
                           if (!have_one) {
                             one_uuid = uuidV5("one");
                             provsql_internal_create_gate(&one_uuid, gate_one,
                                                          0, NULL);
                             have_one = true;
                           }
                           return one_uuid;
                         };
  const auto createOnce = [&](const pg_uuid_t &token, gate_type type,
                              const std::vector<pg_uuid_t> &children,
                              bool certified) {
                            const std::string key = uuid2string(token);
                            if (!created.insert(key).second)
                              return;
                            provsql_internal_create_gate(
                              &token, type,
                              static_cast<unsigned>(children.size()),
                              children.empty() ? NULL : children.data());
                            if (certified)
                              provsql_internal_set_infos(&token,
                                                         DNNF_CERT_INFO, 0);
                          };

  std::vector<gate_t> stack(roots);
  while (!stack.empty()) {
    CHECK_FOR_INTERRUPTS();
    const gate_t g = stack.back();
    if (uuid_of.find(g) != uuid_of.end()) {
      stack.pop_back();
      continue;
    }

    const auto t = dd.getGateType(g);

    if (t == BooleanGate::IN) {
      uuid_of[g] = string2uuid(dd.getUUID(g));
      stack.pop_back();
      continue;
    }

    bool ready = true;
    for (const auto &c : dd.getWires(g))
      if (uuid_of.find(c) == uuid_of.end()) {
        stack.push_back(c);
        ready = false;
      }
    if (!ready)
      continue;

    const bool certified = dd.isDNNFCertified(g);
    pg_uuid_t token;

    switch (t) {
    case BooleanGate::NOT:
    {
      const pg_uuid_t child = uuid_of[dd.getWires(g)[0]];
      const pg_uuid_t one = ensureOne();
      token = uuidV5("monus" + uuid2string(one) + uuid2string(child));
      createOnce(token, gate_monus, {one, child}, false);
    }
    break;

    case BooleanGate::AND:
    case BooleanGate::OR:
    {
      const auto &wires = dd.getWires(g);
      if (wires.empty()) {
        // Constants: empty AND is one, empty OR is zero.
        if (t == BooleanGate::AND)
          token = ensureOne();
        else {
          token = uuidV5("zero");
          createOnce(token, gate_zero, {}, false);
        }
      } else if (wires.size() == 1) {
        // Single-child gates are identities: pass the child through.
        token = uuid_of[wires[0]];
      } else {
        std::vector<std::string> texts;
        texts.reserve(wires.size());
        for (const auto &c : wires)
          texts.push_back(uuid2string(uuid_of[c]));
        std::sort(texts.begin(), texts.end());
        std::string name = (t == BooleanGate::AND ? "times{" : "plus{");
        for (std::size_t i = 0; i < texts.size(); ++i) {
          if (i)
            name += ",";
          name += texts[i];
        }
        name += "}";
        token = uuidV5(name);
        std::vector<pg_uuid_t> children;
        children.reserve(wires.size());
        for (const auto &c : wires)
          children.push_back(uuid_of[c]);
        createOnce(token, t == BooleanGate::AND ? gate_times : gate_plus,
                   children, certified);
      }
    }
    break;

    default:
      provsql_error("reachability: unsupported gate in materialisation");
      throw std::runtime_error("unreachable");
    }

    uuid_of[g] = token;
    stack.pop_back();
  }

  return uuid_of;
}

} // namespace

/**
 * @brief PostgreSQL-callable entry point: exact reachability probability.
 *
 * Arguments: see @c compileFromArgs().
 * Returns: the probability that @c target is reachable from @c source.
 */
Datum reachability_evaluate(PG_FUNCTION_ARGS)
{
  try {
    auto result = compileFromArgs(fcinfo);
    PG_RETURN_FLOAT8(result.dd.probabilityEvaluation());
  } catch (const std::exception &e) {
    provsql_error("reachability: %s", e.what());
  } catch (...) {
    provsql_error("reachability: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable entry point: probability plus compilation
 *        statistics.
 *
 * Arguments: see @c compileFromArgs().
 * Returns: composite @c (probability, data_treewidth, nb_bags,
 * max_states, nb_gates, nb_variables).
 */
Datum reachability_compile_stats(PG_FUNCTION_ARGS)
{
  try {
    auto result = compileFromArgs(fcinfo);

    TupleDesc tupdesc;
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      provsql_error("reachability_compile_stats: expected composite return type");
    tupdesc = BlessTupleDesc(tupdesc);

    Datum values[6];
    bool nulls[6] = {false, false, false, false, false, false};
    values[0] = Float8GetDatum(result.dd.probabilityEvaluation());
    values[1] = Int32GetDatum(static_cast<int32>(result.stats.data_treewidth));
    values[2] = Int64GetDatum(static_cast<int64>(result.stats.nb_bags));
    values[3] = Int64GetDatum(static_cast<int64>(result.stats.max_states));
    values[4] = Int64GetDatum(static_cast<int64>(result.stats.nb_gates));
    values[5] = Int64GetDatum(static_cast<int64>(result.stats.nb_variables));

    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  } catch (const std::exception &e) {
    provsql_error("reachability_compile_stats: %s", e.what());
  } catch (...) {
    provsql_error("reachability_compile_stats: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable entry point: all-targets compilation and
 *        materialisation.
 *
 * Arguments: @c srcs @c int[], @c dsts @c int[], @c tokens @c uuid[],
 * @c probs @c float8[], @c source @c int, @c directed @c boolean.
 * Returns: one @c (vertex, token) row per vertex reachable in the
 * all-edges-present world, @c token being the materialised certified
 * provenance circuit of "vertex is reachable from source".  This is the
 * engine behind the rewriter's recursive-reachability route.
 */
Datum reachability_materialize(PG_FUNCTION_ARGS)
{
  try {
    for (int i = 4; i < 6; ++i)
      if (PG_ARGISNULL(i))
        provsql_error("reachability: source and directed must not be NULL");

    auto rows = edgesFromArgs(fcinfo);
    const unsigned long source = static_cast<unsigned long>(PG_GETARG_INT32(4));
    const bool directed = PG_GETARG_BOOL(5);

    ReachabilityCompiler::AllResult all;
    try {
      all = ReachabilityCompiler::compileAll(rows, source, directed);
    } catch (TreeDecompositionException &) {
      provsql_error(
        "reachability: data treewidth exceeds the supported limit (%d)",
        TreeDecomposition::MAX_TREEWIDTH);
    }

    std::vector<gate_t> roots;
    roots.reserve(all.roots.size());
    for (const auto &vr : all.roots)
      roots.push_back(vr.root);
    const auto uuid_of = materializeCertifiedDD(all.dd, roots);

    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    MemoryContext oldcontext = MemoryContextSwitchTo(per_query_ctx);

    TupleDesc tupdesc;
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
      MemoryContextSwitchTo(oldcontext);
      provsql_error("reachability_materialize: function must return a row type");
    }
    tupdesc = BlessTupleDesc(tupdesc);

    Tuplestorestate *tupstore = tuplestore_begin_heap(
      rsinfo->allowedModes & SFRM_Materialize_Random, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    for (const auto &vr : all.roots) {
      Datum values[2];
      bool nulls[2] = {false, false};
      values[0] = Int32GetDatum(static_cast<int32>(vr.vertex));
      pg_uuid_t *u = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
      *u = uuid_of.at(vr.root);
      values[1] = UUIDPGetDatum(u);
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    MemoryContextSwitchTo(oldcontext);
    return (Datum) 0;
  } catch (const std::exception &e) {
    provsql_error("reachability_materialize: %s", e.what());
  } catch (...) {
    provsql_error("reachability_materialize: unknown exception");
  }
  PG_RETURN_NULL();
}
