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
 * is not handled by this fast path.
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
#include <tuple>
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

/**
 * @brief Number of *essential* (enumerating) variables of a disjunct.
 *
 * The exponential parameter of the DP is not every join variable but only
 * those that must be enumerated: a variable functionally determined by others
 * does not multiply the partial-homomorphism state.  This returns the size of
 * a minimum set @c S of join variables whose FD closure covers all of them
 * (the @c e of the @f$2^{O(k^e)}@f$ bound).
 *
 * The FDs are **mined from the gathered tuples** (@p enc.facts, already
 * post-selection): a column set determines a column when no two tuples of the
 * relation agree on the former and differ on the latter.  This captures any
 * declared key (which necessarily holds on the data) along with FDs incidental
 * to this instance, and needs no catalog lookup.  The closure is sound for the
 * instance being compiled: if the FD holds on these tuples the variable is
 * genuinely fixed in every world of this computation.
 *
 * The minimum-cover search is @f$2^{|V|}@f$ in the join-variable count @c |V|;
 * above @c MAX_FD_VARS it is skipped and the full count returned (a sound upper
 * bound), the design target being a handful of enumerating variables.
 */
unsigned essentialVarCount(const CQ &cq, const std::vector<bool> &appears,
                           const JointEncoding &enc)
{
  std::vector<unsigned> V;          // the join variables
  for (unsigned v = 0; v < cq.n_vars; ++v)
    if (appears[v])
      V.push_back(v);
  const unsigned nV = static_cast<unsigned>(V.size());
  static constexpr unsigned MAX_FD_VARS = 12;
  if (nV <= 1 || nV > MAX_FD_VARS)
    return nV;

  // Tuples per (relation, arity), as pointers into enc.facts.
  std::map<std::pair<unsigned, std::size_t>,
           std::vector<const std::vector<unsigned long> *> > byrel;
  for (const Fact &f : enc.facts)
    byrel[{f.relation_id, f.elements.size()}].push_back(&f.elements);

  // Memoised data FD: do the columns in bitmask @c known determine column @c c
  // in the tuples of (@c rel, @c arity)?
  std::map<std::tuple<unsigned, std::size_t, std::uint64_t, unsigned>, bool> memo;
  auto dataFD = [&](unsigned rel, std::size_t arity,
                    std::uint64_t known, unsigned c) -> bool {
    auto key = std::make_tuple(rel, arity, known, c);
    auto it = memo.find(key);
    if (it != memo.end())
      return it->second;
    bool holds = true;
    auto rit = byrel.find({rel, arity});
    if (rit != byrel.end()) {
      std::map<std::vector<unsigned long>, unsigned long> grp;
      for (const std::vector<unsigned long> *t : rit->second) {
        std::vector<unsigned long> kv;
        for (std::size_t p = 0; p < arity; ++p)
          if (known & (std::uint64_t{1} << p))
            kv.push_back((*t)[p]);
        auto git = grp.find(kv);
        if (git == grp.end())
          grp.emplace(std::move(kv), (*t)[c]);
        else if (git->second != (*t)[c]) {
          holds = false;
          break;
        }
      }
    }
    memo[key] = holds;
    return holds;
  };

  std::unordered_map<unsigned, unsigned> bitOf;
  for (unsigned i = 0; i < nV; ++i)
    bitOf[V[i]] = i;
  const std::uint32_t fullMask = (std::uint32_t{1} << nV) - 1;

  // FD closure of a determined set (bitmask over @c V).
  auto closure = [&](std::uint32_t det) -> std::uint32_t {
    bool changed = true;
    while (changed) {
      changed = false;
      for (const Atom &a : cq.atoms) {
        const std::size_t arity = a.vars.size();
        std::uint64_t knownPos = 0;
        for (std::size_t p = 0; p < arity; ++p)
          if (det & (std::uint32_t{1} << bitOf[a.vars[p]]))
            knownPos |= (std::uint64_t{1} << p);
        for (std::size_t p = 0; p < arity; ++p) {
          if (knownPos & (std::uint64_t{1} << p))
            continue;                       // this column already determined
          const unsigned bit = bitOf[a.vars[p]];
          if (dataFD(a.relation_id, arity, knownPos, static_cast<unsigned>(p))) {
            det |= (std::uint32_t{1} << bit);
            knownPos |= (std::uint64_t{1} << p);
            changed = true;
          }
        }
      }
    }
    return det;
  };

  unsigned best = nV;
  for (std::uint32_t s = 0; s <= fullMask; ++s) {
    const unsigned pc = static_cast<unsigned>(__builtin_popcount(s));
    if (pc >= best)
      continue;                             // cannot beat the current minimum
    if (closure(s) == fullMask)
      best = pc;
  }
  return best;
}

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
    // The exponential parameter is the number of *essential* join variables:
    // those that must be enumerated once the ones functionally determined by
    // others (via FDs mined from the gathered data) are removed.
    stats.n_enumerating[d] = essentialVarCount(cq, appears, enc);
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
                                       std::size_t max_states,
                                       const TreeDecomposition *shared_td,
                                       const std::unordered_map<unsigned long,
                                                                bag_t> *shared_elim,
                                       const std::map<unsigned,
                                                      unsigned long> *head_pin)
{
  UCQJointCompiler::Result result;
  dDNNF &dd = result.dd;

  const unsigned long E = enc.n_elements;
  const std::vector<SliceGate> &slice = enc.slice;
  const std::size_t D = q.disjuncts.size();
  auto isElem = [&](unsigned long v) { return v < E; };
  auto gidx = [&](unsigned long v) { return static_cast<std::size_t>(v - E); };

  // 1. Width screen + decomposition of the joint graph (or reuse a shared
  //    one: the single-sweep per-answer path builds it once and pins the
  //    head in the DP, so the joint graph -- data plus circuit -- is
  //    identical across answers).
  std::unordered_map<unsigned long, bag_t> elim_local;
  std::unique_ptr<TreeDecomposition> built_td;
  const TreeDecomposition *tdp = shared_td;
  if (tdp == nullptr) {
    Graph graph = enc.buildGraph();
    unsigned max_degree = 0;
    if (TreeDecomposition::degeneracyLowerBound(graph, max_degree) > max_treewidth)
      throw TreeDecompositionException();
    built_td.reset(new TreeDecomposition(std::move(graph), &elim_local));
    if (built_td->getTreewidth() > max_treewidth)
      throw TreeDecompositionException();
    tdp = built_td.get();
  }
  const TreeDecomposition &td = *tdp;
  const std::unordered_map<unsigned long, bag_t> &elim =
    shared_elim ? *shared_elim : elim_local;
  stats.joint_treewidth = td.getTreewidth();
  stats.nb_bags = td.getNbBags();
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
  // The head pin (single-sweep per-answer) is threaded through here: a fact
  // binding a head variable to an element other than the answer's value is
  // rejected, so a pinned sweep computes P(exists witness with head = v).
  auto closeFact = [&](State s, const Fact &f,
                       const std::vector<unsigned long> &ed) {
                     if (s.sat)
                       return s;
                     for (std::size_t d = 0; d < D; ++d)
                       if (closeDisjunct(q.disjuncts[d], s.homs[d], f, ed,
                                         head_pin)) {
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

// =====================================================================
// Full top-down single-DP for per-answer evaluation (data-graph regime).
//
// One bottom-up sweep emits one d-DNNF root per answer, replacing the k
// head-pinned sweeps of compileAnswers.  The head variables become a
// STATE-LEVEL key: a head variable is never existentially projected (when
// its element leaves the bag it is recorded as a fixed value, not collapsed
// to DONE-and-forgotten), so different head bindings live in different
// states.  Completed answers are tracked per head-tuple in the state's
// @c done set, and an answer is EMITTED as its own circuit root at the lift
// where the last of its head elements leaves the decomposition (no future
// fact can touch it, so its provenance is final).  The answer roots share
// one circuit; the gate cache values them all in (amortised) one pass.
// =====================================================================

/** @brief Head bookkeeping: which query variables are the head, and the slot
 *         of each.  @c slot_of_var[v] is the head position of variable @p v,
 *         or -1 if @p v is not a head variable. */
struct HeadInfo {
  std::vector<unsigned> head_vars;     ///< Query-variable indices of the head.
  std::vector<int> slot_of_var;        ///< var -> head slot, or -1.
  std::size_t n_head() const { return head_vars.size(); }
};

/** @brief No-value sentinel for an unbound / in-bag head slot. */
constexpr unsigned long NO_VAL = static_cast<unsigned long>(-1);

/** @brief Augmented hom code: head variables additionally carry the fixed
 *         element value once forgotten (@c st[v]==DONE for a head var). */
struct ADCode {
  std::vector<std::int8_t> st;         ///< Per variable: UNASSIGNED/DONE/pos.
  std::uint64_t w = 0;                 ///< Witnessed-atom mask.
  std::vector<unsigned long> hval;     ///< Per head slot: fixed value or NO_VAL.

  bool operator==(const ADCode &o) const {
    return w == o.w && st == o.st && hval == o.hval;
  }
  bool operator<(const ADCode &o) const {
    if (w != o.w) return w < o.w;
    if (st != o.st) return st < o.st;
    return hval < o.hval;
  }
};

/** @brief Augmented DP state: per-disjunct hom-set plus the set of head
 *         tuples already satisfied (sorted, unique). */
struct AState {
  std::vector<std::vector<ADCode> > homs;
  std::vector<std::vector<unsigned long> > done;   ///< Completed head tuples.

  bool operator==(const AState &o) const {
    return done == o.done && homs == o.homs;
  }
};

struct AStateHash {
  std::size_t operator()(const AState &s) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t x) { h ^= x; h *= 1099511628211ull; };
    for (const auto &t : s.done) {
      mix(0xD0E + t.size());
      for (unsigned long e : t) mix(e * 0x9e3779b97f4a7c15ull);
    }
    for (const auto &codes : s.homs) {
      mix(codes.size());
      for (const auto &c : codes) {
        mix(c.w);
        for (std::int8_t v : c.st)
          mix(static_cast<std::uint64_t>(static_cast<std::uint8_t>(v)));
        for (unsigned long e : c.hval) mix(e + 0x9e37);
      }
    }
    return static_cast<std::size_t>(h);
  }
};

inline void canonicalizeA(std::vector<ADCode> &codes)
{
  std::sort(codes.begin(), codes.end());
  codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
}

/** @brief Insert a head tuple into the sorted-unique @c done set. */
inline void addDone(std::vector<std::vector<unsigned long> > &done,
                    const std::vector<unsigned long> &t)
{
  auto it = std::lower_bound(done.begin(), done.end(), t);
  if (it == done.end() || *it != t)
    done.insert(it, t);
}

/** @brief The head-tuple value of a full code (every head var bound). */
inline std::vector<unsigned long> readHeadTuple(
  const ADCode &c, const HeadInfo &hi,
  const std::vector<unsigned long> &domain)
{
  std::vector<unsigned long> t(hi.n_head());
  for (std::size_t i = 0; i < hi.n_head(); ++i) {
    const unsigned v = hi.head_vars[i];
    const std::int8_t s = c.st[v];
    if (s >= 0)
      t[i] = domain[static_cast<std::size_t>(s)];
    else if (s == DONE)
      t[i] = c.hval[i];
    else
      throw JointCompilerException(
        "compileAnswersOneDP: head variable unbound at completion "
        "(head must occur in every disjunct)");
  }
  return t;
}

/**
 * @brief Close a disjunct's augmented code set under a present fact.
 *
 * Like @c closeDisjunct but (a) the head variables are bound like any other
 * (no pin), and (b) a code that reaches the full witnessed mask is a
 * COMPLETION: its head tuple is appended to @p completions and the code is
 * discharged (dropped) rather than turned into a sat collapse.
 */
void closeDisjunctA(const DisjunctInfo &di, const HeadInfo &hi,
                    std::vector<ADCode> &codes, const Fact &fact,
                    const std::vector<unsigned long> &domain,
                    std::vector<std::vector<unsigned long> > &completions)
{
  std::vector<std::size_t> cand;
  for (std::size_t ai = 0; ai < di.atoms.size(); ++ai)
    if (di.atoms[ai].relation_id == fact.relation_id &&
        di.atoms[ai].vars.size() == fact.elements.size())
      cand.push_back(ai);
  if (cand.empty())
    return;

  std::vector<int> pos(fact.elements.size());
  for (std::size_t i = 0; i < fact.elements.size(); ++i)
    pos[i] = positionIn(domain, fact.elements[i]);

  std::set<ADCode> seen(codes.begin(), codes.end());
  std::vector<ADCode> work(codes.begin(), codes.end());

  while (!work.empty()) {
    ADCode c = std::move(work.back());
    work.pop_back();
    for (std::size_t ai : cand) {
      if ((c.w >> ai) & 1u)
        continue;
      const Atom &a = di.atoms[ai];
      ADCode c2 = c;
      bool ok = true;
      for (std::size_t i = 0; i < a.vars.size(); ++i) {
        const unsigned v = a.vars[i];
        const std::int8_t p = static_cast<std::int8_t>(pos[i]);
        if (c2.st[v] == UNASSIGNED)
          c2.st[v] = p;
        else if (c2.st[v] != p) {
          ok = false;
          break;
        }
      }
      if (!ok)
        continue;
      c2.w |= (std::uint64_t{1} << ai);
      if (seen.insert(c2).second) {
        if (c2.w == di.full)
          completions.push_back(readHeadTuple(c2, hi, domain));
        else
          work.push_back(c2);
      }
    }
  }

  codes.clear();
  for (const ADCode &c : seen)
    if (c.w != di.full)
      codes.push_back(c);
  canonicalizeA(codes);
}

/** @brief Apply a present fact to an augmented state (all disjuncts). */
AState closeWithFactA(const QueryCtx &q, const HeadInfo &hi, const AState &s,
                      const Fact &fact, const std::vector<unsigned long> &domain)
{
  AState out = s;
  std::vector<std::vector<unsigned long> > comp;
  for (std::size_t d = 0; d < q.disjuncts.size(); ++d)
    closeDisjunctA(q.disjuncts[d], hi, out.homs[d], fact, domain, comp);
  for (const auto &t : comp)
    addDone(out.done, t);
  return out;
}

/**
 * @brief Re-express an augmented state over @p to_domain.
 *
 * Non-head variables forget exactly as @c forgetLift (DONE if discharged,
 * else the code dies).  A head variable pinned to a leaving element is, if
 * discharged, set DONE with its element VALUE recorded in @c hval (so the
 * answer survives); else the code dies.  The @c done set is carried verbatim
 * (its tuples are element values, immune to position remapping).
 */
AState forgetLiftA(const QueryCtx &q, const HeadInfo &hi, const AState &s,
                   const std::vector<unsigned long> &from_domain,
                   const std::vector<unsigned long> &to_domain)
{
  if (from_domain == to_domain)
    return s;
  std::vector<int> map(from_domain.size());
  for (std::size_t i = 0; i < from_domain.size(); ++i) {
    auto it = std::lower_bound(to_domain.begin(), to_domain.end(),
                               from_domain[i]);
    map[i] = (it != to_domain.end() && *it == from_domain[i])
             ? static_cast<int>(it - to_domain.begin()) : -1;
  }

  AState out;
  out.done = s.done;
  out.homs.resize(q.disjuncts.size());
  for (std::size_t d = 0; d < q.disjuncts.size(); ++d) {
    const DisjunctInfo &di = q.disjuncts[d];
    std::vector<ADCode> &dst = out.homs[d];
    for (const ADCode &c : s.homs[d]) {
      ADCode c2;
      c2.w = c.w;
      c2.st.resize(di.n_vars);
      c2.hval = c.hval;
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
            c2.st[v] = DONE;
            const int slot = hi.slot_of_var[v];
            if (slot >= 0)
              c2.hval[static_cast<std::size_t>(slot)] =
                from_domain[static_cast<std::size_t>(st)];
          } else {
            dead = true;
            break;
          }
        }
      }
      if (!dead)
        dst.push_back(std::move(c2));
    }
    canonicalizeA(dst);
  }
  return out;
}

