/**
 * @file UCQJointCompiler.cpp
 * @brief Implementation of the joint-width UCQ compiler's homomorphism
 *        DP (data-graph regime).
 *
 * See @c UCQJointCompiler.h for the construction's design and the
 * structural argument (determinism and decomposability by
 * construction).  This translation unit implements the §3.5 fast path:
 * every fact gated by an independent Bernoulli event, a single
 * bottom-up sweep over a min-fill tree decomposition of the joint
 * (here, Gaifman) graph of the facts, the Boolean UCQ read off the root
 * table.  The correlated regime (in-bag gate valuations, MULVAR blocks)
 * is layered on the same DP core in a later milestone.
 *
 * ### The homomorphism-type state
 *
 * For each disjunct, the state holds the set of partial-homomorphism
 * *codes* realised by the present facts of the processed subtree.  A
 * code assigns each query variable one of: UNASSIGNED (its image is not
 * yet decided -- it will be pinned higher in the tree), a position in
 * the current bag (its image is an in-bag element), or DONE (its image
 * is an already-forgotten element, and *every* atom incident to it has
 * been witnessed).  Alongside, a bitmask records which atoms are
 * witnessed by a present fact under this assignment.  Because witnessing
 * under a fixed assignment is deterministic, the set of achievable codes
 * is a function of the world; states therefore partition worlds and the
 * emitted ORs are deterministic.  When a disjunct's witnessed mask
 * becomes full the whole state collapses to the absorbing @c sat marker
 * -- the main state-pruning lever, the UCQ analogue of the reachability
 * compiler's final collapse.
 */
#include "UCQJointCompiler.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

/* The DP loops are the runtime hot spots on large instances; keep the
 * backend cancellable, mirroring ReachabilityCompiler.cpp's guard
 * pattern (no-op outside the PostgreSQL extension build). */
#ifdef TDKC
#include "tdkc_interrupt.h"
#define CHECK_FOR_INTERRUPTS() provsql_tdkc_poll()
#else
extern "C" {
#include "postgres.h"
#include "miscadmin.h"
}
#endif

namespace {

/** @brief Underlying integer of a @c bag_t. */
inline std::size_t bag_index(bag_t b)
{
  return static_cast<std::underlying_type<bag_t>::type>(b);
}

/** @brief Variable-status sentinels for a homomorphism code. */
constexpr std::int8_t UNASSIGNED = -2;  ///< Image not yet decided.
constexpr std::int8_t DONE = -1;        ///< Image forgotten, all incident atoms witnessed.

/** @brief Per-disjunct precomputed query structure. */
struct DisjunctInfo {
  unsigned n_vars = 0;                 ///< Number of query variables.
  std::vector<Atom> atoms;             ///< The conjuncts.
  std::uint64_t full = 0;              ///< Mask of all atoms (sat threshold).
  std::vector<std::uint64_t> atoms_of_var; ///< Per variable, the mask of incident atoms.
};

/** @brief Precomputed query context shared by all DP operations. */
struct QueryCtx {
  std::vector<DisjunctInfo> disjuncts;
};

/**
 * @brief One partial-homomorphism code over a disjunct's variables.
 *
 * @c st[v] is UNASSIGNED, DONE, or a bag position; @c w is the
 * witnessed-atom bitmask.  Ordered (for canonical sorted code sets) and
 * hashable through @c State.
 */
struct DCode {
  std::vector<std::int8_t> st;   ///< Per-variable status.
  std::uint64_t w = 0;           ///< Witnessed-atom mask.

  bool operator==(const DCode &o) const {
    return w == o.w && st == o.st;
  }
  bool operator<(const DCode &o) const {
    if (w != o.w)
      return w < o.w;
    return st < o.st;
  }
};

/**
 * @brief One DP state: per-disjunct hom-set, or the absorbing @c sat
 *        marker.
 *
 * When @c sat is set the hom-sets are dropped (canonical), so every
 * accepting world collapses to one state whose gate is their
 * deterministic OR.
 */
struct State {
  bool sat = false;
  std::vector<std::vector<DCode> > homs;   ///< Per disjunct, sorted unique codes.

  // Correlated regime only (empty in the data-graph fast path): the
  // valuation of the in-bag slice-gate vertices, and the subset whose
  // (strong) value is asserted but not yet justified by an in-bag child
  // -- the same valuation/suspicious mechanism as
  // dDNNFTreeDecompositionBuilder (Amarilli-Capelli-Monet-Senellart,
  // ToCS 2020).  The @c sat collapse drops @c homs but keeps these
  // (events still contribute probability).
  std::map<unsigned long, bool> gate_val; ///< In-bag gate vertex -> value.
  std::vector<unsigned long> susp;         ///< Suspicious gate vertices (sorted).

  bool operator==(const State &o) const {
    if (sat != o.sat || gate_val != o.gate_val || susp != o.susp)
      return false;
    if (sat)
      return true;
    return homs == o.homs;
  }
};

/** @brief Hash functor over @c State. */
struct StateHash {
  std::size_t operator()(const State &s) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t x) {
                 h ^= x;
                 h *= 1099511628211ull;
               };
    mix(s.sat ? 0x9e3779b9ull : 0x1234567ull);
    for (const auto &[g, v] : s.gate_val)
      mix((g << 1) | (v ? 1u : 0u));
    for (unsigned long g : s.susp)
      mix(g * 0x9e3779b97f4a7c15ull);
    if (!s.sat)
      for (const auto &codes : s.homs) {
        mix(codes.size());
        for (const auto &c : codes) {
          mix(c.w);
          for (std::int8_t v : c.st)
            mix(static_cast<std::uint64_t>(static_cast<std::uint8_t>(v)));
        }
      }
    return static_cast<std::size_t>(h);
  }
};

/** @brief Position of @p v in the sorted @p domain. */
inline int positionIn(const std::vector<unsigned long> &domain, unsigned long v)
{
  return static_cast<int>(
    std::lower_bound(domain.begin(), domain.end(), v) - domain.begin());
}

