/**
 * @file mobius_evaluate.cpp
 * @brief Möbius-inversion exact route for safe UCQs (the last missing exact
 *        route of the Dalvi-Suciu dichotomy).
 *
 * Some unions of conjunctive queries are safe (PTIME data complexity) only
 * because the #P-hard terms of their inclusion-exclusion expansion carry a
 * zero Möbius value on the CNF lattice and cancel.  The canonical witness is
 * QW / q9 (Dalvi-Suciu 2012; Monet & Olteanu 2018).  No other ProvSQL route
 * handles it in PTIME: the safe-query rewriter is per-CQ and hierarchical, the
 * query is not inversion-free (that is the point), and on adversarial data the
 * joint treewidth is unbounded.
 *
 * This file packages the **extensional** lattice-walking algorithm (Dalvi,
 * Schnaitter & Suciu, "Computing query probability with incidence algebras",
 * PODS 2010) the way the joint-width route already is: a compile-at-execution
 * step producing a certified circuit over the gathered data, and a linear
 * evaluation -- the only genuinely new evaluation primitive being a signed
 * linear combination at @c gate_mobius nodes (see @c provsql_utils.h and the
 * @c gate_mobius handling in @c probability_evaluate.cpp).
 *
 * The probability of a UCQ Q given in CNF as @f$\bigwedge_i d_i@f$ is, by
 * inclusion-exclusion on the @f$\lnot d_i@f$,
 * @f[ P(Q) = \sum_{\emptyset\neq s\subseteq[M]} (-1)^{|s|+1}
 *           P\Big(\bigvee_{i\in s} d_i\Big), @f]
 * and grouping the @f$\bigvee_{i\in s} d_i@f$ up to logical equivalence
 * collapses the hard term (its coefficient sums to zero -- the whole game).
 * Each surviving @f$\bigvee_{i\in s} d_i@f$ is a safe disjunctive sentence,
 * compiled recursively by the standard IndepStep / MobiusStep lifted-inference
 * recursion (component split, disjoint-symbol product, separator
 * independent-project, inner Möbius step) into certified-independent Boolean
 * islands, combined at the root @c gate_mobius by the signed coefficients.
 *
 * v1 restrictions (see doc/TODO/mobius.md): reduced-form UCQs (no relation
 * repeated within a disjunct, no repeated variable inside an atom, no
 * constants), tuple-independent (TID) inputs.  Anything outside that declines
 * (a C++ exception caught at the SQL boundary), so the query falls back to the
 * normal provenance and never fails.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/resowner.h"
#include "utils/uuid.h"

#include "provsql_utils.h"
#include "provsql_mmap.h"

/* Store-backed gate-type lookup (defined in provsql_mmap.c); used to enforce
 * the TID restriction (G3) -- every fact token must be a bare gate_input. */
extern Datum get_gate_type(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ucq_mobius_materialize_tracked);
PG_FUNCTION_INFO_V1(ucq_mobius_compile_stats);
PG_FUNCTION_INFO_V1(ucq_mobius_provenance_answer);
}

#include "c_cpp_compatibility.h"
#include "CertifiedDDMaterialize.h"
#include "mobius_evaluate.h"
#include "provsql_utils_cpp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

/** @brief Thrown when the Möbius route declines (unsafe shape, cap hit, ...). */
class MobiusDecline : public std::runtime_error {
public:
  explicit MobiusDecline(const std::string &w) : std::runtime_error(w) {}
};

/** @brief One argument position of an atom: a variable or a bound constant. */
struct Term {
  bool isVar;
  long v;        ///< Variable id (isVar) or constant element-id (!isVar).
  bool operator==(const Term &o) const { return isVar==o.isVar && v==o.v; }
};

/** @brief A relational atom: a relation symbol applied to terms. */
struct MAtom {
  unsigned rel;
  std::vector<Term> args;
};

using Disjunct = std::vector<MAtom>;   ///< A conjunction of atoms.
using Sentence = std::vector<Disjunct>;///< A disjunction of conjunctions (UCQ).

// ---------------------------------------------------------------------------
// Statistics surfaced by the stats SRF (the demonstrability requirement).
// ---------------------------------------------------------------------------
struct MobiusStats {
  int n_components = 0;       ///< distinct connected-component literals at top
  int n_cnf_conjuncts = 0;    ///< M, the CNF conjunct count at the top level
  int lattice_size = 0;       ///< 2^M subsets enumerated at the top level
  int lattice_collapsed = 0;  ///< distinct elements after equivalence collapse
  int n_nonzero = 0;          ///< elements with coefficient != 0
  int n_cancelled = 0;        ///< distinct elements with coefficient == 0
  bool cancelled_hard = false;///< some cancelled element is #P-hard (no separator)
  long dd_size = 0;           ///< gates materialised
  long memo_hits = 0;         ///< sentence-memo hits
  double probability = 0.0;   ///< P(Q)
};

// ===========================================================================
// Homomorphisms (CQ containment) over single-component conjunctions.
// ===========================================================================

/// Backtracking homomorphism search: does a mapping of @p p's variables to
/// @p q's terms exist sending every @p p-atom onto a @p q-atom (constants
/// matched verbatim)?  @p p, @p q are tiny (reduced-form components), so the
/// naive search is cheap.  Returns true iff @p p ⊑ @p q (i.e. @p q implies
/// @p p as a query -- a hom p->q means every model of p is a model of q is the
/// usual direction; here hom(p,q) maps p's atoms into q, witnessing q ⊑ p).
bool homExists(const Disjunct &p, const Disjunct &q,
               std::size_t ai, std::map<long,Term> &asg)
{
  if(ai == p.size())
    return true;
  const MAtom &pa = p[ai];
  for(const MAtom &qa : q) {
    if(qa.rel != pa.rel || qa.args.size() != pa.args.size())
      continue;
    std::vector<long> undo;
    bool ok = true;
    for(std::size_t k=0; k<pa.args.size(); ++k) {
      const Term &pt = pa.args[k];
      const Term &qt = qa.args[k];
      if(!pt.isVar) {
        if(qt.isVar || qt.v != pt.v) { ok=false; break; }
      } else {
        auto it = asg.find(pt.v);
        if(it == asg.end()) { asg[pt.v]=qt; undo.push_back(pt.v); }
        else if(!(it->second == qt)) { ok=false; break; }
      }
    }
    if(ok && homExists(p, q, ai+1, asg))
      return true;
    for(long u : undo) asg.erase(u);
  }
  return false;
}

/// hom from @p p into @p q (maps p's atoms onto q's): witnesses q ⊑ p.
bool hom(const Disjunct &p, const Disjunct &q)
{
  std::map<long,Term> asg;
  return homExists(p, q, 0, asg);
}