/** @brief Join two augmented states over the same domain (disjoint facts). */
AState joinA(const QueryCtx &q, const HeadInfo &hi,
             const std::vector<unsigned long> &domain,
             const AState &s1, const AState &s2)
{
  AState out;
  out.done = s1.done;
  for (const auto &t : s2.done)
    addDone(out.done, t);
  out.homs.resize(q.disjuncts.size());
  for (std::size_t d = 0; d < q.disjuncts.size(); ++d) {
    const DisjunctInfo &di = q.disjuncts[d];
    std::vector<ADCode> &dst = out.homs[d];
    for (const ADCode &c1 : s1.homs[d])
      for (const ADCode &c2 : s2.homs[d]) {
        ADCode c;
        c.w = c1.w | c2.w;
        c.st.resize(di.n_vars);
        c.hval.assign(hi.n_head(), NO_VAL);
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
          else if (a == DONE || b == DONE) {
            ok = false;
            break;
          } else if (a == b)
            r = a;
          else {
            ok = false;
            break;
          }
          c.st[v] = r;
          // Reconcile the head value when a head variable is (now) forgotten.
          const int slot = hi.slot_of_var[v];
          if (r == DONE && slot >= 0) {
            const unsigned long v1 = (a == DONE)
              ? c1.hval[static_cast<std::size_t>(slot)] : NO_VAL;
            const unsigned long v2 = (b == DONE)
              ? c2.hval[static_cast<std::size_t>(slot)] : NO_VAL;
            if (v1 != NO_VAL && v2 != NO_VAL && v1 != v2) {
              ok = false;
              break;
            }
            c.hval[static_cast<std::size_t>(slot)] =
              (v1 != NO_VAL) ? v1 : v2;
          }
        }
        if (!ok)
          continue;
        if (c.w == di.full)
          addDone(out.done, readHeadTuple(c, hi, domain));
        else
          dst.push_back(std::move(c));
      }
    canonicalizeA(dst);
  }
  return out;
}