/** @brief Sort and deduplicate a disjunct's code set in place. */
void canonicalize(std::vector<DCode> &codes)
{
  std::sort(codes.begin(), codes.end());
  codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
}

/** @brief The trivial state: every variable unassigned, nothing witnessed. */
State trivialState(const QueryCtx &q)
{
  State s;
  s.sat = false;
  s.homs.resize(q.disjuncts.size());
  for (std::size_t d = 0; d < q.disjuncts.size(); ++d)
    s.homs[d].push_back(
      DCode{std::vector<std::int8_t>(q.disjuncts[d].n_vars, UNASSIGNED), 0});
  return s;
}

/** @brief The absorbing satisfied state. */
State satState()
{
  State s;
  s.sat = true;
  return s;
}

/**
 * @brief Close a disjunct's code set under witnessing @p fact, in place.
 * @return @c true if some code reached the full witnessed mask (sat).
 */
bool closeDisjunct(const DisjunctInfo &di,
                   std::vector<DCode> &codes,
                   const Fact &fact,
                   const std::vector<unsigned long> &domain,
                   const std::map<unsigned, unsigned long> *head_pin = nullptr)
{
  // Atoms of this disjunct a present @p fact could witness.
  std::vector<std::size_t> cand;
  for (std::size_t ai = 0; ai < di.atoms.size(); ++ai)
    if (di.atoms[ai].relation_id == fact.relation_id &&
        di.atoms[ai].vars.size() == fact.elements.size())
      cand.push_back(ai);
  if (cand.empty())
    return false;

  std::vector<int> pos(fact.elements.size());
  for (std::size_t i = 0; i < fact.elements.size(); ++i)
    pos[i] = positionIn(domain, fact.elements[i]);

  std::set<DCode> seen(codes.begin(), codes.end());
  std::vector<DCode> work(codes.begin(), codes.end());
  bool sat = false;

  while (!work.empty()) {
    DCode c = std::move(work.back());
    work.pop_back();
    for (std::size_t ai : cand) {
      if ((c.w >> ai) & 1u)
        continue;   // already witnessed under this assignment
      const Atom &a = di.atoms[ai];
      DCode c2 = c;
      bool ok = true;
      for (std::size_t i = 0; i < a.vars.size(); ++i) {
        const unsigned v = a.vars[i];
        const std::int8_t p = static_cast<std::int8_t>(pos[i]);
        // Per-answer head pin: a head variable may bind only to its answer's
        // value (the same restriction the Sel-pin enforces, but without
        // touching the encoding -- so the decomposition is shared).
        if (head_pin) {
          auto pit = head_pin->find(v);
          if (pit != head_pin->end() && fact.elements[i] != pit->second) {
            ok = false;
            break;
          }
        }
        if (c2.st[v] == UNASSIGNED)
          c2.st[v] = p;
        else if (c2.st[v] != p) {   // already pinned to a different element
          ok = false;
          break;
        }
      }
      if (!ok)
        continue;
      c2.w |= (std::uint64_t{1} << ai);
      if (seen.insert(c2).second) {
        if (c2.w == di.full)
          sat = true;
        work.push_back(c2);
      }
    }
  }

  codes.assign(seen.begin(), seen.end());
  canonicalize(codes);
  return sat;
}

/** @brief Apply a present fact to a whole state (closure over all disjuncts). */
State closeWithFact(const QueryCtx &q, const State &s, const Fact &fact,
                    const std::vector<unsigned long> &domain,
                    const std::map<unsigned, unsigned long> *head_pin = nullptr)
{
  if (s.sat)
    return s;
  State out = s;
  for (std::size_t d = 0; d < q.disjuncts.size(); ++d)
    if (closeDisjunct(q.disjuncts[d], out.homs[d], fact, domain, head_pin))
      return satState();
  return out;
}

/**
 * @brief Re-express a state over @p to_domain: forget the leaving
 *        elements, remap the surviving ones.
 *
 * A variable pinned to a leaving element becomes DONE if every incident
 * atom is witnessed, else its code is dropped (the assignment can never
 * complete: the element will not reappear, so an unwitnessed incident
 * atom can never be witnessed).
 */
State forgetLift(const QueryCtx &q, const State &s,
                 const std::vector<unsigned long> &from_domain,
                 const std::vector<unsigned long> &to_domain)
{
  if (s.sat || from_domain == to_domain)
    return s;
  std::vector<int> map(from_domain.size());
  for (std::size_t i = 0; i < from_domain.size(); ++i) {
    auto it = std::lower_bound(to_domain.begin(), to_domain.end(),
                               from_domain[i]);
    map[i] = (it != to_domain.end() && *it == from_domain[i])
             ? static_cast<int>(it - to_domain.begin()) : -1;
  }

  State out;
  out.sat = false;
  out.homs.resize(q.disjuncts.size());
  for (std::size_t d = 0; d < q.disjuncts.size(); ++d) {
    const DisjunctInfo &di = q.disjuncts[d];
    std::vector<DCode> &dst = out.homs[d];
    for (const DCode &c : s.homs[d]) {
      DCode c2;
      c2.w = c.w;
      c2.st.resize(di.n_vars);
      bool dead = false;
      for (unsigned v = 0; v < di.n_vars; ++v) {
        const std::int8_t st = c.st[v];
        if (st == UNASSIGNED || st == DONE) {
          c2.st[v] = st;
        } else {
          const int np = map[static_cast<std::size_t>(st)];
          if (np >= 0) {
            c2.st[v] = static_cast<std::int8_t>(np);
          } else if ((c.w & di.atoms_of_var[v]) == di.atoms_of_var[v]) {
            c2.st[v] = DONE;   // discharged, safe to forget
          } else {
            dead = true;       // an incident atom can never be witnessed
            break;
          }
        }
      }
      if (!dead)
        dst.push_back(std::move(c2));
    }
    canonicalize(dst);
  }
  return out;
}

/**
 * @brief Join two states over the same domain, covering disjoint fact
 *        sets (so the gates are decomposable and the states partition
 *        the combined worlds).
 */