/// Logical equivalence of two single-component conjunctions (self-join-free
/// components are cores, so this is isomorphism).
bool equiv(const Disjunct &p, const Disjunct &q)
{
  return hom(p,q) && hom(q,p);
}

// ===========================================================================
// Connected components.
// ===========================================================================

struct UF {
  std::vector<int> p;
  explicit UF(int n) : p(n) { for(int i=0;i<n;++i) p[i]=i; }
  int find(int x){ while(p[x]!=x){ p[x]=p[p[x]]; x=p[x]; } return x; }
  void uni(int a,int b){ p[find(a)]=find(b); }
};

/// Split a conjunction into its variable-connected components.  (Reduced
/// form: distinct components use disjoint relation symbols, so they are
/// probabilistically independent.)
std::vector<Disjunct> componentsOf(const Disjunct &d)
{
  const int n = static_cast<int>(d.size());
  UF uf(n);
  std::map<long, int> firstAtomOfVar;
  for(int i=0;i<n;++i)
    for(const auto &t : d[i].args)
      if(t.isVar) {
        auto it = firstAtomOfVar.find(t.v);
        if(it==firstAtomOfVar.end()) firstAtomOfVar[t.v]=i;
        else uf.uni(it->second, i);
      }
  std::map<int, Disjunct> byroot;
  for(int i=0;i<n;++i)
    byroot[uf.find(i)].push_back(d[i]);
  std::vector<Disjunct> out;
  for(auto &kv : byroot) out.push_back(std::move(kv.second));
  return out;
}

/// Is an atom fully ground (no variables)?
bool ground(const MAtom &a)
{
  for(const auto &t : a.args) if(t.isVar) return false;
  return true;
}

// ===========================================================================
// Data index (TID facts).
// ===========================================================================

struct FactIndex {
  // (rel, element tuple) -> provenance token (UUID string).  A certain
  // (untracked) fact has an empty token.
  std::map<std::pair<unsigned, std::vector<long>>, std::string> tok;
  std::set<std::pair<unsigned, std::vector<long>>> present;
  // (rel, position) -> active domain (distinct values seen there).
  std::map<std::pair<unsigned,int>, std::set<long>> domain;

  bool isPresent(unsigned rel, const std::vector<long> &el) const {
    return present.count({rel, el}) > 0;
  }
  const std::string *token(unsigned rel, const std::vector<long> &el) const {
    auto it = tok.find({rel, el});
    return it==tok.end() ? nullptr : &it->second;
  }
};

// ===========================================================================
// The compiler.
// ===========================================================================

class MobiusCompiler {
public:
  MobiusCompiler(const FactIndex &fi, MobiusStats &st) : fi(fi), st(st) {}

  /// Compile the top sentence, returning the root token.  @p lineage is the
  /// token of the literal Boolean provenance of the query (the normal lineage
  /// the route falls back to); it is carried on the root gate_mobius as a
  /// designated transparent child so every non-probability evaluator (semiring,
  /// Shapley, Banzhaf, PROV export) sees the literal lineage and works, while
  /// probability uses the signed Möbius combination.  Empty = no lineage
  /// (measure-only; the manual descriptor entry points without a fallback).
  pg_uuid_t compileTop(const Sentence &s, const std::string &lineage = "") {
    pending_lineage = lineage;
    lineage_consumed = false;
    pg_uuid_t root = compile(s, /*top=*/true);
    if(!lineage.empty() && !lineage_consumed)
      // The top did not go through a Möbius step (e.g. a safe query that
      // reached this route): wrap it in a thin gate_mobius selector carrying
      // the lineage and the value with coefficient 1.
      root = mkMobius({root}, {1}, lineage);
    return root;
  }

private:
  const FactIndex &fi;
  MobiusStats &st;
  std::unordered_map<std::string, std::string> memo;  // key -> uuid string
  std::unordered_set<std::string> created;
  std::string pending_lineage;   // lineage to inline into the top Möbius step
  bool lineage_consumed = false;

  // -- materialisation -----------------------------------------------------

  pg_uuid_t mkConst(bool one) {
    pg_uuid_t u = provsqlUuidV5(one ? "one" : "zero");
    if(created.insert(uuid2string(u)).second) {
      provsql_internal_create_gate(&u, one?gate_one:gate_zero, 0, NULL);
      ++st.dd_size;
    }
    return u;
  }

  /// Independent OR / AND over child tokens (deduped).  An empty AND is one,
  /// an empty OR is zero; a single child is returned as-is.
  pg_uuid_t mkBool(bool isAnd, std::vector<pg_uuid_t> ch) {
    std::vector<std::string> texts;
    texts.reserve(ch.size());
    for(const auto &c : ch) texts.push_back(uuid2string(c));
    std::sort(texts.begin(), texts.end());
    texts.erase(std::unique(texts.begin(), texts.end()), texts.end());
    if(texts.empty())
      return mkConst(isAnd);
    if(texts.size()==1)
      return string2uuid(texts[0]);
    std::string name = (isAnd ? "times{" : "plus{");
    for(std::size_t i=0;i<texts.size();++i){ if(i) name+=","; name+=texts[i]; }
    name += "}";
    pg_uuid_t u = provsqlUuidV5(name);
    if(created.insert(uuid2string(u)).second) {
      std::vector<pg_uuid_t> children;
      children.reserve(texts.size());
      for(const auto &t : texts) children.push_back(string2uuid(t));
      provsql_internal_create_gate(&u, isAnd?gate_times:gate_plus,
                                   static_cast<unsigned>(children.size()),
                                   children.data());
      ++st.dd_size;
    }
    return u;
  }