/** @brief The trivial augmented state (every @c hval slot unbound). */
AState trivialAState(const QueryCtx &q, const HeadInfo &hi)
{
  AState s;
  s.homs.resize(q.disjuncts.size());
  for (std::size_t d = 0; d < q.disjuncts.size(); ++d)
    s.homs[d].push_back(
      ADCode{std::vector<std::int8_t>(q.disjuncts[d].n_vars, UNASSIGNED), 0,
             std::vector<unsigned long>(hi.n_head(), NO_VAL)});
  return s;
}

/**
 * @brief The data-graph single top-down DP: build one d-DNNF root per answer
 *        and evaluate them all from the shared circuit.
 */
/** @brief Head bookkeeping built from a UCQ + head-variable list. */
HeadInfo buildHeadInfo(const QueryCtx &q, const std::vector<unsigned> &head_vars)
{
  HeadInfo hi;
  hi.head_vars = head_vars;
  unsigned maxv = 0;
  for (const auto &di : q.disjuncts)
    maxv = std::max(maxv, di.n_vars);
  hi.slot_of_var.assign(maxv, -1);
  for (std::size_t i = 0; i < head_vars.size(); ++i) {
    if (head_vars[i] >= maxv)
      throw JointCompilerException("compileAnswersOneDP: head var out of range");
    hi.slot_of_var[head_vars[i]] = static_cast<int>(i);
  }
  for (const auto &di : q.disjuncts)
    for (unsigned hv : head_vars)
      if (hv >= di.n_vars || di.atoms_of_var[hv] == 0)
        throw JointCompilerException(
          "compileAnswersOneDP: a head variable does not occur in a disjunct");
  return hi;
}