State join(const QueryCtx &q, const State &s1, const State &s2)
{
  if (s1.sat || s2.sat)
    return satState();
  State out;
  out.sat = false;
  out.homs.resize(q.disjuncts.size());
  for (std::size_t d = 0; d < q.disjuncts.size(); ++d) {
    const DisjunctInfo &di = q.disjuncts[d];
    std::vector<DCode> &dst = out.homs[d];
    for (const DCode &c1 : s1.homs[d])
      for (const DCode &c2 : s2.homs[d]) {
        DCode c;
        c.w = c1.w | c2.w;
        c.st.resize(di.n_vars);
        bool ok = true;
        for (unsigned v = 0; v < di.n_vars; ++v) {
          const std::int8_t a = c1.st[v];
          const std::int8_t b = c2.st[v];
          std::int8_t r;
          if (a == UNASSIGNED)
            r = b;
          else if (b == UNASSIGNED)
            r = a;
          else if (a == DONE && b == DONE)
            r = DONE;
          else if (a == DONE || b == DONE) {   // forgotten one side, in-bag the other
            ok = false;
            break;
          } else if (a == b)
            r = a;
          else {                                // pinned to different elements
            ok = false;
            break;
          }
          c.st[v] = r;
        }
        if (!ok)
          continue;
        if (c.w == di.full)
          return satState();
        dst.push_back(std::move(c));
      }
    canonicalize(dst);
  }
  return out;
}

// ---------------------------------------------------------------------
// Correlated regime: the homomorphism DP merged with the
// valuation/suspicious gate machinery (Amarilli-Capelli-Monet-Senellart,
// ToCS 2020), run over the joint graph (element + gate vertices).
// ---------------------------------------------------------------------

/** @brief Build the per-disjunct query context and the query stats. */
void buildQueryCtx(const UCQ &ucq, const JointEncoding &enc,
                   QueryCtx &q, UCQJointCompiler::Stats &stats)
{
  q.disjuncts.resize(ucq.disjuncts.size());
  stats.n_enumerating.resize(ucq.disjuncts.size());
  for (std::size_t d = 0; d < ucq.disjuncts.size(); ++d) {
    const CQ &cq = ucq.disjuncts[d];
    DisjunctInfo &di = q.disjuncts[d];
    di.n_vars = cq.n_vars;
    di.atoms = cq.atoms;
    if (cq.atoms.empty())
      throw JointCompilerException("a UCQ disjunct has no atoms");
    if (cq.atoms.size() > 64)
      throw JointCompilerException("a UCQ disjunct has more than 64 atoms");
    di.full = (cq.atoms.size() == 64)
              ? ~std::uint64_t{0}
              : ((std::uint64_t{1} << cq.atoms.size()) - 1);
    di.atoms_of_var.assign(di.n_vars, 0);
    std::vector<bool> appears(di.n_vars, false);
    for (std::size_t ai = 0; ai < cq.atoms.size(); ++ai)
      for (unsigned v : cq.atoms[ai].vars) {
        if (v >= di.n_vars)
          throw JointCompilerException("atom variable out of range");
        di.atoms_of_var[v] |= (std::uint64_t{1} << ai);
        appears[v] = true;
      }
    // The exponential parameter is the number of variables that occur in
    // an atom (the join variables); a variable in no atom never enters a
    // code.  Key/FD determination (a later milestone) shrinks this.
    unsigned ne = 0;
    for (unsigned v = 0; v < di.n_vars; ++v)
      if (appears[v])
        ++ne;
    stats.n_enumerating[d] = ne;
  }
  stats.data_treewidth_lb = enc.data_treewidth_lb;
  stats.circuit_treewidth_lb = enc.circuit_treewidth_lb;
  stats.nb_variables = enc.events.size();
}

/** @brief Strong assignment (forced by one input): OR=true, AND=false. */
inline bool isStrong(SliceGateType t, bool v)
{
  switch (t) {
  case SliceGateType::OR:    return v;
  case SliceGateType::AND:   return !v;
  case SliceGateType::INPUT: return false;
  default:                   return true;   // NOT: forced either way
  }
}

/**
 * @brief Compile the correlated regime: the merged valuation + hom DP.
 *
 * The world variables are the slice's @c INPUT leaves.  Each fact's
 * presence is the value of its slice gate; a fact is witnessed (extends
 * the homomorphisms) exactly when its gate is true in the state.  The
 * gate valuation is carried in @c State::gate_val with the suspicious
 * set @c State::susp; events are introduced (and their literal emitted)
 * at the unique bag where they are forgotten, so each variable's IN gate
 * appears under one node (decomposability), and the per-world state is a
 * function of the introduced events (determinism).
 */