  /// Signed Möbius combination gate over @p children with integer @p coeffs.
  /// Coefficients are stored keyed by child UUID (@c "uuid:coeff" tokens in
  /// @c extra), so evaluation is robust to any child reordering / dedup the
  /// store may apply; duplicate children are merged (coefficients summed) and
  /// zero-coefficient children dropped.
  /// @p lineage (optional): the literal-lineage token, carried as a
  /// designated transparent child marked @c "L:<uuid>" in @c extra (child 0).
  pg_uuid_t mkMobius(const std::vector<pg_uuid_t> &children,
                     const std::vector<long> &coeffs,
                     const std::string &lineage = "") {
    std::map<std::string,long> bycoeff;
    std::vector<std::string> order;
    for(std::size_t i=0;i<children.size();++i) {
      const std::string u = uuid2string(children[i]);
      if(!bycoeff.count(u)) order.push_back(u);
      bycoeff[u] += coeffs[i];
    }
    std::vector<pg_uuid_t> ch;
    std::string extra, name = "mobius[";
    if(!lineage.empty()) {
      ch.push_back(string2uuid(lineage));     // the literal lineage, child 0
      extra += "L:" + lineage + " ";
      name  += "L:" + lineage + ",";
    }
    for(const std::string &u : order) {
      if(bycoeff[u]==0) continue;
      ch.push_back(string2uuid(u));
      extra += u + ":" + std::to_string(bycoeff[u]) + " ";
      name  += u + ":" + std::to_string(bycoeff[u]) + ",";
    }
    name += "]";
    if(ch.empty() || (!lineage.empty() && ch.size()==1))
      return mkConst(false);   // no surviving combination -> probability 0
    pg_uuid_t u = provsqlUuidV5(name);
    if(created.insert(uuid2string(u)).second) {
      provsql_internal_create_gate(&u, gate_mobius,
                                   static_cast<unsigned>(ch.size()),
                                   ch.data());
      provsql_internal_set_extra(&u, extra.c_str());
      ++st.dd_size;
    }
    return u;
  }

  // -- canonical key (structural, for memoisation) -------------------------

  std::string canonDisjunct(const Disjunct &d) const {
    // Canonically rename variables by first occurrence in a sorted atom order.
    // Build per-atom signatures with placeholder variable slots, then pick the
    // lexicographically smallest atom ordering greedily (sufficient: keys only
    // need same-structure -> same-key, not minimality).
    std::vector<std::string> atomsigRaw;
    for(const auto &a : d) {
      std::string sg = "r" + std::to_string(a.rel) + "(";
      for(const auto &t : a.args) {
        if(t.isVar) sg += "v"; else sg += "c"+std::to_string(t.v);
        sg += ",";
      }
      sg += ")";
      atomsigRaw.push_back(sg);
    }
    // Sort atoms by raw signature, then assign canonical var ids in first
    // occurrence order over that sorted sequence.
    std::vector<int> order(d.size());
    for(std::size_t i=0;i<d.size();++i) order[i]=static_cast<int>(i);
    std::sort(order.begin(), order.end(),
              [&](int a,int b){ return atomsigRaw[a]<atomsigRaw[b]; });
    std::map<long,int> ren;
    std::string out;
    for(int idx : order) {
      const MAtom &a = d[idx];
      out += "r"+std::to_string(a.rel)+"(";
      for(const auto &t : a.args) {
        if(t.isVar) {
          auto it = ren.find(t.v);
          int id = (it==ren.end()) ? (ren[t.v]=static_cast<int>(ren.size())) : it->second;
          out += "v"+std::to_string(id);
        } else out += "c"+std::to_string(t.v);
        out += ",";
      }
      out += ")";
    }
    return out;
  }

  std::string canonSentence(const Sentence &s) const {
    std::vector<std::string> parts;
    for(const auto &d : s) parts.push_back(canonDisjunct(d));
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    std::string out;
    for(const auto &p : parts){ out += p; out += "|"; }
    return out;
  }

  // -- recursion -----------------------------------------------------------

  pg_uuid_t compile(const Sentence &sentence, bool top=false);

  /// Find a separator: a unification class of variable positions present in
  /// every atom of every disjunct.  Returns the (rel,pos) occurrences of the
  /// class via @p occ and per-disjunct the variable ids of the class via
  /// @p classVarsPerDisj; returns false if no separator exists.
  bool findSeparator(const Sentence &s,
                     std::set<std::pair<unsigned,int>> &occ,
                     std::vector<std::set<long>> &classVarsPerDisj);

  /// Möbius (inclusion-exclusion) step over the components of @p s.
  pg_uuid_t mobiusStep(const Sentence &s, bool top);
};

// --- separator -------------------------------------------------------------

bool MobiusCompiler::findSeparator(const Sentence &s,
    std::set<std::pair<unsigned,int>> &occOut,
    std::vector<std::set<long>> &classVarsPerDisj)
{
  // Collect all variable ids and union those sharing a (rel,pos).
  std::map<long,int> id;
  auto vid = [&](long v)->int{
    auto it=id.find(v); if(it!=id.end()) return it->second;
    int n=static_cast<int>(id.size()); id[v]=n; return n;
  };
  for(const auto &d : s) for(const auto &a : d) for(const auto &t : a.args)
    if(t.isVar) vid(t.v);
  if(id.empty()) return false;
  UF uf(static_cast<int>(id.size()));
  std::map<std::pair<unsigned,int>, long> firstAtPos;
  for(const auto &d : s) for(const auto &a : d)
    for(std::size_t k=0;k<a.args.size();++k) {
      if(!a.args[k].isVar) continue;
      auto key = std::make_pair(a.rel, static_cast<int>(k));
      auto it = firstAtPos.find(key);
      if(it==firstAtPos.end()) firstAtPos[key]=a.args[k].v;
      else uf.uni(vid(it->second), vid(a.args[k].v));
    }
  // Candidate classes = class roots.  A class is a separator iff in every
  // disjunct, every atom contains a variable of that class.
  std::vector<long> idToVar(id.size());
  for(auto &kv : id) idToVar[kv.second]=kv.first;

  std::set<int> roots;
  for(int i=0;i<static_cast<int>(id.size());++i) roots.insert(uf.find(i));

  for(int root : roots) {
    bool covers = true;
    std::vector<std::set<long>> cvars(s.size());
    for(std::size_t di=0; di<s.size() && covers; ++di) {
      const Disjunct &d = s[di];
      for(const auto &a : d) {
        bool atomHas=false;
        for(const auto &t : a.args)
          if(t.isVar && uf.find(vid(t.v))==root) {
            atomHas=true; cvars[di].insert(t.v);
          }
        if(!atomHas){ covers=false; break; }
      }
    }
    if(!covers) continue;
    // Found.  Record the (rel,pos) occurrences of this class for the domain.
    occOut.clear();
    for(const auto &d : s) for(const auto &a : d)
      for(std::size_t k=0;k<a.args.size();++k)
        if(a.args[k].isVar && uf.find(vid(a.args[k].v))==root)
          occOut.insert({a.rel, static_cast<int>(k)});
    classVarsPerDisj = std::move(cvars);
    return true;
  }
  return false;
}

// --- Möbius step -----------------------------------------------------------