/** @brief The value a code binds to head slot @p i over @p dom (NO_VAL if
 *         unbound: at a position -> dom[pos]; DONE -> hval; else unbound). */
inline unsigned long headValOf(const ADCode &c, const HeadInfo &hi, std::size_t i,
                               const std::vector<unsigned long> &dom)
{
  const std::int8_t s = c.st[hi.head_vars[i]];
  if (s == UNASSIGNED) return NO_VAL;
  if (s == DONE) return c.hval[i];
  return dom[static_cast<std::size_t>(s)];
}

/** @brief Does a (partial) code still bind every head slot to tuple @p t? */
inline bool bindsTuple(const ADCode &c, const HeadInfo &hi,
                       const std::vector<unsigned long> &t,
                       const std::vector<unsigned long> &dom)
{
  for (std::size_t i = 0; i < hi.n_head(); ++i)
    if (headValOf(c, hi, i, dom) != t[i])
      return false;
  return true;
}

/**
 * @brief Augmented merged state for the CORRELATED single top-down DP: the
 *        answer core (per-disjunct hom codes + completed head tuples) carried
 *        alongside the slice-gate valuation and suspicious set.
 */
struct CState {
  AState core;                              ///< homs (with hval) + done tuples.
  std::map<unsigned long, bool> gate_val;   ///< In-bag gate vertex -> value.
  std::vector<unsigned long> susp;          ///< Suspicious gate vertices (sorted).

  bool operator==(const CState &o) const {
    return gate_val == o.gate_val && susp == o.susp && core == o.core;
  }
};

struct CStateHash {
  std::size_t operator()(const CState &s) const noexcept {
    std::uint64_t h = static_cast<std::uint64_t>(AStateHash{}(s.core));
    auto mix = [&](std::uint64_t x) { h ^= x; h *= 1099511628211ull; };
    for (const auto &[g, v] : s.gate_val)
      mix((g << 1) | (v ? 1u : 0u));
    for (unsigned long g : s.susp)
      mix(g * 0x9e3779b97f4a7c15ull);
    return static_cast<std::size_t>(h);
  }
};

/**
 * @brief The CORRELATED single top-down DP: one bottom-up sweep over the
 *        joint data+circuit decomposition emits one d-DNNF root per answer.
 *
 * Combines the merged valuation/suspicious gate DP of @c mergedCompile (the
 * world variables are the slice INPUT leaves; a fact is present iff its slice
 * gate is true) with the answer machinery of @c compileAnswersOneDPImpl (the
 * head is a state-level key carried in @c hval, completions are per-tuple in
 * @c done, and an answer is emitted -- at the gate already accumulating its
 * subtree's input literals -- when its head elements leave and no surviving
 * code can still witness it).
 */