UCQJointCompiler::Result mergedCompile(const JointEncoding &enc,
                                       const QueryCtx &q,
                                       UCQJointCompiler::Stats stats,
                                       unsigned max_treewidth,
                                       std::size_t max_states)
{
  UCQJointCompiler::Result result;
  dDNNF &dd = result.dd;

  const unsigned long E = enc.n_elements;
  const std::vector<SliceGate> &slice = enc.slice;
  const std::size_t D = q.disjuncts.size();
  auto isElem = [&](unsigned long v) { return v < E; };
  auto gidx = [&](unsigned long v) { return static_cast<std::size_t>(v - E); };

  // 1. Width screen + decomposition of the joint graph.
  Graph graph = enc.buildGraph();
  unsigned max_degree = 0;
  if (TreeDecomposition::degeneracyLowerBound(graph, max_degree) > max_treewidth)
    throw TreeDecompositionException();
  std::unordered_map<unsigned long, bag_t> elim;
  const TreeDecomposition td(std::move(graph), &elim);
  stats.joint_treewidth = td.getTreewidth();
  stats.nb_bags = td.getNbBags();
  if (td.getTreewidth() > max_treewidth)
    throw TreeDecompositionException();
  const std::size_t nb_bags = td.getNbBags();

  auto bagIndex = [](bag_t b) {
                    return static_cast<std::underlying_type<bag_t>::type>(b);
                  };

  // Bag domains (all vertices) and element subdomains (for hom positions).
  std::vector<std::vector<unsigned long> > dom(nb_bags), edom(nb_bags);
  for (std::size_t b = 0; b < nb_bags; ++b) {
    for (gate_t g : td.getBag(bag_t{b}))
      dom[b].push_back(static_cast<std::underlying_type<gate_t>::type>(g));
    std::sort(dom[b].begin(), dom[b].end());
    dom[b].erase(std::unique(dom[b].begin(), dom[b].end()), dom[b].end());
    for (unsigned long v : dom[b])
      if (isElem(v))
        edom[b].push_back(v);
  }

  // Each fact at its rep bag: the earliest-eliminated of its clique
  // (elements ∪ its gate vertex).
  std::vector<std::vector<std::size_t> > facts_at_bag(nb_bags);
  for (std::size_t fi = 0; fi < enc.facts.size(); ++fi) {
    const Fact &f = enc.facts[fi];
    std::vector<unsigned long> cl = f.elements;
    if (f.kind == FactGateKind::GATE)
      cl.push_back(E + f.gate);
    bag_t best = elim.at(cl[0]);
    for (unsigned long v : cl)
      if (bagIndex(elim.at(v)) < bagIndex(best))
        best = elim.at(v);
    facts_at_bag[bagIndex(best)].push_back(fi);
  }

  // 2. Gate emission (deterministic OR / decomposable AND, certified).
  using Table = std::unordered_map<State, gate_t, StateHash>;
  using Accumulator = std::unordered_map<State, std::vector<gate_t>, StateHash>;
  const gate_t invalid{static_cast<std::underlying_type<gate_t>::type>(-1)};
  const gate_t true_gate = dd.setGate(BooleanGate::AND);
  dd.setInfo(true_gate, DNNF_CERT_INFO);
  std::vector<gate_t> ev_in(slice.size(), invalid), ev_not(slice.size(), invalid);
  auto inGate = [&](std::size_t i) {
                  if (ev_in[i] == invalid)
                    ev_in[i] = dd.setGate(slice[i].token, BooleanGate::IN,
                                          slice[i].prob);
                  return ev_in[i];
                };
  auto notGate = [&](std::size_t i) {
                   if (ev_not[i] == invalid) {
                     ev_not[i] = dd.setGate(BooleanGate::NOT);
                     dd.addWire(ev_not[i], inGate(i));
                   }
                   return ev_not[i];
                 };
  auto andGate = [&](gate_t a, gate_t b) {
                   if (a == true_gate) return b;
                   if (b == true_gate) return a;
                   gate_t g = dd.setGate(BooleanGate::AND);
                   dd.setInfo(g, DNNF_CERT_INFO);
                   dd.addWire(g, a);
                   dd.addWire(g, b);
                   return g;
                 };
  auto finalize = [&](Accumulator &acc) {
                    Table t;
                    t.reserve(acc.size());
                    for (auto &e : acc) {
                      if (e.second.size() == 1)
                        t.emplace(e.first, e.second[0]);
                      else {
                        gate_t o = dd.setGate(BooleanGate::OR);
                        dd.setInfo(o, DNNF_CERT_INFO);
                        for (gate_t c : e.second)
                          dd.addWire(o, c);
                        t.emplace(e.first, o);
                      }
                    }
                    acc.clear();
                    stats.max_states = std::max(stats.max_states, t.size());
                    if (t.size() > max_states)
                      throw JointCompilerException(
                              "joint DP state space exceeds the per-node bound (" +
                              std::to_string(max_states) + ")");
                    return t;
                  };

  // Weak-constraint local consistency and strong-gate justification.
  auto almost = [&](const State &s) {
                  for (const auto &[gv, gval] : s.gate_val) {
                    const SliceGate &G = slice[gidx(gv)];
                    for (unsigned c : G.children) {
                      auto it = s.gate_val.find(E + c);
                      if (it == s.gate_val.end())
                        continue;
                      const bool cval = it->second;
                      if (G.type == SliceGateType::AND) {
                        if (gval && !cval) return false;
                      } else if (G.type == SliceGateType::OR) {
                        if (!gval && cval) return false;
                      } else if (G.type == SliceGateType::NOT) {
                        if (gval == cval) return false;
                      }
                    }
                  }
                  return true;
                };
  auto justify = [&](State &s) {
                   std::vector<unsigned long> keep;
                   for (unsigned long g : s.susp) {
                     const SliceGate &G = slice[gidx(g)];
                     const bool gval = s.gate_val.at(g);
                     bool just = false;
                     for (unsigned c : G.children) {
                       auto it = s.gate_val.find(E + c);
                       if (it == s.gate_val.end())
                         continue;
                       const bool cval = it->second;
                       if (G.type == SliceGateType::OR && gval && cval) just = true;
                       if (G.type == SliceGateType::AND && !gval && !cval) just = true;
                       if (G.type == SliceGateType::NOT && cval != gval) just = true;
                     }
                     if (!just)
                       keep.push_back(g);
                   }
                   s.susp = std::move(keep);   // already sorted (susp was sorted)
                 };
  auto addSusp = [](State &s, unsigned long g) {
                   auto it = std::lower_bound(s.susp.begin(), s.susp.end(), g);
                   if (it == s.susp.end() || *it != g)
                     s.susp.insert(it, g);
                 };

  // Close a state's homs under a present fact (reuse the hom machinery).
  auto closeFact = [&](State s, const Fact &f,
                       const std::vector<unsigned long> &ed) {
                     if (s.sat)
                       return s;
                     for (std::size_t d = 0; d < D; ++d)
                       if (closeDisjunct(q.disjuncts[d], s.homs[d], f, ed)) {
                         s.sat = true;
                         s.homs.clear();
                         return s;
                       }
                     return s;
                   };

  // Re-express a state's homs over a new element subdomain (M1 forget).
  auto remapHoms = [&](const State &in, State &out,
                       const std::vector<unsigned long> &ef,
                       const std::vector<unsigned long> &et) {
                     if (in.sat) {
                       out.sat = true;
                       out.homs.clear();
                       return true;
                     }
                     out.sat = false;
                     out.homs.assign(D, {});
                     std::vector<int> map(ef.size());
                     for (std::size_t i = 0; i < ef.size(); ++i) {
                       auto it = std::lower_bound(et.begin(), et.end(), ef[i]);
                       map[i] = (it != et.end() && *it == ef[i])
                                ? static_cast<int>(it - et.begin()) : -1;
                     }
                     for (std::size_t d = 0; d < D; ++d) {
                       const DisjunctInfo &di = q.disjuncts[d];
                       std::vector<DCode> &dst = out.homs[d];
                       for (const DCode &c : in.homs[d]) {
                         DCode c2;
                         c2.w = c.w;
                         c2.st.resize(di.n_vars);
                         bool dead = false;
                         for (unsigned v = 0; v < di.n_vars; ++v) {
                           const std::int8_t st = c.st[v];
                           if (st == UNASSIGNED || st == DONE)
                             c2.st[v] = st;
                           else {
                             const int np = map[static_cast<std::size_t>(st)];
                             if (np >= 0)
                               c2.st[v] = static_cast<std::int8_t>(np);
                             else if ((c.w & di.atoms_of_var[v]) == di.atoms_of_var[v])
                               c2.st[v] = DONE;
                             else { dead = true; break; }
                           }
                         }
                         if (!dead)
                           dst.push_back(std::move(c2));
                       }
                       canonicalize(dst);
                     }
                     return true;
                   };

  // lift: forget (event literal / gate non-suspicious / element remap),
  // then introduce fresh gate vertices (enumerate values).
  std::function<Table(const Table &, const std::vector<unsigned long> &,
                      const std::vector<unsigned long> &)> lift =
    [&](const Table &tab, const std::vector<unsigned long> &from,
        const std::vector<unsigned long> &to) -> Table {
      if (from == to)
        return tab;
      std::vector<unsigned long> ef, et;
      for (unsigned long v : from) if (isElem(v)) ef.push_back(v);
      for (unsigned long v : to)   if (isElem(v)) et.push_back(v);
      std::vector<unsigned long> gforget, gintro;
      for (unsigned long v : from)
        if (!isElem(v) && !std::binary_search(to.begin(), to.end(), v))
          gforget.push_back(v);
      for (unsigned long v : to)
        if (!isElem(v) && !std::binary_search(from.begin(), from.end(), v))
          gintro.push_back(v);
      Accumulator acc;
      for (const auto &[St, g] : tab) {
        CHECK_FOR_INTERRUPTS();
        State cur;
        cur.gate_val = St.gate_val;
        cur.susp = St.susp;
        gate_t cg = g;
        bool dead = false;
        for (unsigned long v : gforget) {
          if (slice[gidx(v)].type == SliceGateType::INPUT)
            cg = andGate(cg, St.gate_val.at(v) ? inGate(gidx(v)) : notGate(gidx(v)));
          else if (std::binary_search(St.susp.begin(), St.susp.end(), v)) {
            dead = true;        // strong gate forgotten unjustified
            break;
          }
          cur.gate_val.erase(v);
          auto it = std::lower_bound(cur.susp.begin(), cur.susp.end(), v);
          if (it != cur.susp.end() && *it == v)
            cur.susp.erase(it);
        }
        if (dead)
          continue;
        State base;
        base.gate_val = cur.gate_val;
        base.susp = cur.susp;
        remapHoms(St, base, ef, et);
        std::vector<State> sts = {std::move(base)};
        std::vector<gate_t> gs = {cg};
        for (unsigned long v : gintro) {
          std::vector<State> ns;
          std::vector<gate_t> n2;
          for (std::size_t i = 0; i < sts.size(); ++i)
            for (int b = 0; b < 2; ++b) {
              State nv = sts[i];
              nv.gate_val[v] = static_cast<bool>(b);
              if (slice[gidx(v)].type != SliceGateType::INPUT &&
                  isStrong(slice[gidx(v)].type, static_cast<bool>(b)))
                addSusp(nv, v);
              ns.push_back(std::move(nv));
              n2.push_back(gs[i]);
            }
          sts = std::move(ns);
          gs = std::move(n2);
        }
        for (std::size_t i = 0; i < sts.size(); ++i) {
          justify(sts[i]);
          if (!almost(sts[i]))
            continue;
          acc[std::move(sts[i])].push_back(gs[i]);
        }
      }
      return finalize(acc);
    };

  auto join = [&](const Table &t1, const Table &t2) {
                Accumulator acc;
                for (const auto &[A, ga] : t1)
                  for (const auto &[B, gb] : t2) {
                    CHECK_FOR_INTERRUPTS();
                    bool ok = true;
                    for (const auto &[k, v] : A.gate_val) {
                      auto it = B.gate_val.find(k);
                      if (it != B.gate_val.end() && it->second != v) {
                        ok = false;
                        break;
                      }
                    }
                    if (!ok)
                      continue;
                    State nv;
                    nv.gate_val = A.gate_val;
                    for (const auto &[k, v] : B.gate_val)
                      nv.gate_val[k] = v;
                    // Suspicious only if suspicious in both subtrees.
                    for (unsigned long x : A.susp)
                      if (std::binary_search(B.susp.begin(), B.susp.end(), x))
                        nv.susp.push_back(x);
                    justify(nv);
                    if (!almost(nv))
                      continue;
                    if (A.sat || B.sat) {
                      nv.sat = true;
                      nv.homs.clear();
                    } else {
                      nv.sat = false;
                      nv.homs.assign(D, {});
                      bool sat = false;
                      for (std::size_t d = 0; d < D; ++d) {
                        const DisjunctInfo &di = q.disjuncts[d];
                        std::vector<DCode> &dst = nv.homs[d];
                        for (const DCode &c1 : A.homs[d])
                          for (const DCode &c2 : B.homs[d]) {
                            DCode c;
                            c.w = c1.w | c2.w;
                            c.st.resize(di.n_vars);
                            bool good = true;
                            for (unsigned v = 0; v < di.n_vars; ++v) {
                              const std::int8_t a = c1.st[v], bb = c2.st[v];
                              std::int8_t r;
                              if (a == UNASSIGNED) r = bb;
                              else if (bb == UNASSIGNED) r = a;
                              else if (a == DONE && bb == DONE) r = DONE;
                              else if (a == DONE || bb == DONE) { good = false; break; }
                              else if (a == bb) r = a;
                              else { good = false; break; }
                              c.st[v] = r;
                            }
                            if (!good)
                              continue;
                            if (c.w == di.full) sat = true;
                            dst.push_back(std::move(c));
                          }
                        canonicalize(dst);
                      }
                      if (sat) {
                        nv.sat = true;
                        nv.homs.clear();
                      }
                    }
                    acc[std::move(nv)].push_back(andGate(ga, gb));
                  }
                return finalize(acc);
              };

  auto applyFacts = [&](Table tab, std::size_t b) {
                      for (std::size_t fi : facts_at_bag[b]) {
                        const Fact &f = enc.facts[fi];
                        Accumulator acc;
                        for (const auto &[St, g] : tab) {
                          const bool present =
                            f.kind == FactGateKind::CERTAIN ||
                            (St.gate_val.count(E + f.gate) &&
                             St.gate_val.at(E + f.gate));
                          State ns = present ? closeFact(St, f, edom[b]) : St;
                          acc[std::move(ns)].push_back(g);
                        }
                        tab = finalize(acc);
                      }
                      return tab;
                    };

  // The empty/trivial state introduced into a bag's domain.
  auto trivialTable = [&](const std::vector<unsigned long> &d) {
                        State z;
                        z.homs.assign(D, {});
                        for (std::size_t i = 0; i < D; ++i)
                          z.homs[i].push_back(
                            DCode{std::vector<std::int8_t>(q.disjuncts[i].n_vars,
                                                           UNASSIGNED), 0});
                        Table e;
                        e.emplace(std::move(z), true_gate);
                        return lift(e, {}, d);
                      };

  // 3. Bottom-up sweep (single sweep; the query is Boolean).
  struct Frame {
    bag_t bag;
    std::size_t next_child = 0;
    Table table;
    bool has_table = false;
    explicit Frame(bag_t b) : bag(b) {
    }
  };
  std::vector<Frame> stack;
  stack.push_back(Frame(td.getRoot()));
  std::size_t root_bag = bagIndex(td.getRoot());
  Table root_table;
  while (!stack.empty()) {
    Frame &frame = stack.back();
    const auto &children = td.getChildren(frame.bag);
    if (frame.next_child < children.size()) {
      stack.push_back(Frame(children[frame.next_child++]));
      continue;
    }
    CHECK_FOR_INTERRUPTS();
    const std::size_t b = bagIndex(frame.bag);
    Table table = frame.has_table ? std::move(frame.table) : trivialTable(dom[b]);
    table = applyFacts(std::move(table), b);
    if (stack.size() == 1) {
      root_table = std::move(table);
      stack.pop_back();
      break;
    }
    Frame &parent = stack[stack.size() - 2];
    const std::size_t pb = bagIndex(parent.bag);
    Table lifted = lift(table, dom[b], dom[pb]);
    if (!parent.has_table) {
      parent.table = std::move(lifted);
      parent.has_table = true;
    } else {
      parent.table = join(parent.table, lifted);
    }
    stack.pop_back();
  }

  // 4. Forget everything (apply remaining event literals); accept sat.
  Table top = lift(root_table, dom[root_bag], {});
  std::vector<gate_t> accepting;
  for (const auto &[s, g] : top)
    if (s.sat)
      accepting.push_back(g);
  gate_t root;
  if (accepting.empty()) {
    root = dd.setGate(BooleanGate::OR);
    dd.setInfo(root, DNNF_CERT_INFO);
  } else if (accepting.size() == 1) {
    root = accepting[0];
  } else {
    root = dd.setGate(BooleanGate::OR);
    dd.setInfo(root, DNNF_CERT_INFO);
    for (gate_t g : accepting)
      dd.addWire(root, g);
  }
  dd.setRoot(root);
  stats.dd_size = dd.getNbGates();
  result.stats = std::move(stats);
  return result;
}

} // namespace