pg_uuid_t MobiusCompiler::mobiusStep(const Sentence &s, bool top)
{
  // 1. Components of each disjunct -> a global literal pool with equivalence
  //    collapse.  Each disjunct becomes the set of its component literal ids.
  std::vector<Disjunct> literals;          // representative component per id
  auto litId = [&](const Disjunct &c)->int{
    for(std::size_t i=0;i<literals.size();++i)
      if(equiv(literals[i], c)) return static_cast<int>(i);
    literals.push_back(c);
    return static_cast<int>(literals.size()-1);
  };
  std::vector<std::set<int>> terms;        // DNF terms (per disjunct)
  for(const auto &d : s) {
    std::set<int> t;
    for(const auto &c : componentsOf(d)) t.insert(litId(c));
    terms.push_back(std::move(t));
  }
  if(top) st.n_components = static_cast<int>(literals.size());

  // Implication matrix on literals: imp[i][j] == (lit_i implies lit_j),
  // i.e. lit_i ⊑ lit_j, witnessed by hom(lit_j -> lit_i).
  const int L = static_cast<int>(literals.size());
  std::vector<std::vector<char>> imp(L, std::vector<char>(L, 0));
  for(int i=0;i<L;++i) for(int j=0;j<L;++j)
    imp[i][j] = hom(literals[j], literals[i]) ? 1 : 0;

  // Minimise a disjunction (literal set): drop lit i if some other lit j in
  // the set is implied-BY i (i ⊑ j), since i ∨ j ≡ j.  Keep the weakest.
  auto minimiseDisj = [&](std::set<int> in)->std::vector<int>{
    std::vector<int> v(in.begin(), in.end());
    std::vector<char> keep(v.size(),1);
    for(std::size_t a=0;a<v.size();++a) for(std::size_t b=0;b<v.size();++b)
      if(a!=b && keep[b] && imp[v[a]][v[b]]) { keep[a]=0; break; }
    std::vector<int> out;
    for(std::size_t a=0;a<v.size();++a) if(keep[a]) out.push_back(v[a]);
    std::sort(out.begin(), out.end());
    return out;
  };

  // 2. DNF -> CNF: a clause picks one literal from each term and ORs them.
  //    Cap the conjunct count.
  std::set<std::vector<int>> clauseSet;
  std::vector<std::set<int>> clauses;  // working as literal sets
  // Enumerate transversals.
  std::vector<int> pick(terms.size(), 0);
  // Bound the product to avoid blow-up before the M cap is even computed.
  unsigned long prod = 1;
  for(const auto &t : terms) prod *= std::max<std::size_t>(1, t.size());
  if(prod > 100000UL)
    throw MobiusDecline("Möbius: DNF->CNF transversal count too large");
  std::function<void(std::size_t,std::set<int>&)> gen =
    [&](std::size_t ti, std::set<int> &acc){
      if(ti==terms.size()){
        std::vector<int> mn = minimiseDisj(acc);
        if(!mn.empty()) clauseSet.insert(mn);
        return;
      }
      for(int lit : terms[ti]) {
        bool fresh = acc.insert(lit).second;
        gen(ti+1, acc);
        if(fresh) acc.erase(lit);
      }
    };
  { std::set<int> acc; gen(0, acc); }

  // Clause subsumption: drop clause B if some clause A ⊑ B (every literal of
  // A is implied by some literal of B... no: ⋁A implies ⋁B iff every a∈A has
  // a b∈B with a ⊑ b).  Then B is redundant in the conjunction.
  std::vector<std::vector<int>> cl(clauseSet.begin(), clauseSet.end());
  auto disjImplies = [&](const std::vector<int>&A, const std::vector<int>&B){
    for(int a : A){ bool ok=false; for(int b : B) if(imp[a][b]){ ok=true; break; }
                    if(!ok) return false; }
    return true;
  };
  std::vector<char> keepCl(cl.size(),1);
  for(std::size_t a=0;a<cl.size();++a) for(std::size_t b=0;b<cl.size();++b)
    if(a!=b && keepCl[a] && cl[a]!=cl[b] && disjImplies(cl[a],cl[b]))
      keepCl[b]=0;
  std::vector<std::vector<int>> M;
  for(std::size_t a=0;a<cl.size();++a) if(keepCl[a]) M.push_back(cl[a]);

  const int m = static_cast<int>(M.size());
  if(top) { st.n_cnf_conjuncts = m; st.lattice_size = (m<=30)?(1<<m):0; }
  if(m < 2)
    throw MobiusDecline("Möbius: no inclusion-exclusion structure (M<2); "
                        "sentence has no separator and does not decompose");
  if(m > 8)
    throw MobiusDecline("Möbius: CNF conjunct count exceeds the cap (M>8)");

  // 3. Enumerate non-empty subsets s of [m]; ψ_s = ⋁ of literals across the
  //    clauses in s, minimised.  Accumulate coefficient (-1)^{|s|+1} per
  //    distinct ψ_s (keyed by its minimised literal-id set).
  std::map<std::vector<int>, long> coeff;
  for(unsigned mask=1; mask < (1u<<m); ++mask) {
    std::set<int> lits;
    for(int i=0;i<m;++i) if(mask & (1u<<i))
      lits.insert(M[i].begin(), M[i].end());
    std::vector<int> key = minimiseDisj(lits);
    const int pc = __builtin_popcount(mask);
    coeff[key] += (pc & 1) ? 1 : -1;
  }
  if(top) st.lattice_collapsed = static_cast<int>(coeff.size());

  // 4. Build the gate_mobius over the surviving elements.  Each element is a
  //    pure disjunction of its literal components.
  std::vector<pg_uuid_t> children;
  std::vector<long> coeffs;
  int cancelled = 0;
  bool cancelledHard = false;
  for(const auto &kv : coeff) {
    if(kv.second == 0) {
      ++cancelled;
      // Is this cancelled element #P-hard (no separator, no decomposition)?
      // Probe cheaply: a pure disjunction of these literals with shared
      // symbols and no separator is the hard term whose cancellation makes
      // the query tractable.
      if(!cancelledHard) {
        Sentence el;
        for(int lid : kv.first) el.push_back(literals[lid]);
        std::set<std::pair<unsigned,int>> occ;
        std::vector<std::set<long>> cv;
        // Symbol-connected and no separator => hard.
        if(el.size() >= 2 && !findSeparator(el, occ, cv)) {
          // check symbol-connected
          UF uf(static_cast<int>(el.size()));
          std::map<unsigned,int> firstRelDisj;
          for(std::size_t i=0;i<el.size();++i)
            for(const auto &a : el[i])
              { auto it=firstRelDisj.find(a.rel);
                if(it==firstRelDisj.end()) firstRelDisj[a.rel]=static_cast<int>(i);
                else uf.uni(it->second, static_cast<int>(i)); }
          std::set<int> r; for(std::size_t i=0;i<el.size();++i) r.insert(uf.find(static_cast<int>(i)));
          if(r.size()==1) cancelledHard = true;
        }
      }
      continue;
    }
    Sentence el;
    for(int lid : kv.first) el.push_back(literals[lid]);
    children.push_back(compile(el, false));
    coeffs.push_back(kv.second);
  }
  if(top) { st.n_cancelled = cancelled; st.cancelled_hard = cancelledHard;
            st.n_nonzero = static_cast<int>(children.size()); }

  if(children.empty())
    return mkConst(false);
  // The top-level Möbius step inlines the literal lineage onto its own gate so
  // the root gate_mobius carries it directly (single level); nested steps do
  // not (their values are discarded by the transparent-to-lineage passthrough).
  if(top && !pending_lineage.empty()) {
    lineage_consumed = true;
    return mkMobius(children, coeffs, pending_lineage);
  }
  return mkMobius(children, coeffs);
}