UCQJointCompiler::AnswerCircuit compileAnswersOneDPCorrelated(
  const JointEncoding &enc, const UCQ &ucq, const QueryCtx &q,
  const HeadInfo &hi, UCQJointCompiler::Stats stats,
  unsigned max_treewidth, std::size_t max_states)
{
  UCQJointCompiler::AnswerCircuit result;
  dDNNF &dd = result.dd;

  const unsigned long E = enc.n_elements;
  const std::vector<SliceGate> &slice = enc.slice;
  const std::size_t D = q.disjuncts.size();
  auto isElem = [&](unsigned long v) { return v < E; };
  auto gidx = [&](unsigned long v) { return static_cast<std::size_t>(v - E); };

  // Width screen + decomposition of the joint graph (data + circuit slice).
  Graph graph = enc.buildGraph();
  unsigned max_degree = 0;
  if (TreeDecomposition::degeneracyLowerBound(graph, max_degree) > max_treewidth)
    throw TreeDecompositionException();
  std::unordered_map<unsigned long, bag_t> elim;
  const TreeDecomposition td(std::move(graph), &elim);
  if (td.getTreewidth() > max_treewidth)
    throw TreeDecompositionException();
  const std::size_t nb_bags = td.getNbBags();
  stats.joint_treewidth = td.getTreewidth();
  stats.nb_bags = nb_bags;
  auto bagIdx = [](bag_t b) {
                  return static_cast<std::underlying_type<bag_t>::type>(b);
                };

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
  std::vector<std::vector<std::size_t> > facts_at_bag(nb_bags);
  for (std::size_t fi = 0; fi < enc.facts.size(); ++fi) {
    const Fact &f = enc.facts[fi];
    std::vector<unsigned long> cl = f.elements;
    if (f.kind == FactGateKind::GATE)
      cl.push_back(E + f.gate);
    bag_t best = elim.at(cl[0]);
    for (unsigned long v : cl)
      if (bagIdx(elim.at(v)) < bagIdx(best))
        best = elim.at(v);
    facts_at_bag[bagIdx(best)].push_back(fi);
  }

  // Connected components of the joint graph (elements + gate vertices, joined
  // by each fact's clique).  An answer is settled -- safe to emit -- only when
  // its whole component has left the bag: a witness fact's gate vertex can
  // outlive the head element, and the input literal it folds in then is part
  // of the answer's provenance.  So we keep an answer open until no vertex of
  // its component remains, by when every component gate has folded into the
  // state gate.  Independent answers fall in separate components (no
  // co-carrying); correlated answers share one (and must be carried together).
  const unsigned long NV = E + slice.size();
  std::vector<unsigned long> uf(NV);
  for (unsigned long v = 0; v < NV; ++v) uf[v] = v;
  std::function<unsigned long(unsigned long)> ufind =
    [&](unsigned long x) { while (uf[x] != x) { uf[x] = uf[uf[x]]; x = uf[x]; } return x; };
  auto uunion = [&](unsigned long a, unsigned long b) { uf[ufind(a)] = ufind(b); };
  for (const Fact &f : enc.facts) {
    std::vector<unsigned long> cl = f.elements;
    if (f.kind == FactGateKind::GATE)
      cl.push_back(E + f.gate);
    for (std::size_t i = 1; i < cl.size(); ++i)
      uunion(cl[0], cl[i]);
  }

  // Gate emission (certified deterministic OR / decomposable AND).
  using Table = std::unordered_map<CState, gate_t, CStateHash>;
  using Accumulator = std::unordered_map<CState, std::vector<gate_t>, CStateHash>;
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
  auto orGates = [&](const std::vector<gate_t> &gs) {
                   if (gs.size() == 1) return gs[0];
                   gate_t o = dd.setGate(BooleanGate::OR);
                   dd.setInfo(o, DNNF_CERT_INFO);
                   for (gate_t c : gs) dd.addWire(o, c);
                   return o;
                 };
  auto finalize = [&](Accumulator &acc) {
                    Table t;
                    t.reserve(acc.size());
                    for (auto &e : acc)
                      t.emplace(e.first, orGates(e.second));
                    acc.clear();
                    stats.max_states = std::max(stats.max_states, t.size());
                    if (t.size() > max_states)
                      throw JointCompilerException(
                        "joint DP state space exceeds the per-node bound (" +
                        std::to_string(max_states) + ")");
                    return t;
                  };

  // Gate-valuation local consistency / strong-gate justification.
  auto almost = [&](const std::map<unsigned long, bool> &gv) {
                  for (const auto &[g, gval] : gv) {
                    const SliceGate &G = slice[gidx(g)];
                    for (unsigned c : G.children) {
                      auto it = gv.find(E + c);
                      if (it == gv.end()) continue;
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
  auto justify = [&](std::map<unsigned long, bool> &gv,
                     std::vector<unsigned long> &susp) {
                   std::vector<unsigned long> keep;
                   for (unsigned long g : susp) {
                     const SliceGate &G = slice[gidx(g)];
                     const bool gval = gv.at(g);
                     bool just = false;
                     for (unsigned c : G.children) {
                       auto it = gv.find(E + c);
                       if (it == gv.end()) continue;
                       const bool cval = it->second;
                       if (G.type == SliceGateType::OR && gval && cval) just = true;
                       if (G.type == SliceGateType::AND && !gval && !cval) just = true;
                       if (G.type == SliceGateType::NOT && cval != gval) just = true;
                     }
                     if (!just) keep.push_back(g);
                   }
                   susp = std::move(keep);
                 };
  auto addSusp = [](std::vector<unsigned long> &susp, unsigned long g) {
                   auto it = std::lower_bound(susp.begin(), susp.end(), g);
                   if (it == susp.end() || *it != g) susp.insert(it, g);
                 };

  std::map<std::vector<unsigned long>, std::vector<gate_t> > answer_acc;

  // Apply a bag's facts: a fact is present iff its slice gate is true in the
  // valuation; a present fact closes the disjuncts (completions -> done).
  auto applyFacts = [&](Table table, std::size_t b) {
                      for (std::size_t fi : facts_at_bag[b]) {
                        const Fact &f = enc.facts[fi];
                        Accumulator acc;
                        for (const auto &[St, g] : table) {
                          CHECK_FOR_INTERRUPTS();
                          const bool present =
                            f.kind == FactGateKind::CERTAIN ||
                            (St.gate_val.count(E + f.gate) &&
                             St.gate_val.at(E + f.gate));
                          CState ns = St;
                          if (present) {
                            std::vector<std::vector<unsigned long> > comp;
                            for (std::size_t d = 0; d < D; ++d)
                              closeDisjunctA(q.disjuncts[d], hi, ns.core.homs[d],
                                             f, edom[b], comp);
                            for (const auto &t : comp)
                              addDone(ns.core.done, t);
                          }
                          acc[std::move(ns)].push_back(g);
                        }
                        table = finalize(acc);
                      }
                      return table;
                    };

  // Lift to the parent: forget leaving gates (fold INPUT literals, kill an
  // unjustified strong gate), forget/remap elements on the core, EMIT settled
  // answers, then introduce the parent's fresh gates (enumerate values).
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
        std::map<unsigned long, bool> cur_gv = St.gate_val;
        std::vector<unsigned long> cur_susp = St.susp;
        gate_t cg = g;
        bool dead = false;
        for (unsigned long v : gforget) {
          if (slice[gidx(v)].type == SliceGateType::INPUT)
            cg = andGate(cg, St.gate_val.at(v) ? inGate(gidx(v)) : notGate(gidx(v)));
          else if (std::binary_search(St.susp.begin(), St.susp.end(), v)) {
            dead = true;
            break;
          }
          cur_gv.erase(v);
          auto it = std::lower_bound(cur_susp.begin(), cur_susp.end(), v);
          if (it != cur_susp.end() && *it == v)
            cur_susp.erase(it);
        }
        if (dead)
          continue;

        AState core = forgetLiftA(q, hi, St.core, ef, et);
        // Emit answers whose whole component has left @p to: then every
        // witness gate has folded into @c cg and no future fact can touch the
        // answer.  (The component of the head element subsumes both the
        // head-element-departure and the no-pending-witness tests.)
        std::vector<std::vector<unsigned long> > keep;
        for (const auto &tup : core.done) {
          const unsigned long cv = ufind(tup[0]);
          bool present = false;
          for (unsigned long u : to)
            if (ufind(u) == cv) { present = true; break; }
          if (!present)
            answer_acc[tup].push_back(cg);
          else
            keep.push_back(tup);
        }
        core.done = std::move(keep);

        std::vector<CState> sts;
        std::vector<gate_t> gs;
        sts.push_back(CState{std::move(core), std::move(cur_gv),
                             std::move(cur_susp)});
        gs.push_back(cg);
        for (unsigned long v : gintro) {
          std::vector<CState> ns;
          std::vector<gate_t> n2;
          for (std::size_t i = 0; i < sts.size(); ++i)
            for (int bv = 0; bv < 2; ++bv) {
              CState nv = sts[i];
              nv.gate_val[v] = static_cast<bool>(bv);
              if (slice[gidx(v)].type != SliceGateType::INPUT &&
                  isStrong(slice[gidx(v)].type, static_cast<bool>(bv)))
                addSusp(nv.susp, v);
              ns.push_back(std::move(nv));
              n2.push_back(gs[i]);
            }
          sts = std::move(ns);
          gs = std::move(n2);
        }
        for (std::size_t i = 0; i < sts.size(); ++i) {
          justify(sts[i].gate_val, sts[i].susp);
          if (!almost(sts[i].gate_val))
            continue;
          acc[std::move(sts[i])].push_back(gs[i]);
        }
      }
      return finalize(acc);
    };

  auto join = [&](const Table &t1, const Table &t2,
                  const std::vector<unsigned long> &edomain) {
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
                    CState nv;
                    nv.gate_val = A.gate_val;
                    for (const auto &[k, v] : B.gate_val)
                      nv.gate_val[k] = v;
                    for (unsigned long x : A.susp)
                      if (std::binary_search(B.susp.begin(), B.susp.end(), x))
                        nv.susp.push_back(x);
                    justify(nv.gate_val, nv.susp);
                    if (!almost(nv.gate_val))
                      continue;
                    nv.core = joinA(q, hi, edomain, A.core, B.core);
                    acc[std::move(nv)].push_back(andGate(ga, gb));
                  }
                return finalize(acc);
              };

  auto trivialTable = [&](const std::vector<unsigned long> &d) {
                        CState z;
                        z.core = trivialAState(q, hi);
                        Table e;
                        e.emplace(std::move(z), true_gate);
                        return lift(e, {}, d);
                      };

  // Bottom-up sweep.
  struct Frame {
    bag_t bag;
    std::size_t next_child = 0;
    Table table;
    bool has_table = false;
    explicit Frame(bag_t b) : bag(b) {}
  };
  Table root_table;
  std::size_t root_bag = bagIdx(td.getRoot());
  {
    std::vector<Frame> stack;
    stack.push_back(Frame(td.getRoot()));
    while (!stack.empty()) {
      Frame &frame = stack.back();
      const auto &children = td.getChildren(frame.bag);
      if (frame.next_child < children.size()) {
        stack.push_back(Frame(children[frame.next_child++]));
        continue;
      }
      CHECK_FOR_INTERRUPTS();
      const std::size_t b = bagIdx(frame.bag);
      Table table = frame.has_table ? std::move(frame.table) : trivialTable(dom[b]);
      table = applyFacts(std::move(table), b);
      if (stack.size() == 1) {
        root_table = std::move(table);
        stack.pop_back();
        break;
      }
      Frame &parent = stack[stack.size() - 2];
      const std::size_t pb = bagIdx(parent.bag);
      Table lifted = lift(table, dom[b], dom[pb]);
      if (!parent.has_table) {
        parent.table = std::move(lifted);
        parent.has_table = true;
      } else {
        parent.table = join(parent.table, lifted, edom[pb]);
      }
      stack.pop_back();
    }
  }
  // Forget the root domain: folds the remaining input literals and emits
  // every remaining answer.
  lift(root_table, dom[root_bag], {});

  // One root per discovered answer; the caller materialises / evaluates them.
  result.answers.reserve(answer_acc.size());
  for (auto &entry : answer_acc)
    result.answers.push_back(
      UCQJointCompiler::AnswerRoot{entry.first, orGates(entry.second)});
  result.max_states = stats.max_states;
  return result;
}

