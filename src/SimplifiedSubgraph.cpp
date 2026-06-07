/**
 * @file SimplifiedSubgraph.cpp
 * @brief SQL function `provsql.simplified_circuit_subgraph()`.
 *
 * Returns a BFS subgraph of the IN-MEMORY GenericCircuit rooted at a
 * given UUID, so consumers see the result of `simplify_on_load` /
 * RangeCheck / AnalyticEvaluator passes rather than the persisted
 * mmap DAG.
 *
 * Output: jsonb array of objects, each
 *   {node, parent, child_pos, gate_type, info1, info2, extra, depth}.
 * Same row shape as the recursive-CTE `circuit_subgraph`, with the
 * `extra` column inlined so the caller doesn't have to round-trip
 * through `get_extra` (which would hit the persisted DAG and miss
 * extras introduced by the simplifier).
 *
 * Returning jsonb (rather than SETOF record) keeps the C++ side free
 * of SRF / FuncCallContext mechanics; a single PG round-trip is the
 * same cost as the recursive CTE in the persisted-DAG path.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/jsonb.h"
#include "utils/fmgrprotos.h"
#include "utils/uuid.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(simplified_circuit_subgraph);
}

#include "CircuitFromMMap.h"
#include "GenericCircuit.h"
#include "HybridEvaluator.h"
#include "provsql_utils_cpp.h"

#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void
json_escape(std::ostringstream &out, const std::string &s)
{
  for (char c : s) {
    switch (c) {
      case '"':  out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b";  break;
      case '\f': out << "\\f";  break;
      case '\n': out << "\\n";  break;
      case '\r': out << "\\r";  break;
      case '\t': out << "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out << buf;
        } else {
          out << c;
        }
    }
  }
}

void
emit_str(std::ostringstream &out, const std::string &s)
{
  out << '"';
  json_escape(out, s);
  out << '"';
}

/* Emit a single output row.  Mirrors `circuit_subgraph`'s columns
 * plus an inline `extra`; info1 / info2 are NULL for gate types that
 * don't use them (input, zero, one), otherwise the integer is
 * rendered as a string so jsonb_in keeps the same wire shape the
 * caller expects (TEXT-typed columns in `circuit_subgraph`'s
 * RETURNS TABLE). */
void
emit_node_row(std::ostringstream &out,
              bool &first,
              const GenericCircuit &gc,
              gate_t g,
              const std::string &node_uuid,
              const std::string *parent_uuid,
              int child_pos,           /* 1-based; ignored if parent_uuid == nullptr */
              int depth)
{
  if (!first) out << ',';
  first = false;
  const auto t = gc.getGateType(g);
  const char *type_name = (t < nb_gate_types) ? gate_type_name[t] : "invalid";
  /* GenericCircuit::getInfos returns {-1u, -1u} when the gate has no
   * entry in the in-memory infos map (CircuitFromMMap only populates
   * it for mulinput / eq / agg / cmp / arith).  Map that sentinel to
   * JSON null so the caller sees "no info" rather than 4294967295. */
  const auto infos = gc.getInfos(g);
  const unsigned NO_INFO = static_cast<unsigned>(-1);

  out << '{';
  out << "\"node\":";        emit_str(out, node_uuid);
  out << ",\"parent\":";
  if (parent_uuid) emit_str(out, *parent_uuid);
  else             out << "null";
  out << ",\"child_pos\":";
  if (parent_uuid) out << child_pos;
  else             out << "null";
  out << ",\"gate_type\":"; emit_str(out, type_name);
  out << ",\"info1\":";
  if (infos.first == NO_INFO) { out << "null"; }
  else                        { out << '"' << infos.first << '"'; }
  out << ",\"info2\":";
  if (infos.second == NO_INFO) { out << "null"; }
  else                         { out << '"' << infos.second << '"'; }
  out << ",\"extra\":";
  try {
    emit_str(out, gc.getExtra(g));
  } catch (const CircuitException &) {
    out << "null";
  }
  /* Emit prob inline for every input / mulinput gate.  Consumers that
   * need a per-gate probability (Studio's anonymous-input inline
   * percentage; categorical-block introspection) get it without a
   * separate provsql.get_prob round-trip.  NaN is serialised as JSON
   * null so jsonb_in does not choke on it. */
  if (t == gate_input || t == gate_mulinput) {
    const double p = gc.getProb(g);
    out << ",\"prob\":";
    if (p != p)         /* NaN */ out << "null";
    else                          out << p;
  }
  /* In-memory Boolean-assumption marker (set by
   * foldBooleanIdentities) -- emit a JSON true so Studio can stamp
   * the same B badge on a flag-marked gate as it does on a
   * persistent gate_assumed wrapper.  Default true is
   * suppressed by only emitting the field when the flag is set, so
   * the wire size stays unchanged for the common case. */
  if (gc.isBooleanAssumed(g)) {
    out << ",\"boolean_assumed\":true";
  }
  out << ",\"depth\":" << depth;
  out << '}';
}

}  // namespace