/**
 * @brief Data-graph (TID/BID) compile, with optional decomposition reuse
 *        and an optional in-DP head pin.
 *
 * With @p shared_td == nullptr and @p head_pin == nullptr this is the
 * Boolean data-graph compiler verbatim (the path the 223-test suite
 * exercises).  @c compileAnswers supplies a shared decomposition and a
 * per-answer head pin so the gather/encode/decompose stages are paid once.
 */
static UCQJointCompiler::Result compileImpl(
  const JointEncoding &enc,
  const UCQ &ucq,
  unsigned max_treewidth,
  std::size_t max_states,
  const TreeDecomposition *shared_td,
  const std::unordered_map<unsigned long, bag_t> *shared_elim,
  const std::map<unsigned, unsigned long> *head_pin)
{
  using Result = UCQJointCompiler::Result;
  using Stats  = UCQJointCompiler::Stats;
  Result result;
  dDNNF &dd = result.dd;
  Stats &stats = result.stats;

  if (ucq.disjuncts.empty())
    throw JointCompilerException("empty UCQ");

  // ------------------------------------------------------------------
  // 0. Precompute the per-disjunct query structure.
  // ------------------------------------------------------------------
  QueryCtx q;
  buildQueryCtx(ucq, enc, q, stats);

  // Correlated regime (facts gated by internal circuit gates): the
  // merged valuation + homomorphism DP.  (The single-sweep per-answer path
  // -- shared_td / head_pin -- is data-graph only and never gets here.)
  if (enc.correlated)
    return mergedCompile(enc, q, stats, max_treewidth, max_states);

  // ------------------------------------------------------------------
  // 1. Width screen + tree decomposition of the joint graph (or reuse a
  //    shared one).  The degeneracy lower bound proves an unconstructible
  //    width without paying for min-fill (thesis Prop. 4.2.11: the screen
  //    must be on the joint graph; for the data-graph regime that is the
  //    data treewidth, the sound screen there).
  // ------------------------------------------------------------------
  std::unordered_map<unsigned long, bag_t> elim_local;
  std::unique_ptr<TreeDecomposition> built_td;
  const TreeDecomposition *tdp = shared_td;
  if (tdp == nullptr) {
    Graph graph = enc.buildGraph();
    unsigned max_degree = 0;
    if (TreeDecomposition::degeneracyLowerBound(graph, max_degree) >
        max_treewidth)
      throw TreeDecompositionException();
    built_td.reset(new TreeDecomposition(std::move(graph), &elim_local));
    if (built_td->getTreewidth() > max_treewidth)
      throw TreeDecompositionException();
    tdp = built_td.get();
  }
  const TreeDecomposition &td = *tdp;
  const std::unordered_map<unsigned long, bag_t> &elimination_bag =
    shared_elim ? *shared_elim : elim_local;
  stats.joint_treewidth = td.getTreewidth();
  stats.nb_bags = td.getNbBags();
  const std::size_t nb_bags = td.getNbBags();

  // Each fact is introduced at exactly one bag: the bag created when the
  // earliest-eliminated of its elements was eliminated contains all the
  // fact's elements (elimination invariant over the fact's clique).  A
  // unique introduction point is what makes the AND gates decomposable.
  std::vector<std::vector<std::size_t> > facts_at_bag(nb_bags);
  for (std::size_t i = 0; i < enc.facts.size(); ++i) {
    const Fact &f = enc.facts[i];
    bag_t best = elimination_bag.at(f.elements[0]);
    for (unsigned long e : f.elements) {
      bag_t be = elimination_bag.at(e);
      if (bag_index(be) < bag_index(best))
        best = be;
    }
    facts_at_bag[bag_index(best)].push_back(i);
  }

  // Bag domains: the bag's element ids, sorted.
  std::vector<std::vector<unsigned long> > domains(nb_bags);
  for (std::size_t b = 0; b < nb_bags; ++b) {
    auto &dom = domains[b];
    dom.reserve(td.getBag(bag_t{b}).size());
    for (gate_t g : td.getBag(bag_t{b}))
      dom.push_back(static_cast<std::underlying_type<gate_t>::type>(g));
    std::sort(dom.begin(), dom.end());
    dom.erase(std::unique(dom.begin(), dom.end()), dom.end());
  }

  // ------------------------------------------------------------------
  // 2. Gate-emission helpers.  Every emitted OR is deterministic and
  //    every AND decomposable by construction; mark them with the d-D
  //    certificate so the certificate-aware consumers can evaluate the
  //    circuit linearly.
  // ------------------------------------------------------------------
  using Table = std::unordered_map<State, gate_t, StateHash>;
  using Accumulator = std::unordered_map<State, std::vector<gate_t>, StateHash>;

  const gate_t invalid_gate{static_cast<std::underlying_type<gate_t>::type>(-1)};
  const gate_t true_gate = dd.setGate(BooleanGate::AND);   // empty AND = true
  dd.setInfo(true_gate, DNNF_CERT_INFO);

  std::vector<gate_t> ev_in(enc.events.size(), invalid_gate);
  std::vector<gate_t> ev_not(enc.events.size(), invalid_gate);
  auto inGate = [&](std::size_t e) {
                  if (ev_in[e] == invalid_gate)
                    ev_in[e] = dd.setGate(enc.events[e].token, BooleanGate::IN,
                                          enc.events[e].prob);
                  return ev_in[e];
                };
  auto notGate = [&](std::size_t e) {
                   if (ev_not[e] == invalid_gate) {
                     ev_not[e] = dd.setGate(BooleanGate::NOT);
                     dd.addWire(ev_not[e], inGate(e));
                   }
                   return ev_not[e];
                 };
  auto andGate = [&](gate_t a, gate_t b) {
                   if (a == true_gate)
                     return b;
                   if (b == true_gate)
                     return a;
                   gate_t g = dd.setGate(BooleanGate::AND);
                   dd.setInfo(g, DNNF_CERT_INFO);
                   dd.addWire(g, a);
                   dd.addWire(g, b);
                   return g;
                 };

  auto finalize = [&](Accumulator &acc) {
                    Table t;
                    t.reserve(acc.size());
                    for (auto &entry : acc) {
                      if (entry.second.size() == 1)
                        t.emplace(entry.first, entry.second[0]);
                      else {
                        gate_t g = dd.setGate(BooleanGate::OR);
                        dd.setInfo(g, DNNF_CERT_INFO);
                        for (gate_t c : entry.second)
                          dd.addWire(g, c);
                        t.emplace(entry.first, g);
                      }
                    }
                    acc.clear();
                    stats.max_states = std::max(stats.max_states, t.size());
                    if (t.size() > max_states)
                      throw JointCompilerException(
                              "joint DP state space exceeds the per-node bound (" +
                              std::to_string(max_states) + ")");
                    return t;
                  };

  const State kTrivial = trivialState(q);
  auto trivialTable = [&]() {
                        return Table{{kTrivial, true_gate}};
                      };
  auto isTrivial = [&](const Table &t) {
                     return t.size() == 1 && t.begin()->second == true_gate &&
                            t.begin()->first == kTrivial;
                   };

  // Re-express a child's below table over the parent domain (forget the
  // leaving elements, remap the survivors).  States may collapse, hence
  // the accumulator.
  auto lift = [&](const Table &t, const std::vector<unsigned long> &from,
                  const std::vector<unsigned long> &to) {
                if (from == to)
                  return t;
                Accumulator acc;
                for (const auto &entry : t)
                  acc[forgetLift(q, entry.first, from, to)].push_back(
                    entry.second);
                return finalize(acc);
              };

  // Join two tables over the same domain (disjoint fact sets).
  auto joinTables = [&](const Table &t1, const Table &t2) {
                      if (isTrivial(t1))
                        return t2;
                      if (isTrivial(t2))
                        return t1;
                      Accumulator acc;
                      for (const auto &left : t1)
                        for (const auto &right : t2) {
                          CHECK_FOR_INTERRUPTS();
                          acc[join(q, left.first, right.first)].push_back(
                            andGate(left.second, right.second));
                        }
                      return finalize(acc);
                    };

  // Introduce bag b's facts into a table over b's domain.
  auto applyFacts = [&](Table table, std::size_t b) {
                      const auto &domain = domains[b];
                      for (std::size_t fi : facts_at_bag[b]) {
                        const Fact &fact = enc.facts[fi];
                        Accumulator acc;
                        for (const auto &entry : table) {
                          CHECK_FOR_INTERRUPTS();
                          State present = closeWithFact(q, entry.first, fact,
                                                        domain, head_pin);
                          if (fact.kind == FactGateKind::CERTAIN) {
                            acc[std::move(present)].push_back(entry.second);
                          } else if (present == entry.first) {
                            // The fact cannot change the state in these
                            // worlds: keep the gate (its value marginalises).
                            acc[entry.first].push_back(entry.second);
                          } else {
                            acc[std::move(present)].push_back(
                              andGate(entry.second, inGate(fact.event)));
                            acc[entry.first].push_back(
                              andGate(entry.second, notGate(fact.event)));
                          }
                        }
                        table = finalize(acc);
                      }
                      return table;
                    };

  // ------------------------------------------------------------------
  // 3. Bottom-up sweep (single sweep: the query is Boolean, so the root
  //    table already determines satisfaction -- the top-down sweep is
  //    only needed for free first-order variables).
  // ------------------------------------------------------------------
  struct Frame {
    bag_t bag;
    std::size_t next_child = 0;
    Table table;
    bool has_table = false;
    explicit Frame(bag_t b) : bag(b) {
    }
  };

  Table root_table;
  {
    std::vector<Frame> stack;
    stack.push_back(Frame(td.getRoot()));
    while (!stack.empty()) {
      Frame &frame = stack.back();
      const auto &children = td.getChildren(frame.bag);
      if (frame.next_child < children.size()) {
        bag_t c = children[frame.next_child++];
        stack.push_back(Frame(c));
        continue;
      }

      CHECK_FOR_INTERRUPTS();
      const std::size_t b = bag_index(frame.bag);
      Table table = frame.has_table ? std::move(frame.table) : trivialTable();
      table = applyFacts(std::move(table), b);

      if (stack.size() == 1) {
        root_table = std::move(table);
        stack.pop_back();
        break;
      }

      Frame &parent = stack[stack.size() - 2];
      const std::size_t pb = bag_index(parent.bag);
      Table lifted = lift(table, domains[b], domains[pb]);
      if (!parent.has_table) {
        parent.table = std::move(lifted);
        parent.has_table = true;
      } else {
        parent.table = joinTables(parent.table, lifted);
      }
      stack.pop_back();
    }
  }

  // ------------------------------------------------------------------
  // 4. Root: the answer is the deterministic OR over the accepting
  //    (sat) root states.  Satisfaction is captured during the sweep
  //    (witnessing only happens at fact introduction / join), so no
  //    final re-expression is needed.
  // ------------------------------------------------------------------
  std::vector<gate_t> accepting;
  for (const auto &[s, g] : root_table)
    if (s.sat)
      accepting.push_back(g);

  gate_t root;
  if (accepting.empty()) {
    root = dd.setGate(BooleanGate::OR);   // constant false
    dd.setInfo(root, DNNF_CERT_INFO);
  } else if (accepting.size() == 1) {
    root = accepting[0];
  } else {
    root = dd.setGate(BooleanGate::OR);
    dd.setInfo(root, DNNF_CERT_INFO);
    for (gate_t g : accepting)
      dd.addWire(root, g);
  }
  dd.setRoot(root);
  stats.dd_size = dd.getNbGates();
  return result;
}