// --- main recursion --------------------------------------------------------

pg_uuid_t MobiusCompiler::compile(const Sentence &sentence0, bool top)
{
  CHECK_FOR_INTERRUPTS();

  // 1. Drop disjuncts containing an ABSENT ground atom (the conjunction is
  //    then FALSE).  Present ground atoms are kept: under TID a tuple in the
  //    relation is a probabilistic Bernoulli event (its input token), NOT a
  //    certainty, so it stays an atom and bottoms out as a token leaf.
  Sentence s;
  for(const Disjunct &d0 : sentence0) {
    bool dead = false;
    for(const MAtom &a : d0) {
      if(ground(a)) {
        std::vector<long> el;
        for(const auto &t : a.args) el.push_back(t.v);
        if(!fi.isPresent(a.rel, el)) { dead=true; break; }
      }
    }
    if(!dead && !d0.empty()) s.push_back(d0);
  }
  if(s.empty())
    return mkConst(false);

  // Base case: a single fully-ground disjunct is a conjunction of tuples ->
  // the independent AND of their input tokens (a certain / untracked tuple
  // contributes the identity gate_one).
  auto isGround = [&](const Disjunct &d){
    for(const auto &a : d) if(!ground(a)) return false;
    return true;
  };
  auto tokenAnd = [&](const Disjunct &d)->pg_uuid_t {
    std::vector<pg_uuid_t> toks;
    for(const auto &a : d) {
      std::vector<long> el; for(const auto &t : a.args) el.push_back(t.v);
      const std::string *tk = fi.token(a.rel, el);
      if(tk==nullptr) return mkConst(false);     // absent (shouldn't reach here)
      if(tk->empty()) continue;                  // certain fact: identity for AND
      toks.push_back(string2uuid(*tk));
    }
    return mkBool(true, toks);                    // empty -> gate_one (all certain)
  };

  // Memoisation.
  const std::string key = canonSentence(s);
  {
    auto it = memo.find(key);
    if(it!=memo.end()) { ++st.memo_hits; return string2uuid(it->second); }
  }

  pg_uuid_t result;

  // 2. Independent union: split disjuncts into relation-symbol-connected
  //    groups (disjoint vocabulary => independent).
  {
    const int n = static_cast<int>(s.size());
    UF uf(n);
    std::map<unsigned,int> firstDisjWithRel;
    for(int i=0;i<n;++i)
      for(const auto &a : s[i]) {
        auto it=firstDisjWithRel.find(a.rel);
        if(it==firstDisjWithRel.end()) firstDisjWithRel[a.rel]=i;
        else uf.uni(it->second, i);
      }
    std::map<int, Sentence> groups;
    for(int i=0;i<n;++i) groups[uf.find(i)].push_back(s[i]);
    if(groups.size() > 1) {
      std::vector<pg_uuid_t> ch;
      for(auto &kv : groups) ch.push_back(compile(kv.second, false));
      result = mkBool(false, ch);
      memo[key] = uuid2string(result);
      return result;
    }
  }

  // 3. Single symbol-connected group.
  if(s.size()==1) {
    // Fully ground -> AND of tuple tokens (base case).
    if(isGround(s[0])) {
      result = tokenAnd(s[0]);
      memo[key] = uuid2string(result);
      return result;
    }
    // One disjunct (a CQ): split into variable-connected components and AND
    // them.  Independence of the product needs the components to use disjoint
    // relation symbols (reduced form); a shared symbol is a within-disjunct
    // self-join (or a ground/var correlation on one relation) -- decline (that
    // is Increment 3, ranking/shattering).
    std::vector<Disjunct> comps = componentsOf(s[0]);
    if(comps.size() > 1) {
      for(std::size_t i=0;i<comps.size();++i)
        for(std::size_t j=i+1;j<comps.size();++j) {
          std::set<unsigned> ri;
          for(const auto &a : comps[i]) ri.insert(a.rel);
          for(const auto &a : comps[j])
            if(ri.count(a.rel))
              throw MobiusDecline("Möbius: within-disjunct self-join (a "
                "relation shared by two components) -- needs ranking "
                "(Increment 3)");
        }
      std::vector<pg_uuid_t> ch;
      for(auto &c : comps) { Sentence one{c}; ch.push_back(compile(one,false)); }
      result = mkBool(true, ch);
      memo[key] = uuid2string(result);
      return result;
    }
    // Single connected CQ: must have a separator (a var in all atoms) or it is
    // #P-hard.  Handled by the separator branch below.
  }

  // 4. Separator (independent project) if one exists.
  {
    std::set<std::pair<unsigned,int>> occ;
    std::vector<std::set<long>> classVars;
    if(findSeparator(s, occ, classVars)) {
      // Active domain of the separator class.
      std::set<long> dom;
      for(const auto &rp : occ) {
        auto it = fi.domain.find(rp);
        if(it!=fi.domain.end()) dom.insert(it->second.begin(), it->second.end());
      }
      std::vector<pg_uuid_t> ch;
      for(long a : dom) {
        // Substitute every class variable (per disjunct) by the constant a.
        Sentence sub;
        sub.reserve(s.size());
        for(std::size_t di=0; di<s.size(); ++di) {
          Disjunct nd;
          for(const MAtom &at : s[di]) {
            MAtom na = at;
            for(auto &t : na.args)
              if(t.isVar && classVars[di].count(t.v)) { t.isVar=false; t.v=a; }
            nd.push_back(std::move(na));
          }
          sub.push_back(std::move(nd));
        }
        ch.push_back(compile(sub, false));   // pieces for distinct a independent
      }
      result = mkBool(false, ch);            // independent OR over the domain
      memo[key] = uuid2string(result);
      return result;
    }
  }

  // 5. No separator: a single connected CQ here is genuinely #P-hard; a
  //    multi-disjunct sentence goes to the Möbius (inclusion-exclusion) step.
  if(s.size()==1)
    throw MobiusDecline("Möbius: non-hierarchical single CQ (no separator); "
                        "the query is #P-hard");
  result = mobiusStep(s, top);
  memo[key] = uuid2string(result);
  return result;
}