extern "C" Datum
simplified_circuit_subgraph(PG_FUNCTION_ARGS)
{
  pg_uuid_t *root_arg = (pg_uuid_t *) PG_GETARG_POINTER(0);
  int max_depth = PG_GETARG_INT32(1);

  std::ostringstream out;
  out << '[';
  bool first = true;

  try {
    GenericCircuit gc = getGenericCircuit(*root_arg);
    gate_t root_gate;
    try {
      root_gate = gc.getGate(uuid2string(*root_arg));
    } catch (const CircuitException &) {
      /* Root not in the simplified circuit (shouldn't normally
       * happen).  Return an empty array so the caller can degrade
       * gracefully rather than erroring. */
      out << ']';
      Datum json_datum = DirectFunctionCall1(
        jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
      PG_RETURN_DATUM(json_datum);
    }

    /* getGenericCircuit applied foldSemiringIdentities for us when
     * provsql.simplify_on_load is on, so the wires here already
     * reflect identity / absorber collapses; no extra substitution
     * needed at BFS time.
     *
     * Run the hybrid-evaluator simplifier too when the corresponding
     * GUC is on (default), so consumers see arith folding /
     * normal-family closures the way probability_evaluate would.
     * Otherwise the persisted-DAG view and the in-memory simplified
     * view drift on every introspection feature that depends on a
     * structural rewrite. */
    if (provsql_hybrid_evaluation) {
      provsql::runHybridSimplifier(gc);
    }

    /* Longest-path depth of each reachable gate from the root: the
     * canonical circuit-depth notion. We do a BFS to discover the
     * reachable set, then a Kahn's-style topological relaxation over
     * those nodes to compute the longest path. Bounded by max_depth:
     * a node only enters the result if some path of length at most
     * max_depth reaches it (matching the SQL @c circuit_subgraph CTE
     * semantics). */
    std::unordered_set<gate_t> reachable;
    {
      std::queue<gate_t> q;
      reachable.insert(root_gate);
      q.push(root_gate);
      while (!q.empty()) {
        gate_t g = q.front(); q.pop();
        for (gate_t c : gc.getWires(g)) {
          if (reachable.insert(c).second) q.push(c);
        }
      }
    }
    /* Indegree restricted to the reachable subgraph, so the relaxation
     * processes each node only after all its predecessors. */
    std::unordered_map<gate_t, int> indeg;
    for (gate_t g : reachable) indeg[g] = 0;
    for (gate_t g : reachable) {
      for (gate_t c : gc.getWires(g)) {
        if (reachable.count(c)) indeg[c]++;
      }
    }
    std::unordered_map<gate_t, int> depth_of;
    depth_of[root_gate] = 0;
    std::queue<gate_t> bfs;
    bfs.push(root_gate);
    while (!bfs.empty()) {
      gate_t g = bfs.front(); bfs.pop();
      int d = depth_of[g];
      if (d >= max_depth) continue;
      for (gate_t c : gc.getWires(g)) {
        if (!reachable.count(c)) continue;
        auto it = depth_of.find(c);
        int candidate = d + 1;
        if (it == depth_of.end() || it->second < candidate) {
          depth_of[c] = candidate;
        }
        if (--indeg[c] == 0) bfs.push(c);
      }
    }

    /* Emit one row per (parent, node, child_pos) edge plus a
     * synthetic root row with parent=NULL.  Matches the output shape
     * of the SQL @c circuit_subgraph (one row per (parent, child)
     * triple). */
    const std::string root_uuid = gc.getUUID(root_gate);
    emit_node_row(out, first, gc, root_gate, root_uuid,
                  /* parent */ nullptr, /* child_pos */ 0,
                  /* depth */ 0);

    for (const auto &[g, d] : depth_of) {
      if (d >= max_depth) continue;
      const std::string parent_uuid = gc.getUUID(g);
      const auto &wires = gc.getWires(g);
      for (std::size_t i = 0; i < wires.size(); ++i) {
        gate_t c = wires[i];
        auto cit = depth_of.find(c);
        if (cit == depth_of.end()) continue;  /* unreachable in BFS */
        if (cit->second > max_depth) continue;
        const std::string child_uuid = gc.getUUID(c);
        emit_node_row(out, first, gc, c, child_uuid,
                      &parent_uuid, static_cast<int>(i + 1),
                      cit->second);
      }
    }
  } catch (const std::exception &e) {
    provsql_error("simplified_circuit_subgraph: %s", e.what());
  } catch (...) {
    provsql_error("simplified_circuit_subgraph: unknown exception");
  }

  out << ']';

  Datum json_datum = DirectFunctionCall1(
    jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
  PG_RETURN_DATUM(json_datum);
}