UCQJointCompiler::Result UCQJointCompiler::compile(
  const JointEncoding &enc,
  const UCQ &ucq,
  unsigned max_treewidth,
  std::size_t max_states)
{
  return compileImpl(enc, ucq, max_treewidth, max_states,
                     nullptr, nullptr, nullptr);
}

std::vector<UCQJointCompiler::Answer> UCQJointCompiler::compileAnswers(
  const JointEncoding &enc,
  const UCQ &ucq,
  const std::vector<unsigned> &head_vars,
  const std::vector<std::vector<unsigned long> > &candidates,
  unsigned max_treewidth,
  std::size_t max_states)
{
  if (ucq.disjuncts.empty())
    throw JointCompilerException("empty UCQ");
  if (enc.correlated)
    throw JointCompilerException(
      "compileAnswers: single-sweep is data-graph (TID/BID) only");

  // Build the joint graph + tree decomposition ONCE; the head pin keeps the
  // encoding and decomposition identical across answers.
  Graph graph = enc.buildGraph();
  unsigned max_degree = 0;
  if (TreeDecomposition::degeneracyLowerBound(graph, max_degree) > max_treewidth)
    throw TreeDecompositionException();
  std::unordered_map<unsigned long, bag_t> elim;
  TreeDecomposition td(std::move(graph), &elim);
  if (td.getTreewidth() > max_treewidth)
    throw TreeDecompositionException();

  std::vector<Answer> answers;
  for (const std::vector<unsigned long> &tuple : candidates) {
    if (tuple.size() != head_vars.size())
      throw JointCompilerException("compileAnswers: head tuple arity mismatch");
    std::map<unsigned, unsigned long> pin;
    for (std::size_t i = 0; i < head_vars.size(); ++i)
      pin[head_vars[i]] = tuple[i];
    Result r = compileImpl(enc, ucq, max_treewidth, max_states, &td, &elim, &pin);
    const double p = r.dd.probabilityEvaluation();
    if (p > 0.0) {
      Answer a;
      a.head = tuple;
      a.probability = p;
      a.max_states = r.stats.max_states;
      answers.push_back(std::move(a));
    }
  }
  return answers;
}