// ===========================================================================
// Argument decoding (mirrors ucq_joint's columnar convention).
// ===========================================================================

int checkedLen(ArrayType *a, const char *what) {
  if(a==NULL) return 0;
  if(ARR_NDIM(a)>1) provsql_error("ucq_mobius: %s must be 1-D", what);
  if(ARR_HASNULL(a)) provsql_error("ucq_mobius: %s must not contain NULLs", what);
  return ARR_NDIM(a)==0 ? 0 : ARR_DIMS(a)[0];
}
const int32 *intArr(FunctionCallInfo fcinfo, int n, const char *what, int &len){
  ArrayType *a = PG_ARGISNULL(n)?NULL:PG_GETARG_ARRAYTYPE_P(n);
  len = checkedLen(a, what);
  return (a==NULL||len==0)?NULL:(const int32*)ARR_DATA_PTR(a);
}

/// Build the top sentence from the raw query arrays.  Disjunct-local variables
/// are globalised by a per-disjunct offset (returned via @p base) so
/// unification across disjuncts is by (rel,pos), not by raw id, and so head
/// variables can be pinned per disjunct.
Sentence buildSentenceArrays(const int32 *d_nvars, int n_disj,
                             const int32 *a_disj, const int32 *a_rel,
                             const int32 *a_vars, int n_av,
                             const int32 *a_arity, int n_ad,
                             std::vector<long> &base)
{
  if(n_disj==0) provsql_error("ucq_mobius: the UCQ has no disjuncts");
  base.assign(n_disj, 0);
  long acc = 1;   // start at 1 so id 0 is unused (avoids any sentinel clash)
  for(int d=0; d<n_disj; ++d){ base[d]=acc; acc += d_nvars[d] + 1; }

  Sentence s(n_disj);
  int voff=0;
  for(int i=0;i<n_ad;++i){
    const int d=a_disj[i];
    if(d<0||d>=n_disj) provsql_error("ucq_mobius: atom disjunct out of range");
    const int ar=a_arity[i];
    if(ar<0||voff+ar>n_av)
      provsql_error("ucq_mobius: atom_vars shorter than arities");
    MAtom at; at.rel=static_cast<unsigned>(a_rel[i]);
    for(int k=0;k<ar;++k){
      Term t; t.isVar=true; t.v = base[d] + a_vars[voff+k];
      at.args.push_back(t);
    }
    voff+=ar;
    s[d].push_back(std::move(at));
  }
  return s;
}

/// Build a FactIndex (and, optionally, the text->dense-id map) from the raw
/// fact arrays.
FactIndex buildFactIndexArrays(const int32 *f_rel, int n_fr,
                               const int32 *f_elems, int n_fe,
                               const int32 *f_arity,
                               const pg_uuid_t *tok)
{
  // G3 (tuple independence): every present fact must be gated by a bare
  // gate_input.  A fact whose token is an internal gate (a view-derived /
  // reachability / repair_key lineage) is correlated -- out of scope for the
  // lifted-inference recursion, which assumes independence -- so decline and
  // let the caller fall back (the more general joint-width route already had
  // its chance).  The input-gate enum OID, fetched once.
  const Oid input_oid = get_constants(true).GATE_TYPE_TO_OID[gate_input];

  FactIndex fi;
  int eoff=0;
  for(int i=0;i<n_fr;++i){
    const int ar=f_arity[i];
    if(ar<0||eoff+ar>n_fe)
      provsql_error("ucq_mobius: fact_elems shorter than arities");
    unsigned rel=static_cast<unsigned>(f_rel[i]);
    std::vector<long> el;
    for(int k=0;k<ar;++k){
      long e=static_cast<long>(f_elems[eoff+k]);
      el.push_back(e);
      fi.domain[{rel,k}].insert(e);
    }
    eoff+=ar;
    fi.present.insert({rel, el});
    bool nil=true;
    for(int b=0;b<16;++b) if(tok[i].data[b]!=0) nil=false;
    if(!nil) {
      pg_uuid_t tk = tok[i];
      const Datum gt = DirectFunctionCall1(get_gate_type, UUIDPGetDatum(&tk));
      if(static_cast<Oid>(DatumGetInt32(gt)) != input_oid)
        throw MobiusDecline("non-TID input: a fact token is not a bare "
                            "gate_input (correlated / derived lineage)");
    }
    fi.tok[{rel, el}] = nil ? std::string() : uuid2string(tok[i]);
  }
  return fi;
}

/// Decode the query arrays (0..4) into the top sentence (Boolean path).
Sentence decodeQuery(FunctionCallInfo fcinfo)
{
  int n_disj,n_ad,n_ar,n_av,n_aa;
  const int32 *d_nvars = intArr(fcinfo,0,"disjunct_nvars",n_disj);
  const int32 *a_disj  = intArr(fcinfo,1,"atom_disjunct",n_ad);
  const int32 *a_rel   = intArr(fcinfo,2,"atom_rel",n_ar);
  const int32 *a_vars  = intArr(fcinfo,3,"atom_vars",n_av);
  const int32 *a_arity = intArr(fcinfo,4,"atom_arity",n_aa);
  if(n_ad!=n_ar || n_ad!=n_aa)
    provsql_error("ucq_mobius: atom arrays must have the same length");
  std::vector<long> base;
  return buildSentenceArrays(d_nvars,n_disj,a_disj,a_rel,a_vars,n_av,
                             a_arity,n_ad,base);
}

/// Decode the fact arrays (5..8) into a FactIndex (Boolean path).
FactIndex decodeFacts(FunctionCallInfo fcinfo)
{
  int n_fr,n_fe,n_fa,n_ft;
  const int32 *f_rel   = intArr(fcinfo,5,"fact_rel",n_fr);
  const int32 *f_elems = intArr(fcinfo,6,"fact_elems",n_fe);
  const int32 *f_arity = intArr(fcinfo,7,"fact_arity",n_fa);
  ArrayType *toks = PG_ARGISNULL(8)?NULL:PG_GETARG_ARRAYTYPE_P(8);
  n_ft = checkedLen(toks,"fact_tokens");
  if(n_fr!=n_fa || n_fr!=n_ft)
    provsql_error("ucq_mobius: fact arrays must have the same length");
  const pg_uuid_t *tok = (toks&&n_ft)?(const pg_uuid_t*)ARR_DATA_PTR(toks):NULL;
  return buildFactIndexArrays(f_rel,n_fr,f_elems,n_fe,f_arity,tok);
}