UCQJointCompiler::AnswerCircuit compileAnswersOneDPImpl(
  const JointEncoding &enc, const UCQ &ucq,
  const std::vector<unsigned> &head_vars,
  unsigned max_treewidth, std::size_t max_states)
{
  using Stats  = UCQJointCompiler::Stats;
  if (ucq.disjuncts.empty())
    throw JointCompilerException("empty UCQ");

  QueryCtx q;
  Stats stats;
  buildQueryCtx(ucq, enc, q, stats);
  HeadInfo hi = buildHeadInfo(q, head_vars);

  // Correlated regime (facts gated by internal circuit gates): the merged
  // valuation + answer DP over the joint data+circuit decomposition.
  if (enc.correlated)
    return compileAnswersOneDPCorrelated(enc, ucq, q, hi, std::move(stats),
                                         max_treewidth, max_states);

  // Width screen + decomposition of the data graph.
  Graph graph = enc.buildGraph();
  unsigned max_degree = 0;
  if (TreeDecomposition::degeneracyLowerBound(graph, max_degree) > max_treewidth)
    throw TreeDecompositionException();
  std::unordered_map<unsigned long, bag_t> elim;
  const TreeDecomposition td(std::move(graph), &elim);
  if (td.getTreewidth() > max_treewidth)
    throw TreeDecompositionException();
  const std::size_t nb_bags = td.getNbBags();
  stats.joint_treewidth = td.getTreewidth();
  stats.nb_bags = nb_bags;

  std::vector<std::vector<std::size_t> > facts_at_bag(nb_bags);
  for (std::size_t i = 0; i < enc.facts.size(); ++i) {
    const Fact &f = enc.facts[i];
    bag_t best = elim.at(f.elements[0]);
    for (unsigned long e : f.elements)
      if (bag_index(elim.at(e)) < bag_index(best))
        best = elim.at(e);
    facts_at_bag[bag_index(best)].push_back(i);
  }
  std::vector<std::vector<unsigned long> > domains(nb_bags);
  for (std::size_t b = 0; b < nb_bags; ++b) {
    auto &dom = domains[b];
    for (gate_t g : td.getBag(bag_t{b}))
      dom.push_back(static_cast<std::underlying_type<gate_t>::type>(g));
    std::sort(dom.begin(), dom.end());
    dom.erase(std::unique(dom.begin(), dom.end()), dom.end());
  }

  // Gate emission (identical discipline to compileImpl: deterministic OR,
  // decomposable AND, certified).
  UCQJointCompiler::AnswerCircuit result;
  dDNNF &dd = result.dd;
  using Table = std::unordered_map<AState, gate_t, AStateHash>;
  using Accumulator = std::unordered_map<AState, std::vector<gate_t>, AStateHash>;
  const gate_t invalid_gate{static_cast<std::underlying_type<gate_t>::type>(-1)};
  const gate_t true_gate = dd.setGate(BooleanGate::AND);
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
                   if (a == true_gate) return b;
                   if (b == true_gate) return a;
                   gate_t g = dd.setGate(BooleanGate::AND);
                   dd.setInfo(g, DNNF_CERT_INFO);
                   dd.addWire(g, a);
                   dd.addWire(g, b);
                   return g;
                 };
  auto orGates = [&](const std::vector<gate_t> &gs) {
                   if (gs.size() == 1)
                     return gs[0];
                   gate_t o = dd.setGate(BooleanGate::OR);
                   dd.setInfo(o, DNNF_CERT_INFO);
                   for (gate_t c : gs)
                     dd.addWire(o, c);
                   return o;
                 };
  auto finalize = [&](Accumulator &acc) {
                    Table t;
                    t.reserve(acc.size());
                    for (auto &entry : acc)
                      t.emplace(entry.first, orGates(entry.second));
                    acc.clear();
                    stats.max_states = std::max(stats.max_states, t.size());
                    if (t.size() > max_states)
                      throw JointCompilerException(
                        "joint DP state space exceeds the per-node bound (" +
                        std::to_string(max_states) + ")");
                    return t;
                  };

  // Per-answer accumulated roots, filled when a head tuple leaves the tree.
  std::map<std::vector<unsigned long>, std::vector<gate_t> > answer_acc;

  const AState kTrivial = trivialAState(q, hi);
  auto trivialTable = [&]() { return Table{{kTrivial, true_gate}}; };
  auto isTrivial = [&](const Table &t) {
                     return t.size() == 1 && t.begin()->second == true_gate &&
                            t.begin()->first == kTrivial;
                   };

  auto applyFacts = [&](Table table, std::size_t b) {
                      const auto &domain = domains[b];
                      for (std::size_t fi : facts_at_bag[b]) {
                        const Fact &fact = enc.facts[fi];
                        Accumulator acc;
                        for (const auto &entry : table) {
                          CHECK_FOR_INTERRUPTS();
                          AState present =
                            closeWithFactA(q, hi, entry.first, fact, domain);
                          if (fact.kind == FactGateKind::CERTAIN) {
                            acc[std::move(present)].push_back(entry.second);
                          } else if (present == entry.first) {
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

  // Lift a child table to the parent domain.  An answer is EMITTED (its
  // provenance is then final) when all its head elements have left AND no
  // surviving partial code still witnesses it -- a code committed to the
  // answer's head value (via a forgotten-element @c hval) can complete the
  // answer in a higher bag, so we must keep it open until no such code
  // remains.  Emitting on element-departure alone would split one answer's
  // provenance across the lifts of its several witnesses (over-counting).
  auto lift = [&](const Table &t, const std::vector<unsigned long> &from,
                  const std::vector<unsigned long> &to) {
                if (from == to)
                  return t;
                Accumulator acc;
                for (const auto &entry : t) {
                  AState ls = forgetLiftA(q, hi, entry.first, from, to);
                  std::vector<std::vector<unsigned long> > keep;
                  for (const auto &tup : ls.done) {
                    bool gone = true;
                    for (unsigned long e : tup)
                      if (std::binary_search(to.begin(), to.end(), e)) {
                        gone = false;
                        break;
                      }
                    // At the root (empty parent) no future fact can witness
                    // anything, so a code still binding the tuple is a dead
                    // end, not a pending witness: emit unconditionally.
                    bool pending = false;
                    if (gone && !to.empty())
                      for (const auto &codes : ls.homs) {
                        for (const ADCode &c : codes)
                          if (bindsTuple(c, hi, tup, to)) { pending = true; break; }
                        if (pending) break;
                      }
                    if (gone && !pending)
                      answer_acc[tup].push_back(entry.second);
                    else
                      keep.push_back(tup);
                  }
                  ls.done = std::move(keep);
                  acc[std::move(ls)].push_back(entry.second);
                }
                return finalize(acc);
              };

  auto joinTables = [&](const Table &t1, const Table &t2,
                        const std::vector<unsigned long> &domain) {
                      if (isTrivial(t1)) return t2;
                      if (isTrivial(t2)) return t1;
                      Accumulator acc;
                      for (const auto &l : t1)
                        for (const auto &r : t2) {
                          CHECK_FOR_INTERRUPTS();
                          acc[joinA(q, hi, domain, l.first, r.first)].push_back(
                            andGate(l.second, r.second));
                        }
                      return finalize(acc);
                    };

  // Bottom-up sweep.
  struct Frame {
    bag_t bag;
    std::size_t next_child = 0;
    Table table;
    bool has_table = false;
    explicit Frame(bag_t b) : bag(b) {}
  };
  Table root_table;
  std::size_t root_bag = bag_index(td.getRoot());
  {
    std::vector<Frame> stack;
    stack.push_back(Frame(td.getRoot()));
    while (!stack.empty()) {
      Frame &frame = stack.back();
      const auto &children = td.getChildren(frame.bag);
      if (frame.next_child < children.size()) {
        stack.push_back(Frame(children[frame.next_child++]));
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
        parent.table = joinTables(parent.table, lifted, domains[pb]);
      }
      stack.pop_back();
    }
  }
  // Forget the root domain: emits every remaining answer.
  lift(root_table, domains[root_bag], {});

  // One root per discovered answer in the shared circuit; the caller
  // materialises / evaluates them (the gate cache then shares work).
  result.answers.reserve(answer_acc.size());
  for (auto &entry : answer_acc)
    result.answers.push_back(
      UCQJointCompiler::AnswerRoot{entry.first, orGates(entry.second)});
  result.max_states = stats.max_states;
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
  // merged valuation + homomorphism DP.  The single-sweep per-answer path
  // (shared_td / shared_elim / head_pin) is supported here too: the joint
  // graph spans data and circuit, is built once, and the head is pinned in
  // the DP rather than by a Sel atom (which would change the encoding).
  if (enc.correlated)
    return mergedCompile(enc, q, stats, max_treewidth, max_states,
                         shared_td, shared_elim, head_pin);

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

  // Build the joint graph + tree decomposition ONCE; the head pin keeps the
  // encoding and decomposition identical across answers.  This holds in both
  // regimes: the data-graph joint graph is the data graph, the correlated
  // joint graph spans data and circuit slice -- neither depends on the head.
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

UCQJointCompiler::AnswerCircuit UCQJointCompiler::compileAnswersOneDP(
  const JointEncoding &enc,
  const UCQ &ucq,
  const std::vector<unsigned> &head_vars,
  unsigned max_treewidth,
  std::size_t max_states)
{
  return compileAnswersOneDPImpl(enc, ucq, head_vars, max_treewidth,
                                 max_states);
}