// ===========================================================================
// Per-answer (free head variables): head-pin then compile, one circuit per
// output group.  On the first call of a query the facts are gathered once
// (ucq_joint_gather) and the value dictionary cached; each group binds its
// head variables to their values, the head positions are substituted to
// constants across every disjunct, and the Möbius circuit is compiled and
// cached.  Mirrors ucq_joint_provenance_answer (the per-group caching), but
// each answer is a separate compile rather than a single sweep.
// ===========================================================================

struct MobAnswerCache {
  bool ready = false;                 ///< gather succeeded
  Sentence sentence;                  ///< the UCQ template (global vars)
  std::vector<long> base;             ///< per-disjunct variable offset
  std::vector<int>  d_nvars;          ///< per-disjunct n_vars
  FactIndex fi;                       ///< the gathered facts
  std::map<std::string,long> val_to_id;   ///< text value -> dense id
  std::map<std::string,std::string> tokcache;  ///< head-key -> token uuid
};

void mobAnswerCacheDelete(void *arg) { delete reinterpret_cast<MobAnswerCache*>(arg); }

std::string mobHeadKey(const std::vector<std::string> &vals)
{
  std::string k;
  for(const auto &v : vals){ k += v; k.push_back('\x1f'); }
  return k;
}

/// Gather the facts + value dictionary once (via ucq_joint_gather) into @p c.
/// Returns false on any failure (the caller then declines to the fallback).
bool mobGather(Datum descriptor, MobAnswerCache *c)
{
  SPI_connect();
  Oid argt[1] = { JSONBOID };
  Datum argv[1] = { descriptor };
  char argn[1] = { ' ' };
  const int rc = SPI_execute_with_args(
    "SELECT * FROM provsql.ucq_joint_gather($1)", 1, argt, argv, argn, true, 1);
  if(rc != SPI_OK_SELECT || SPI_processed != 1) { SPI_finish(); return false; }

  bool ok = true;
  try {
    TupleDesc td = SPI_tuptable->tupdesc;
    HeapTuple row = SPI_tuptable->vals[0];
    bool isnull;
    auto ia = [&](int col, int &n)->const int32*{
      Datum d = SPI_getbinval(row, td, col, &isnull);
      if(isnull){ n=0; return nullptr; }
      ArrayType *a = DatumGetArrayTypeP(d);
      n = ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
      return (const int32*) ARR_DATA_PTR(a);
    };
    int n_dnv,n_adisj,n_arel,n_avars,n_aarity,n_frel,n_felems,n_farity;
    const int32 *dnv    = ia(1,n_dnv);
    const int32 *adisj  = ia(2,n_adisj);
    const int32 *arel   = ia(3,n_arel);
    const int32 *avars  = ia(4,n_avars);
    const int32 *aarity = ia(5,n_aarity);
    const int32 *frel   = ia(6,n_frel);
    const int32 *felems = ia(7,n_felems);
    const int32 *farity = ia(8,n_farity);
    Datum dtok = SPI_getbinval(row, td, 9, &isnull);
    ArrayType *atok = isnull ? nullptr : DatumGetArrayTypeP(dtok);
    const int n_ftok = atok ? ArrayGetNItems(ARR_NDIM(atok), ARR_DIMS(atok)) : 0;
    const pg_uuid_t *ftok = atok ? (const pg_uuid_t*) ARR_DATA_PTR(atok) : nullptr;
    if(n_frel != n_farity || n_frel != n_ftok)
      throw MobiusDecline("ucq_mobius: fact arrays length mismatch");

    c->sentence = buildSentenceArrays(dnv,n_dnv,adisj,arel,avars,n_avars,
                                      aarity,n_adisj,c->base);
    c->d_nvars.assign(dnv, dnv+n_dnv);
    c->fi = buildFactIndexArrays(frel,n_frel,felems,n_felems,farity,ftok);

    // The value dictionary: dense id -> text, inverted to text -> id.
    Datum dval = SPI_getbinval(row, td, 10, &isnull);
    if(!isnull) {
      ArrayType *aval = DatumGetArrayTypeP(dval);
      Datum *elems; bool *nulls; int nval;
      deconstruct_array(aval, TEXTOID, -1, false, TYPALIGN_INT,
                        &elems, &nulls, &nval);
      for(int i=0;i<nval;++i)
        if(!nulls[i])
          c->val_to_id[TextDatumGetCString(elems[i])] = i;
    }
  } catch(...) {
    ok = false;
  }
  SPI_finish();
  return ok;
}

}  // namespace

/**
 * @brief Materialise the safe-UCQ Möbius circuit and return its root token.
 *
 * Columnar arguments (mirrors @c ucq_joint_materialize_tracked, TID inputs):
 *   0..4 disjunct_nvars, atom_disjunct, atom_rel, atom_vars, atom_arity
 *   5..8 fact_rel, fact_elems, fact_arity, fact_tokens
 *   9    lineage (uuid, optional): the literal Boolean provenance of the query,
 *        carried on the root gate_mobius so the token still answers Shapley /
 *        semiring / PROV on the normal lineage (the Möbius combination is a
 *        probability-only shortcut layered over it).
 *
 * The root is a @c gate_mobius carrying the certified-independent Boolean
 * islands (the signed combination) and the lineage child; @c probability_evaluate
 * answers the #P-hard UCQ in PTIME through the fast Möbius route, and every other
 * evaluator passes through to the lineage.  Declines raise an error so the SQL
 * wrapper falls back.
 */
Datum ucq_mobius_materialize_tracked(PG_FUNCTION_ARGS)
{
  try {
    Sentence s = decodeQuery(fcinfo);
    FactIndex fi = decodeFacts(fcinfo);
    std::string lineage;
    if(!PG_ARGISNULL(9))
      lineage = uuid2string(*PG_GETARG_UUID_P(9));
    MobiusStats st;
    MobiusCompiler mc(fi, st);
    pg_uuid_t root = mc.compileTop(s, lineage);
    pg_uuid_t *u = (pg_uuid_t*) palloc(sizeof(pg_uuid_t));
    *u = root;
    PG_RETURN_UUID_P(u);
  } catch(const MobiusDecline &e) {
    provsql_error("ucq_mobius: %s", e.what());
  } catch(const std::exception &e) {
    provsql_error("ucq_mobius: %s", e.what());
  } catch(...) {
    provsql_error("ucq_mobius: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief Compile the Möbius circuit and return the lattice statistics plus the
 *        probability (the demonstrability surface).  Same columnar arguments
 *        as @c ucq_mobius_materialize_tracked.
 */
Datum ucq_mobius_compile_stats(PG_FUNCTION_ARGS)
{
  try {
    Sentence s = decodeQuery(fcinfo);
    FactIndex fi = decodeFacts(fcinfo);
    MobiusStats st;
    MobiusCompiler mc(fi, st);
    pg_uuid_t root = mc.compileTop(s);
    st.probability = mobius_probability_of(root);

    TupleDesc tupdesc;
    if(get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      provsql_error("ucq_mobius_compile_stats: expected composite return type");
    tupdesc = BlessTupleDesc(tupdesc);
    Datum values[9];
    bool nulls[9] = {false,false,false,false,false,false,false,false,false};
    values[0] = Float8GetDatum(st.probability);
    values[1] = Int32GetDatum(st.n_components);
    values[2] = Int32GetDatum(st.n_cnf_conjuncts);
    values[3] = Int32GetDatum(st.lattice_collapsed);
    values[4] = Int32GetDatum(st.n_nonzero);
    values[5] = Int32GetDatum(st.n_cancelled);
    values[6] = BoolGetDatum(st.cancelled_hard);
    values[7] = Int64GetDatum(static_cast<int64>(st.dd_size));
    values[8] = Int64GetDatum(static_cast<int64>(st.memo_hits));
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  } catch(const std::exception &e) {
    provsql_error("ucq_mobius_compile_stats: %s", e.what());
  } catch(...) {
    provsql_error("ucq_mobius_compile_stats: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief Per-answer Möbius provenance (the planner-substituted entry point for
 *        a non-Boolean UCQ with free head variables).
 *
 * Arguments: (descriptor jsonb, head_vars int[], head_vals text[],
 * fallback uuid).  Called once per output group; on the first call the facts
 * are gathered once and the value dictionary cached, then each group pins its
 * head variables (the canonical head indices @p head_vars, in every disjunct)
 * to their values (@p head_vals, matched through the gather's text dictionary)
 * and compiles the head-pinned Möbius circuit, caching head-key -> token.  On
 * any decline (unsafe shape, head value absent, ...) returns @p fallback.
 */
Datum ucq_mobius_provenance_answer(PG_FUNCTION_ARGS)
{
  MobAnswerCache *cache =
    reinterpret_cast<MobAnswerCache*>(fcinfo->flinfo->fn_extra);

  if(cache == nullptr) {
    MemoryContext fnctx = fcinfo->flinfo->fn_mcxt;
    cache = new MobAnswerCache();
    MemoryContextCallback *cb = (MemoryContextCallback*)
      MemoryContextAllocZero(fnctx, sizeof(MemoryContextCallback));
    cb->func = mobAnswerCacheDelete;
    cb->arg = cache;
    MemoryContextRegisterResetCallback(fnctx, cb);
    fcinfo->flinfo->fn_extra = cache;

    if(!PG_ARGISNULL(0)) {
      // Gather inside a subtransaction so a SQL error declines gracefully.
      MemoryContext oldcxt = CurrentMemoryContext;
      ResourceOwner oldowner = CurrentResourceOwner;
      BeginInternalSubTransaction(NULL);
      PG_TRY();
      {
        cache->ready = mobGather(PG_GETARG_DATUM(0), cache);
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcxt);
        CurrentResourceOwner = oldowner;
      }
      PG_CATCH();
      {
        MemoryContextSwitchTo(oldcxt);
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcxt);
        CurrentResourceOwner = oldowner;
        FlushErrorState();
        cache->ready = false;
      }
      PG_END_TRY();
    }
  }

  // The group's head values (text), and the head variable indices.
  if(cache->ready && !PG_ARGISNULL(1) && !PG_ARGISNULL(2)) {
    std::vector<int> head_vars;
    {
      ArrayType *a = PG_GETARG_ARRAYTYPE_P(1);
      const int32 *d = (const int32*) ARR_DATA_PTR(a);
      const int n = ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
      for(int i=0;i<n;++i) head_vars.push_back(d[i]);
    }
    std::vector<std::string> head_vals;
    {
      ArrayType *a = PG_GETARG_ARRAYTYPE_P(2);
      Datum *elems; bool *nulls; int n;
      deconstruct_array(a, TEXTOID, -1, false, TYPALIGN_INT, &elems, &nulls, &n);
      for(int i=0;i<n;++i)
        head_vals.push_back(nulls[i] ? std::string() : TextDatumGetCString(elems[i]));
    }

    if(head_vars.size() == head_vals.size()) {
      const std::string key = mobHeadKey(head_vals);
      auto it = cache->tokcache.find(key);
      if(it != cache->tokcache.end()) {
        pg_uuid_t *u = (pg_uuid_t*) palloc(sizeof(pg_uuid_t));
        *u = string2uuid(it->second);
        PG_RETURN_UUID_P(u);
      }
      try {
        // Map each head value to its dense id; pin the head variable (canonical
        // index hv, hence global base[d]+hv in every disjunct) to that constant.
        Sentence s = cache->sentence;
        bool resolved = true;
        for(std::size_t h=0; h<head_vars.size() && resolved; ++h) {
          auto vit = cache->val_to_id.find(head_vals[h]);
          if(vit == cache->val_to_id.end()) { resolved = false; break; }
          const long val = vit->second;
          const int hv = head_vars[h];
          for(std::size_t d=0; d<s.size(); ++d) {
            const long gid = cache->base[d] + hv;
            for(MAtom &at : s[d])
              for(Term &t : at.args)
                if(t.isVar && t.v == gid) { t.isVar=false; t.v=val; }
          }
        }
        if(resolved) {
          // The per-group literal lineage (the normal per-answer provenance,
          // argument 3) is carried on the gate_mobius so this answer's token
          // still answers Shapley / semiring on its normal lineage.
          std::string lineage;
          if(!PG_ARGISNULL(3))
            lineage = uuid2string(*PG_GETARG_UUID_P(3));
          MobiusStats st;
          MobiusCompiler mc(cache->fi, st);
          pg_uuid_t root = mc.compileTop(s, lineage);
          cache->tokcache[key] = uuid2string(root);
          pg_uuid_t *u = (pg_uuid_t*) palloc(sizeof(pg_uuid_t));
          *u = root;
          PG_RETURN_UUID_P(u);
        }
      } catch(...) {
        // decline this group -> fallback
      }
    }
  }

  if(PG_ARGISNULL(3))
    PG_RETURN_NULL();
  PG_RETURN_DATUM(PG_GETARG_DATUM(3));
}
