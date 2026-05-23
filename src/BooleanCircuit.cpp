/**
 * @file BooleanCircuit.cpp
 * @brief Boolean circuit implementation and evaluation algorithms.
 *
 * Implements the methods declared in @c BooleanCircuit.h, including:
 * - Gate management (@c addGate, @c setGate, @c setInfo, @c setProb).
 * - Probability evaluation algorithms: possible worlds, Monte Carlo,
 *   WeightMC, independent evaluation.
 * - Knowledge compilation: @c compilation() (external tools),
 *   @c interpretAsDD() (direct from circuit structure),
 *   @c makeDD() (dispatcher).
 * - @c rewriteMultivaluedGates(): replaces MULVAR/MULIN clusters with
 *   standard AND/OR/NOT circuits.
 * - @c TseytinCNF(): DIMACS/weighted CNF generation for model counters.
 * - @c exportCircuit(): serialisation in the @c tdkc text format.
 * - @c toString(): human-readable gate description.
 *
 * In the standalone @c tdkc build (when @c TDKC is defined) a lightweight
 * @c elog() stub replaces the PostgreSQL error-reporting function.
 */
#include "BooleanCircuit.h"
#include "Circuit.hpp"
#include <type_traits>

extern "C" {
#include <unistd.h>
#include <sys/wait.h>
#include <math.h>
}

#include <cassert>
#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>
#include <stack>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>

#include "dDNNFTreeDecompositionBuilder.h"
#include "external_tool.h"

// "provsql_utils.h"
#ifdef TDKC
constexpr bool provsql_interrupted = false;
constexpr int provsql_verbose = 0;
constexpr int provsql_monte_carlo_seed = -1;
enum levels {ERROR, NOTICE};
#define elog(level, ...) {fprintf(stderr, __VA_ARGS__); if(level==ERROR) exit(EXIT_FAILURE);}
#define CHECK_FOR_INTERRUPTS() ((void)0)
#else
extern "C" {
#include "provsql_utils.h"
#include "utils/elog.h"
#include "miscadmin.h"
}
#endif
#include "provsql_error.h"
#include "scoped_tempdir.h"
using provsql::ScopedTempDir;

gate_t BooleanCircuit::setGate(BooleanGate type)
{
  auto id = Circuit::setGate(type);
  if(type == BooleanGate::IN) {
    setProb(id,1.);
    inputs.insert(id);
  } else if(type == BooleanGate::MULIN) {
    mulinputs.insert(id);
  }
  return id;
}

gate_t BooleanCircuit::setGate(const uuid &u, BooleanGate type)
{
  auto id = Circuit::setGate(u, type);
  if(type == BooleanGate::IN) {
    setProb(id,1.);
    inputs.insert(id);
  } else if(type == BooleanGate::MULIN) {
    mulinputs.insert(id);
  }
  return id;
}

gate_t BooleanCircuit::setGate(const uuid &u, BooleanGate type, double p)
{
  auto id = setGate(u, type);
  if(std::isnan(p))
    p=1.;
  setProb(id,p);
  return id;
}

gate_t BooleanCircuit::setGate(BooleanGate type, double p)
{
  auto id = setGate(type);
  if(std::isnan(p))
    p=1.;
  setProb(id,p);
  return id;
}

gate_t BooleanCircuit::addGate()
{
  auto id=Circuit::addGate();
  prob.push_back(1);
  return id;
}

std::string BooleanCircuit::toString(gate_t g) const
{
  return toStringHelper(g, BooleanGate::UNDETERMINED, nullptr);
}

std::string BooleanCircuit::toString(
  gate_t g,
  const std::unordered_map<gate_t, std::string> &labels) const
{
  return toStringHelper(g, BooleanGate::UNDETERMINED, &labels);
}

std::string BooleanCircuit::toStringHelper(
  gate_t g,
  BooleanGate parent,
  const std::unordered_map<gate_t, std::string> *labels) const
{
  std::string op;
  std::string result;
  auto gtype = getGateType(g);

  switch(gtype) {
  case BooleanGate::IN:
    if(labels) {
      auto it = labels->find(g);
      if(it != labels->end())
        return it->second;
    }
    return "x"+to_string(g);
  case BooleanGate::MULIN:
    if(labels) {
      auto it = labels->find(g);
      if(it != labels->end())
        return it->second + "[" + std::to_string(getProb(g)) + "]";
    }
    return "{" + to_string(*getWires(g).begin()) + "=" + std::to_string(getInfo(g)) + "}[" + std::to_string(getProb(g)) + "]";
  case BooleanGate::NOT:
    op="¬";
    break;
  case BooleanGate::UNDETERMINED:
    op="?";
    break;
  case BooleanGate::AND:
    op="∧";
    break;
  case BooleanGate::OR:
    op="∨";
    break;
  case BooleanGate::MULVAR:
    ;   // already dealt with in MULIN
  }

  if(getWires(g).empty()) {
    if(gtype==BooleanGate::AND)
      return "⊤";
    else if(gtype==BooleanGate::OR)
      return "⊥";
    else return op;
  }

  for(auto s: getWires(g)) {
    if(gtype==BooleanGate::NOT)
      result = op;
    else if(!result.empty())
      result+=" "+op+" ";
    result+=toStringHelper(s, gtype, labels);
  }

  // Parenthesis elision:
  //   * single-wire AND/OR: the join carries no information, drop the wrap.
  //   * root call (parent = UNDETERMINED): no enclosing context, drop the wrap.
  //   * same-op nesting (parent == gtype, AND/OR only): associative, drop the wrap.
  bool single_join = (gtype==BooleanGate::AND || gtype==BooleanGate::OR)
                     && getWires(g).size()==1;
  bool same_op_assoc = (gtype==BooleanGate::AND || gtype==BooleanGate::OR)
                       && parent==gtype;
  if(single_join || parent==BooleanGate::UNDETERMINED || same_op_assoc)
    return result;
  return "("+result+")";
}

std::string BooleanCircuit::exportCircuit(gate_t root) const
{
  std::stringstream ss;

  std::unordered_set<gate_t> processed;
  std::stack<gate_t> to_process;
  to_process.push(root);

  while(!to_process.empty()) {
    auto g=to_process.top();
    to_process.pop();

    if(processed.find(g)!=processed.end())
      continue;

    ss << g << " ";

    switch(getGateType(g)) {
    case BooleanGate::IN:
      ss << "IN " << getProb(g);
      break;

    case BooleanGate::NOT:
      ss << "NOT " << getWires(g)[0];
      break;

    case BooleanGate::AND:
      ss << "AND";

      for(auto s:getWires(g))
        ss << " " << s;
      break;

    case BooleanGate::OR:
      ss << "OR";

      for(auto s:getWires(g))
        ss << " " << s;
      break;

    case BooleanGate::MULVAR:
    case BooleanGate::MULIN:
    case BooleanGate::UNDETERMINED:
      assert(false);   // not done
    }

    ss << "\n";

    for(auto s: getWires(g)) {
      if(processed.find(s)==processed.end())
        to_process.push(s);
    }

    processed.insert(g);
  }

  return ss.str();
}

bool BooleanCircuit::evaluate(gate_t g, const std::unordered_set<gate_t> &sampled) const
{
  bool disjunction=false;

  switch(getGateType(g)) {
  case BooleanGate::IN:
    return sampled.find(g)!=sampled.end();
  case BooleanGate::MULIN:
  case BooleanGate::MULVAR:
    throw CircuitException("Monte-Carlo sampling not implemented on multivalued inputs");
  case BooleanGate::NOT:
    return !evaluate(*(getWires(g).begin()), sampled);
  case BooleanGate::AND:
    disjunction = false;
    break;
  case BooleanGate::OR:
    disjunction = true;
    break;
  case BooleanGate::UNDETERMINED:
    throw CircuitException("Incorrect gate type");
  }

  for(auto s: getWires(g)) {
    bool e = evaluate(s, sampled);
    if(disjunction && e)
      return true;
    if(!disjunction && !e)
      return false;
  }

  if(disjunction)
    return false;
  else
    return true;
}

double BooleanCircuit::monteCarlo(gate_t g, unsigned samples) const
{
  // Seed mt19937_64 from the provsql.monte_carlo_seed GUC: -1 (the
  // default) means non-deterministic via std::random_device, any other
  // value (including 0) is a literal seed so regression tests can pin
  // sampling for reproducibility.
  std::mt19937_64 rng;
  if(provsql_monte_carlo_seed != -1) {
    rng.seed(static_cast<uint64_t>(provsql_monte_carlo_seed));
  } else {
    std::random_device rd;
    rng.seed((static_cast<uint64_t>(rd()) << 32) | rd());
  }
  std::uniform_real_distribution<double> uniform01(0.0, 1.0);

  auto success{0u};

  for(unsigned i=0; i<samples; ++i) {
    std::unordered_set<gate_t> sampled;
    for(auto in: inputs) {
      if(uniform01(rng) < getProb(in)) {
        sampled.insert(in);
      }
    }

    if(evaluate(g, sampled))
      ++success;

    if(provsql_interrupted)
      throw CircuitException("Interrupted after "+std::to_string(i+1)+" samples");
  }

  return success*1./samples;
}

double BooleanCircuit::possibleWorlds(gate_t g) const
{
  if(inputs.size()>=8*sizeof(unsigned long long))
    throw CircuitException("Too many possible worlds to iterate over");

  unsigned long long nb=(1<<inputs.size());
  double totalp=0.;

  for(unsigned long long i=0; i < nb; ++i) {
    std::unordered_set<gate_t> s;
    double p = 1;

    unsigned j=0;
    for(gate_t in : inputs) {
      if(i & (1 << j)) {
        s.insert(in);
        p*=getProb(in);
      } else {
        p*=1-getProb(in);
      }
      ++j;
    }

    if(evaluate(g, s))
      totalp+=p;

    if(provsql_interrupted)
      throw CircuitException("Interrupted");
  }

  return totalp;
}

std::string BooleanCircuit::TseytinCNF(gate_t g, bool display_prob) const {
  std::vector<std::vector<int> > clauses;

  // Tseytin transformation
  for(gate_t i{0}; i<gates.size(); ++i) {
    switch(getGateType(i)) {
    case BooleanGate::AND:
    {
      int id{static_cast<int>(i)+1};
      std::vector<int> c = {id};
      for(auto s: getWires(i)) {
        clauses.push_back({-id, static_cast<int>(s)+1});
        c.push_back(-static_cast<int>(s)-1);
      }
      clauses.push_back(c);
      break;
    }

    case BooleanGate::OR:
    {
      int id{static_cast<int>(i)+1};
      std::vector<int> c = {-id};
      for(auto s: getWires(i)) {
        clauses.push_back({id, -static_cast<int>(s)-1});
        c.push_back(static_cast<int>(s)+1);
      }
      clauses.push_back(c);
    }
    break;

    case BooleanGate::NOT:
    {
      int id=static_cast<int>(i)+1;
      auto s=*getWires(i).begin();
      clauses.push_back({-id,-static_cast<int>(s)-1});
      clauses.push_back({id,static_cast<int>(s)+1});
      break;
    }

    case BooleanGate::MULIN:
      throw CircuitException("Multivalued inputs should have been removed by then.");
    case BooleanGate::MULVAR:
    case BooleanGate::IN:
    case BooleanGate::UNDETERMINED:
      ;
    }
  }
  clauses.push_back({(int)g+1});

  std::ostringstream oss;
  oss << "p cnf " << gates.size() << " " << clauses.size() << "\n";
  for(unsigned i=0; i<clauses.size(); ++i) {
    for(int x : clauses[i]) {
      oss << x << " ";
    }
    oss << "0\n";
  }
  if(display_prob) {
    for(gate_t in: inputs) {
      oss << "w " << (static_cast<std::underlying_type<gate_t>::type>(in)+1) << " " << getProb(in) << "\n";
      oss << "w -" << (static_cast<std::underlying_type<gate_t>::type>(in)+1) << " " << (1. - getProb(in)) << "\n";
    }
  }
  return oss.str();
}

dDNNF BooleanCircuit::paniniCompile(gate_t g, const std::string &lang) const {
  // Validate / map the language token to Panini's `--lang` argument.
  // Three target classes ProvSQL exposes:
  //   OBDD             unstructured / structured BDD
  //   OBDD[AND]        OBDD augmented with conjunctive decomposition
  //   Decision-DNNF    decomposable + decision-deterministic NNF
  // R2-D2 and CCDD are intentionally omitted: both emit Panini K
  // (kernelize) nodes encoding literal-equivalence constraints over a
  // shared kernel variable, which cannot be expressed as a
  // decomposable AND. Until we case-split the K's kernel variables
  // into a proper decision structure, the probabilities those two
  // languages produce are silently wrong.
  if (lang != "OBDD" && lang != "OBDD[AND]" && lang != "Decision-DNNF") {
    throw CircuitException("Unknown Panini target language: " + lang);
  }

  if (find_external_tool("panini").empty())
    throw CircuitException(
            "panini not found on PATH; install KCBox/Panini or add "
            "its directory to provsql.tool_search_path");

  ScopedTempDir tmp;
  std::string filename    = tmp.file("input");
  std::string outfilename = tmp.file("input.out");
  {
    std::ofstream ofs(filename);
    ofs << TseytinCNF(g, false);
  }

  if (provsql_verbose >= 20) {
    provsql_notice("Tseytin circuit in %s", filename.c_str());
  }

  // Panini's binary is a multi-tool dispatcher: argv[1] must be the
  // sub-tool name ("Panini"), the actual options follow.
  std::string cmdline =
      "panini Panini --lang \"" + lang + "\" --out " + outfilename
      + " --quiet " + filename;

  int retvalue = run_external_tool(cmdline);

#ifndef TDKC
  if (WIFSIGNALED(retvalue) && WTERMSIG(retvalue) == SIGINT) {
    InterruptPending = true;
    QueryCancelPending = true;
  }
#endif
  CHECK_FOR_INTERRUPTS();

  if (retvalue)
    throw CircuitException(format_external_tool_status(retvalue, "panini"));

  std::ifstream ifs(outfilename.c_str());
  if (!ifs)
    throw CircuitException("Cannot open Panini output: " + outfilename);

  // Skip Panini's preamble ("Variable order: ...", "Maximum variable: ...",
  // "Number of nodes: ...") and stop at the first data line, which always
  // starts with "0:".
  std::string line;
  bool found_data = false;
  while (std::getline(ifs, line)) {
    if (line.rfind("0:", 0) == 0) { found_data = true; break; }
  }
  if (!found_data)
    throw CircuitException("Panini output: no data lines found");

  dDNNF dnnf;
  // Panini node ids are sequential 0, 1, 2, ... Highest id is the
  // root of the compilation.
  std::vector<gate_t> id_to_gate;

  do {
    if (line.empty()) continue;
    auto colon_pos = line.find(':');
    if (colon_pos == std::string::npos) continue;

    // Sanity-check the leading id matches the size of id_to_gate so far
    // (the file should be in monotonically increasing id order).
    int panini_id = std::stoi(line.substr(0, colon_pos));
    if (static_cast<size_t>(panini_id) != id_to_gate.size())
      throw CircuitException(
              "Panini output: out-of-order node id "
              + std::to_string(panini_id));

    std::stringstream ss(line.substr(colon_pos + 1));
    std::string first;
    ss >> first;

    gate_t this_gate;
    if (first == "F") {
      // FALSE terminal: empty OR.
      this_gate = dnnf.setGate(BooleanGate::OR);
    } else if (first == "T") {
      // TRUE terminal: empty AND.
      this_gate = dnnf.setGate(BooleanGate::AND);
    } else if (first == "C" || first == "D") {
      // C (CONJOIN), D (DECOMPOSE) are decomposable conjunctions in
      // Panini's CDD format; OR is only ever expressed implicitly by
      // the (v ? t : f) decision nodes. K (KERNELIZE) nodes encode
      // literal-equivalence constraints over a shared kernel
      // variable and break decomposability; we refuse the only two
      // target languages that emit them (R2-D2 and CCDD) upstream,
      // so seeing K here is an upstream-Panini surprise.
      this_gate = dnnf.setGate(BooleanGate::AND);
      int child;
      while (ss >> child) {
        if (child == 0) break;
        if (child < 0 || static_cast<size_t>(child) >= id_to_gate.size())
          throw CircuitException(
                  "Panini output: forward / invalid child reference "
                  + std::to_string(child));
        dnnf.addWire(this_gate, id_to_gate[child]);
      }
    } else if (first == "K") {
      throw CircuitException(
              "Panini output: unexpected K (kernelize) node; ProvSQL "
              "does not support Panini target languages that emit K "
              "nodes (R2-D2, CCDD).");
    } else {
      // Decision node: <var> <false_child> <true_child> [0]
      // (Panini's CDD::Display emits children in ch[0]/ch[1] order;
      // CDD.cpp's DOT writer maps ch[0] to the dotted/false edge and
      // ch[1] to the solid/true edge.)
      int var = std::stoi(first);
      int f_child, t_child;
      if (!(ss >> f_child >> t_child))
        throw CircuitException(
                "Panini output: malformed decision line at id "
                + std::to_string(panini_id));
      if (t_child < 0 || f_child < 0
          || static_cast<size_t>(t_child) >= id_to_gate.size()
          || static_cast<size_t>(f_child) >= id_to_gate.size())
        throw CircuitException(
                "Panini output: forward / invalid decision child at id "
                + std::to_string(panini_id));
      gate_t t_gate = id_to_gate[t_child];
      gate_t f_gate = id_to_gate[f_child];

      // Translate the decision. Two cases:
      //   (a) v is an input gate: keep the literal in the structure,
      //       OR(AND(v, t'), AND(NOT(v), f')).
      //   (b) v is a Tseytin auxiliary: aux vars are functionally
      //       determined by inputs, so under WMC we want literal
      //       weights w(v) = w(NOT v) = 1 (not (p, 1-p)). With those
      //       weights the AND wrappers contribute 1 to either branch
      //       and we can drop them entirely, emitting just
      //       OR(t', f'). The OR is not determinism-preserving on
      //       the variable v, but the input-projection of its two
      //       arms is still disjoint by Tseytin determinism so
      //       @c dDNNF::probabilityEvaluation() still returns the
      //       correct weighted model count.
      size_t var_idx = static_cast<size_t>(var) - 1;
      if (var_idx < gates.size() && gates[var_idx] == BooleanGate::IN) {
        gate_t pos_lit = dnnf.setGate(
            getUUID(static_cast<gate_t>(var_idx)),
            BooleanGate::IN, prob[var_idx]);
        gate_t neg_lit = dnnf.setGate(BooleanGate::NOT);
        dnnf.addWire(neg_lit, pos_lit);
        gate_t and_t = dnnf.setGate(BooleanGate::AND);
        dnnf.addWire(and_t, pos_lit);
        dnnf.addWire(and_t, t_gate);
        gate_t and_f = dnnf.setGate(BooleanGate::AND);
        dnnf.addWire(and_f, neg_lit);
        dnnf.addWire(and_f, f_gate);
        this_gate = dnnf.setGate(BooleanGate::OR);
        dnnf.addWire(this_gate, and_t);
        dnnf.addWire(this_gate, and_f);
      } else {
        this_gate = dnnf.setGate(BooleanGate::OR);
        dnnf.addWire(this_gate, t_gate);
        dnnf.addWire(this_gate, f_gate);
      }
    }
    id_to_gate.push_back(this_gate);
  } while (std::getline(ifs, line));

  ifs.close();

  if (provsql_verbose >= 20) {
    tmp.keep();
    provsql_notice("Compiled Panini %s in %s",
                   lang.c_str(), outfilename.c_str());
  }

  if (id_to_gate.empty())
    throw CircuitException("Panini output produced no nodes");

  // The root of a Panini DD is the highest-id node.
  dnnf.setRoot(id_to_gate.back());

  dnnf.simplify();
  return dnnf;
}

dDNNF BooleanCircuit::compilation(gate_t g, std::string compiler) const {
  // Panini (KCBox) does not produce a d4-style NNF file: it emits its
  // own BDD/DD format. Dispatch to the Panini-specific compile + parse
  // path early so we do not run the NNF parser below on its output.
  if (compiler.rfind("panini-", 0) == 0) {
    std::string suffix = compiler.substr(7);
    std::string lang;
    if      (suffix == "obdd")     lang = "OBDD";
    else if (suffix == "obdd-and") lang = "OBDD[AND]";
    else if (suffix == "decdnnf")  lang = "Decision-DNNF";
    else
      throw CircuitException("Unknown Panini variant: " + compiler);
    return paniniCompile(g, lang);
  }

  // Validate the compiler name before any temp-dir or CNF work; that
  // keeps the unknown-compiler error path lean and leak-free.
  if(compiler!="d4" && compiler!="d4v2" && compiler!="c2d"
     && compiler!="minic2d" && compiler!="dsharp") {
    throw CircuitException("Unknown compiler '"+compiler+"'");
  }
  if(find_external_tool(compiler).empty())
    throw CircuitException(
            compiler + " not found on PATH; install it or add its "
            "directory to provsql.tool_search_path");

  ScopedTempDir tmp;
  std::string filename    = tmp.file("input");
  std::string outfilename = tmp.file("input.nnf");
  {
    std::ofstream ofs(filename);
    ofs << TseytinCNF(g, false);
  }

  if(provsql_verbose>=20) {
    provsql_notice("Tseytin circuit in %s", filename.c_str());
  }

  bool new_d4 {false};
  std::string cmdline=compiler+" ";
  if(compiler=="d4") {
    cmdline+="-dDNNF "+filename+" -out="+outfilename;
    new_d4 = true;
  } else if(compiler=="d4v2") {
    // d4v2 (crillab/d4v2): rewritten d4 with library-first architecture
    // and tree-decomposition-guided branching by default. We feed it
    // the same Tseytin CNF the other compilers use; its --input-type
    // circuit mode (BC-S1.2) would in principle let us skip Tseytin,
    // but d4v2's BC parser ignores `I` declarations (TODO in upstream)
    // so the resulting variable numbering does not match our gate
    // indices and the parser cannot map decisions back to inputs.
    cmdline+="-i "+filename+" --dump-file "+outfilename;
    new_d4 = true;
  } else if(compiler=="c2d") {
    cmdline+="-in "+filename+" -silent";
  } else if(compiler=="minic2d") {
    // miniC2D 1.0.0's getopt uses -c (--cnf) for the input CNF, not
    // -in like c2d. The NNF is written to <filename>.nnf, which
    // matches the outfilename convention we use below.
    cmdline+="-c "+filename;
  } else /* dsharp */ {
    cmdline+="-q -Fnnf "+outfilename+" "+filename;
  }

  int retvalue=run_external_tool(cmdline);

  // PG's StatementTimeoutHandler (and pg_cancel_backend, etc.) sends
  // SIGINT to the whole process group via kill(-MyProcPid, SIGINT).
  // The child compiler in our group dies on default SIGINT, but
  // glibc system() temporarily SIG_IGNs SIGINT in the parent for the
  // duration of the wait, so the same signal is silently discarded
  // here and InterruptPending / QueryCancelPending are never set.
  // Translate that wstatus into a proper PG cancel so the
  // CHECK_FOR_INTERRUPTS below raises 57014 instead of us either
  // throwing "Error executing", or falling through to the legacy
  // d4-syntax retry on the corpse (which then mis-parses an empty
  // .nnf as "Unreadable d-DNNF" XX000).
#ifndef TDKC
  if(WIFSIGNALED(retvalue) && WTERMSIG(retvalue) == SIGINT) {
    InterruptPending = true;
    QueryCancelPending = true;
  }
#endif

  CHECK_FOR_INTERRUPTS();

  // PG's StatementTimeoutHandler (and pg_cancel_backend, etc.) sends
  // SIGINT to the whole process group via kill(-MyProcPid, SIGINT).
  // The child compiler in our group dies on default SIGINT, but
  // glibc system() temporarily SIG_IGNs SIGINT in the parent for the
  // duration of the wait, so the same signal is silently discarded
  // here and InterruptPending / QueryCancelPending are never set.
  // Translate that wstatus into a proper PG cancel so the
  // CHECK_FOR_INTERRUPTS below raises 57014 instead of us either
  // throwing "Error executing", or falling through to the legacy
  // d4-syntax retry on the corpse (which then mis-parses an empty
  // .nnf as "Unreadable d-DNNF" XX000).
#ifndef TDKC
  if(WIFSIGNALED(retvalue) && WTERMSIG(retvalue) == SIGINT) {
    InterruptPending = true;
    QueryCancelPending = true;
  }
#endif

  CHECK_FOR_INTERRUPTS();

  if(retvalue && compiler=="d4") {
    // Temporary support for older version of d4
    new_d4 = false;
    cmdline = "d4 "+filename+" -out="+outfilename;
    retvalue=run_external_tool(cmdline);
#ifndef TDKC
    if(WIFSIGNALED(retvalue) && WTERMSIG(retvalue) == SIGINT) {
      InterruptPending = true;
      QueryCancelPending = true;
    }
#endif
  }

  CHECK_FOR_INTERRUPTS();

  if(retvalue)
    throw CircuitException(format_external_tool_status(retvalue, compiler));

  std::ifstream ifs(outfilename.c_str());

  std::string line;
  getline(ifs,line);

  if(line.rfind("nnf", 0) != 0) {
    // New d4 / d4v2 do not include this magic line; any other compiler
    // that omits it has produced an unsatisfiable formula (its NNF file
    // is empty, so we get end-of-stream here).

    if(compiler != "d4" && compiler != "d4v2") {
      // unsatisfiable formula
      return dDNNF();
    }
  } else {
    std::string nnf;
    unsigned nb_nodes, nb_edges, nb_variables;

    std::stringstream ss(line);
    ss >> nnf >> nb_nodes >> nb_edges >> nb_variables;

    if(nb_variables!=gates.size())
      throw CircuitException("Unreadable d-DNNF (wrong number of variables: " + std::to_string(nb_variables) +" vs " + std::to_string(gates.size()) + ")");

    getline(ifs,line);
  }

  dDNNF dnnf;

  unsigned i=0;
  do {
    std::stringstream ss(line);

    std::string c;
    ss >> c;

    if(c=="O") {
      int var, args;
      ss >> var >> args;
      auto id=dnnf.getGate(std::to_string(i));
      dnnf.setGate(std::to_string(i), BooleanGate::OR);
      int g;
      while(ss >> g) {
        auto id2=dnnf.getGate(std::to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=="A") {
      int args;
      ss >> args;
      auto id=dnnf.getGate(std::to_string(i));
      dnnf.setGate(std::to_string(i), BooleanGate::AND);
      int g;
      while(ss >> g) {
        auto id2=dnnf.getGate(std::to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=="L") {
      int leaf;
      ss >> leaf;
      auto and_gate=dnnf.setGate(std::to_string(i), BooleanGate::AND);
      if(gates[abs(leaf)-1]==BooleanGate::IN) {
        if(leaf<0) {
          auto leaf_gate = dnnf.setGate(getUUID(static_cast<gate_t>(-leaf-1)), BooleanGate::IN, prob[-leaf-1]);
          auto not_gate = dnnf.setGate(BooleanGate::NOT);
          dnnf.addWire(not_gate, leaf_gate);
          dnnf.addWire(and_gate, not_gate);
        } else {
          auto leaf_gate = dnnf.setGate(getUUID(static_cast<gate_t>(leaf-1)), BooleanGate::IN, prob[leaf-1]);
          dnnf.addWire(and_gate, leaf_gate);
        }
      } else {
        ; // Do nothing, TRUE gate
      }
    } else if(c=="f" || c=="o") {
      // d4 extended format
      // A FALSE gate is an OR gate without wires
      int var;
      ss >> var;
      dnnf.setGate(std::to_string(var), BooleanGate::OR);
    } else if(c=="t" || c=="a") {
      // d4 extended format
      // A TRUE gate is an AND gate without wires
      int var;
      ss >> var;
      dnnf.setGate(std::to_string(var), BooleanGate::AND);
    } else if(dnnf.hasGate(c)) {
      // d4 extended format
      int var;
      ss >> var;
      auto id2=dnnf.getGate(std::to_string(var));

      std::vector<int> decisions;
      int decision;
      while(ss >> decision) {
        if(decision==0)
          break;
        // d4v2 may introduce internal Tseytin aux variables beyond
        // our gate count when given a circuit input. They appear as
        // decision literals on edges; treat them as non-IN (skipped),
        // matching how we handle the auxiliaries from our own Tseytin.
        size_t idx = static_cast<size_t>(abs(decision)) - 1;
        if(idx < gates.size() && gates[idx]==BooleanGate::IN)
          decisions.push_back(decision);
      }

      if(decisions.empty()) {
        dnnf.addWire(dnnf.getGate(c), id2);
      } else {
        auto and_gate = dnnf.setGate(BooleanGate::AND);
        dnnf.addWire(dnnf.getGate(c), and_gate);
        dnnf.addWire(and_gate, id2);
        for(auto leaf : decisions) {
          if(leaf<0) {
            auto leaf_gate = dnnf.setGate(getUUID(static_cast<gate_t>(-leaf-1)), BooleanGate::IN, prob[-leaf-1]);
            auto not_gate = dnnf.setGate(BooleanGate::NOT);
            dnnf.addWire(not_gate, leaf_gate);
            dnnf.addWire(and_gate, not_gate);
          } else {
            auto leaf_gate = dnnf.setGate(getUUID(static_cast<gate_t>(leaf-1)), BooleanGate::IN, prob[leaf-1]);
            dnnf.addWire(and_gate, leaf_gate);
          }
        }
      }
    } else
      throw CircuitException(std::string("Unreadable d-DNNF (unknown node type: ")+c+")");

    ++i;
  } while(getline(ifs, line));

  ifs.close();

  if(provsql_verbose>=20) {
    tmp.keep();
    provsql_notice("Compiled d-DNNF in %s", outfilename.c_str());
  }

  dnnf.setRoot(dnnf.getGate(new_d4?"1":std::to_string(i-1)));

  // External NNF writers (c2d, minic2d, dsharp) leave TRUE constants
  // (empty AND gates) and FALSE constants (empty OR gates) embedded in
  // the structure, because their target formats (Decision-DNNF, SDD)
  // require every variable to be "covered" even when its value is
  // forced by the CNF. Run the standard peephole so the d-DNNF returned
  // to callers is in canonical form, matching the tree-decomposition
  // builder which already simplifies.
  dnnf.simplify();

  return dnnf;
}

double BooleanCircuit::Ganak(gate_t g, std::string /*opt*/) const {
  // Ganak (meelgroup/ganak): exact weighted model counter using the
  // MCC 2024 input format. We append our weight lines after the
  // standard Tseytin CNF in the format `c p weight <lit> <w> 0`.
  if(find_external_tool("ganak").empty())
    throw CircuitException(
            "ganak not found on PATH; install it or add its "
            "directory to provsql.tool_search_path");

  ScopedTempDir tmp;
  std::string filename    = tmp.file("input");
  std::string outfilename = tmp.file("input.out");
  {
    std::ofstream ofs(filename);
    ofs << "c t wmc\n";
    ofs << TseytinCNF(g, false);
    for(gate_t in : inputs) {
      int id = static_cast<int>(in) + 1;
      ofs << "c p weight " << id << ' ' << getProb(in)        << " 0\n";
      ofs << "c p weight -" << id << ' ' << (1.0 - getProb(in)) << " 0\n";
    }
  }
  // --mode 7: MPFR float (weighted floating-point counting). The
  // exact line we parse is `c s exact quadruple float <value>`.
  std::string cmdline = "ganak --mode 7 " + filename
                      + " > " + outfilename + " 2>&1";

  int retvalue = run_external_tool(cmdline);
#ifndef TDKC
  if(WIFSIGNALED(retvalue) && WTERMSIG(retvalue) == SIGINT) {
    InterruptPending = true;
    QueryCancelPending = true;
  }
#endif
  CHECK_FOR_INTERRUPTS();
  if(retvalue)
    throw CircuitException(format_external_tool_status(retvalue, "ganak"));

  std::ifstream ifs(outfilename);
  std::string line, value;
  while(std::getline(ifs, line)) {
    // Look for `c s exact arb frac <value>`, `c s exact arb int <value>`,
    // or `c s exact quadruple float <value>`. The value is the last
    // whitespace-separated token on the line.
    if(line.rfind("c s exact", 0) == 0) {
      std::stringstream ss(line);
      std::string tok;
      while(ss >> tok) value = tok;
    }
  }
  ifs.close();
  if(value.empty())
    throw CircuitException("ganak: could not find 'c s exact' line in output");

  // Output may be `<num>/<den>` for fractions (mode 1) or scientific
  // notation for floats; std::stod handles the latter, parse the
  // former by hand.
  double ret;
  auto slash = value.find('/');
  if(slash != std::string::npos) {
    double num = std::stod(value.substr(0, slash));
    double den = std::stod(value.substr(slash + 1));
    ret = (den == 0.0) ? 0.0 : num / den;
  } else {
    ret = std::stod(value);
  }

  if(provsql_verbose >= 20)
    tmp.keep();

  return ret;
}

double BooleanCircuit::SharpSATTD(gate_t g, std::string /*opt*/) const {
  // SharpSAT-TD (Korhonen & Järvisalo, CP 2021): tree-decomposition
  // guided exact weighted model counter. Same MCC 2024 weighted
  // DIMACS input as Ganak. The binary is invoked from a freshly
  // mkdtemp'd directory because it writes flowcutter scratch files
  // into `--tmpdir` and we want them cleaned up afterwards.
  if(find_external_tool("sharpsat-td").empty())
    throw CircuitException(
            "sharpsat-td not found on PATH; install it (binary name "
            "'sharpsat-td', with the flow_cutter_pace17 helper also "
            "on PATH) or add the directory to provsql.tool_search_path");
  if(find_external_tool("flow_cutter_pace17").empty())
    throw CircuitException(
            "sharpsat-td's flow_cutter_pace17 helper not found on PATH; "
            "install it alongside sharpsat-td (the counter invokes it "
            "via the relative path ./flow_cutter_pace17) or add the "
            "directory to provsql.tool_search_path");

  ScopedTempDir tmp;
  const std::string &dirname = tmp.path();
  std::string filename    = tmp.file("input");
  std::string outfilename = tmp.file("input.out");
  {
    std::ofstream ofs(filename);
    ofs << "c t wmc\n";
    ofs << TseytinCNF(g, false);
    for(gate_t in : inputs) {
      int id = static_cast<int>(in) + 1;
      ofs << "c p weight " << id << ' ' << getProb(in)        << " 0\n";
      ofs << "c p weight -" << id << ' ' << (1.0 - getProb(in)) << " 0\n";
    }
  }
  // Flags:
  //  -WE        enable weighted MC with arbitrary precision floats
  //  -decot 1   run flowcutter for at most 1s (demo-friendly; for
  //             larger instances bump via tool-specific args)
  //  -decow 100 weight of the TD in branching heuristic
  //  -tmpdir    where flowcutter writes scratch files
  //  -cs 3500   cache size limit in MB
  //  -prec 20   digits of precision in the printed weighted count
  //
  // sharpsat-td invokes its FlowCutter helper via the relative path
  // `./flow_cutter_pace17`, so we `cd` into the directory where that
  // helper lives before launching the counter. The Tseytin file
  // (-tmpdir, input) is passed as an absolute path so the cwd change
  // doesn't break it.
  std::string cmdline =
      "cd \"$(dirname \"$(command -v flow_cutter_pace17)\")\" && "
      "sharpsat-td -WE -decot 1 -decow 100 -tmpdir " + dirname
      + " -cs 3500 -prec 20 " + filename
      + " > " + outfilename + " 2>&1";

  int retvalue = run_external_tool(cmdline);
#ifndef TDKC
  if(WIFSIGNALED(retvalue) && WTERMSIG(retvalue) == SIGINT) {
    InterruptPending = true;
    QueryCancelPending = true;
  }
#endif
  CHECK_FOR_INTERRUPTS();
  if(retvalue)
    throw CircuitException(format_external_tool_status(retvalue, "sharpsat-td"));

  // Parse: `c s exact arb float <value>` per MCC 2024 weighted output.
  std::ifstream ifs(outfilename);
  std::string line, value;
  while(std::getline(ifs, line)) {
    if(line.rfind("c s exact", 0) == 0) {
      std::stringstream ss(line);
      std::string tok;
      while(ss >> tok) value = tok;
    }
  }
  ifs.close();
  if(value.empty())
    throw CircuitException("sharpsat-td: could not find 'c s exact' line");

  double ret;
  auto slash = value.find('/');
  if(slash != std::string::npos) {
    double num = std::stod(value.substr(0, slash));
    double den = std::stod(value.substr(slash + 1));
    ret = (den == 0.0) ? 0.0 : num / den;
  } else {
    ret = std::stod(value);
  }

  // flowcutter may have left additional scratch files in dirname;
  // rmdir failures inside the ScopedTempDir destructor are silently
  // swallowed.
  if(provsql_verbose >= 20)
    tmp.keep();
  (void)dirname;
  return ret;
}

double BooleanCircuit::DPMC(gate_t g, std::string /*opt*/) const {
  // DPMC (Dudek, Phan, Vardi, CP 2020): two-stage pipeline.
  //   1. htb (planner) reads CNF + weights, emits a project-join tree
  //   2. dmc  (executor) consumes the tree + CNF, returns the count
  // We assume both binaries are installed as `htb` and `dmc` on PATH,
  // and pipe the planner's stdout into dmc's stdin.
  if(find_external_tool("htb").empty() || find_external_tool("dmc").empty())
    throw CircuitException(
            "DPMC needs 'htb' (planner) and 'dmc' (executor) on PATH; "
            "install both from vardigroup/DPMC or add their directory "
            "to provsql.tool_search_path");

  ScopedTempDir tmp;
  std::string filename    = tmp.file("input.cnf");
  std::string outfilename = tmp.file("input.cnf.out");
  {
    std::ofstream ofs(filename);
    ofs << "c t wmc\n";
    ofs << TseytinCNF(g, false);
    for(gate_t in : inputs) {
      int id = static_cast<int>(in) + 1;
      ofs << "c p weight " << id << ' ' << getProb(in)        << " 0\n";
      ofs << "c p weight -" << id << ' ' << (1.0 - getProb(in)) << " 0\n";
    }
  }
  // htb emits a project-join tree on stdout; dmc reads it from stdin
  // (no flag for the join-tree input; the help line says explicitly
  // "Diagram Model Counter (reads join tree from stdin)").
  std::string cmdline =
      "htb --cf=" + filename + " | dmc --cf=" + filename
      + " > " + outfilename + " 2>&1";

  int retvalue = run_external_tool(cmdline);
#ifndef TDKC
  if(WIFSIGNALED(retvalue) && WTERMSIG(retvalue) == SIGINT) {
    InterruptPending = true;
    QueryCancelPending = true;
  }
#endif
  CHECK_FOR_INTERRUPTS();
  if(retvalue)
    throw CircuitException(format_external_tool_status(retvalue, "dpmc"));

  // DMC's output format: look for `s wmc ...` or `c s ...` lines.
  // The actual line shape varies by version; we accept either the
  // MCC `c s exact arb float <value>` form or DMC's older `s wmc N`
  // form. The line we want has a parseable last token.
  std::ifstream ifs(outfilename);
  std::string line, value;
  while(std::getline(ifs, line)) {
    if(line.rfind("c s exact", 0) == 0
       || line.rfind("s wmc", 0) == 0
       || line.rfind("s SATISFIABLE", 0) == 0) {
      std::stringstream ss(line);
      std::string tok;
      while(ss >> tok) value = tok;
    }
  }
  ifs.close();
  if(value.empty() || value == "SATISFIABLE" || value == "UNSATISFIABLE")
    throw CircuitException("dpmc: could not find a parseable count line");

  double ret;
  auto slash = value.find('/');
  if(slash != std::string::npos) {
    double num = std::stod(value.substr(0, slash));
    double den = std::stod(value.substr(slash + 1));
    ret = (den == 0.0) ? 0.0 : num / den;
  } else {
    ret = std::stod(value);
  }

  if(provsql_verbose >= 20)
    tmp.keep();
  return ret;
}

double BooleanCircuit::WeightMC(gate_t g, std::string opt) const {
  //opt of the form 'delta;epsilon'
  std::stringstream ssopt(opt);
  std::string delta_s, epsilon_s;
  getline(ssopt, delta_s, ';');
  getline(ssopt, epsilon_s, ';');

  double delta = 0;
  try {
    delta=stod(delta_s);
  } catch (std::invalid_argument &) {
    delta=0;
  }
  double epsilon = 0;
  try {
    epsilon=stod(epsilon_s);
  } catch (std::invalid_argument &) {
    epsilon=0;
  }
  if(delta == 0) delta=0.2;
  if(epsilon == 0) epsilon=0.8;

  //TODO calcul numIterations

  //calcul pivotAC
  const double pivotAC=2*ceil(exp(3./2)*(1+1/epsilon)*(1+1/epsilon));

  if(find_external_tool("weightmc").empty())
    throw CircuitException(
            "weightmc not found on PATH; install it or add its "
            "directory to provsql.tool_search_path");

  ScopedTempDir tmp;
  std::string filename    = tmp.file("input");
  std::string outfilename = tmp.file("input.out");
  {
    std::ofstream ofs(filename);
    ofs << TseytinCNF(g, true);
  }

  std::string cmdline="weightmc --startIteration=0 --gaussuntil=400 --verbosity=0 --pivotAC="+std::to_string(pivotAC)+ " "+filename+" > "+outfilename;

  int retvalue=run_external_tool(cmdline);
  if(retvalue) {
    throw CircuitException(format_external_tool_status(retvalue, "weightmc"));
  }

  //parsing
  std::ifstream ifs(outfilename.c_str());
  std::string line, prev_line;
  while(getline(ifs,line))
    prev_line=line;

  std::stringstream ss(prev_line);
  std::string result;
  ss >> result >> result >> result >> result >> result;

  std::istringstream iss(result);
  std::string val, exp;
  getline(iss, val, 'x');
  getline(iss, exp);
  double value=stod(val);
  exp=exp.substr(2);
  double exponent=stod(exp);
  double ret=value*(pow(2.0,exponent));

  if(provsql_verbose >= 20)
    tmp.keep();

  return ret;
}

double BooleanCircuit::independentEvaluationInternal(
  gate_t g, std::set<gate_t> &seen) const
{
  double result=1.;

  switch(getGateType(g)) {
  case BooleanGate::AND:
    for(const auto &c: getWires(g)) {
      result*=independentEvaluationInternal(c, seen);
    }
    break;

  case BooleanGate::OR:
  {
    // We collect probability among each group of children, where we
    // group MULIN gates with the same key var together
    std::map<gate_t, double> groups;
    std::set<gate_t> local_mulins;
    std::set<std::pair<gate_t, unsigned> > mulin_seen;

    for(const auto &c: getWires(g)) {
      auto group = c;
      if(getGateType(c) == BooleanGate::MULIN) {
        group = *getWires(c).begin();
        if(local_mulins.find(group)==local_mulins.end()) {
          if(seen.find(group)!=seen.end())
            throw CircuitException("Not an independent circuit");
          else
            seen.insert(group);
          local_mulins.insert(group);
        }
        auto p = std::make_pair(group, getInfo(c));
        if(mulin_seen.find(p)==mulin_seen.end()) {
          groups[group] += getProb(c);
          mulin_seen.insert(p);
        }
      } else
        groups[group] = independentEvaluationInternal(c, seen);
    }

    for(const auto [k, v]: groups)
      result *= 1-v;
    result = 1-result;
  }
  break;

  case BooleanGate::NOT:
    result=1-independentEvaluationInternal(*getWires(g).begin(), seen);
    break;

  case BooleanGate::IN:
  {
    /* A leaf with probability 0 or 1 is a constant : it carries no
     * Boolean variable that can collide with another occurrence of
     * itself.  Skip the seen-set bookkeeping so circuits where the
     * shared subgraphs are all constants (e.g. RangeCheck-resolved
     * comparators flowing through a non-tree structure, or
     * user-flipped Bernoullis pinned to 0 / 1) stay evaluable under
     * the read-once `independent` method.  Anything strictly between
     * 0 and 1 is a real Bernoulli variable and must remain
     * read-once. */
    const double p = getProb(g);
    if (p == 0.0 || p == 1.0) {
      result = p;
      break;
    }
    if(seen.find(g)!=seen.end())
      throw CircuitException("Not an independent circuit");
    seen.insert(g);
    result=p;
  }
  break;

  case BooleanGate::MULIN:
  {
    auto child = *getWires(g).begin();
    if(seen.find(child)!=seen.end())
      throw CircuitException("Not an independent circuit");
    seen.insert(child);
    result=getProb(g);
  }
  break;

  case BooleanGate::UNDETERMINED:
  case BooleanGate::MULVAR:
    throw CircuitException("Bad gate");
  }

  return result;
}

double BooleanCircuit::independentEvaluation(gate_t g) const
{
  std::set<gate_t> seen;
  return independentEvaluationInternal(g, seen);
}

void BooleanCircuit::setInfo(gate_t g, unsigned int i)
{
  info[g] = i;
}

unsigned BooleanCircuit::getInfo(gate_t g) const
{
  auto it = info.find(g);

  if(it==info.end())
    return 0;
  else
    return it->second;
}

void BooleanCircuit::rewriteMultivaluedGatesRec(
  const std::vector<gate_t> &muls,
  const std::vector<double> &cumulated_probs,
  unsigned start,
  unsigned end,
  std::vector<gate_t> &prefix)
{
  if(start==end) {
    getWires(muls[start]) = prefix;
    return;
  }

  unsigned mid = (start+end)/2;
  // cumulated_probs is an *inclusive* prefix sum (cumulated_probs[i] =
  // p[0]+...+p[i]).  The conditional probability of being in the left
  // half [start..mid] given the range [start..end] is therefore
  //   (cum[mid] - cum[start-1]) / (cum[end] - cum[start-1])
  // with cum[-1] treated as 0 when start==0.
  double prev_start = (start == 0) ? 0. : cumulated_probs[start - 1];
  auto g = setGate(
    BooleanGate::IN,
    (cumulated_probs[mid] - prev_start) /
    (cumulated_probs[end] - prev_start));
  auto not_g = setGate(BooleanGate::NOT);
  getWires(not_g).push_back(g);

  prefix.push_back(g);
  rewriteMultivaluedGatesRec(muls, cumulated_probs, start, mid, prefix);
  prefix.pop_back();
  prefix.push_back(not_g);
  rewriteMultivaluedGatesRec(muls, cumulated_probs, mid+1, end, prefix);
  prefix.pop_back();
}

/**
 * @brief Check whether two double values are approximately equal.
 * @param a  First value.
 * @param b  Second value.
 * @return   @c true if @p a and @p b differ by less than 10× machine epsilon.
 */
static constexpr bool almost_equals(double a, double b)
{
  double diff = a - b;
  constexpr double epsilon = std::numeric_limits<double>::epsilon() * 10;

  return (diff < epsilon && diff > -epsilon);
}

void BooleanCircuit::rewriteMultivaluedGates()
{
  std::map<gate_t,std::vector<gate_t> > var2mulinput;
  for(auto mul: mulinputs) {
    var2mulinput[*getWires(mul).begin()].push_back(mul);
  }
  mulinputs.clear();

  for(const auto &[var, muls]: var2mulinput)
  {
    const unsigned n = muls.size();
    std::vector<double> cumulated_probs(n);
    double cumulated_prob=0.;

    for(unsigned i=0; i<n; ++i) {
      cumulated_prob += getProb(muls[i]);
      cumulated_probs[i] = cumulated_prob;
      gates[static_cast<std::underlying_type<gate_t>::type>(muls[i])] = BooleanGate::AND;
      getWires(muls[i]).clear();
    }

    std::vector<gate_t> prefix;
    prefix.reserve(static_cast<unsigned>(log(n)/log(2)+2));
    if(!almost_equals(cumulated_probs[n-1],1.)) {
      prefix.push_back(setGate(BooleanGate::IN, cumulated_probs[n-1]));
    }
    rewriteMultivaluedGatesRec(muls, cumulated_probs, 0, n-1, prefix);
  }
}

gate_t BooleanCircuit::interpretAsDDInternal(gate_t g, std::set<gate_t> &seen, dDNNF &dd) const {
  gate_t dg{0};

  switch(getGateType(g)) {
  case BooleanGate::AND:
  {
    dg = dd.setGate(BooleanGate::AND);
    for(const auto &c: getWires(g)) {
      auto dc = interpretAsDDInternal(c, seen, dd);
      dd.addWire(dg, dc);
    }
  }
  break;

  case BooleanGate::OR:
  {
    dg = dd.setGate(BooleanGate::NOT);
    auto dng = dd.setGate(BooleanGate::AND);
    dd.addWire(dg, dng);
    for(const auto &c: getWires(g)) {
      auto dc = interpretAsDDInternal(c, seen, dd);
      auto dnc = dd.setGate(BooleanGate::NOT);
      dd.addWire(dnc, dc);
      dd.addWire(dng, dnc);
    }
  }
  break;

  case BooleanGate::NOT:
  {
    dg = dd.setGate(BooleanGate::NOT);
    auto dc = interpretAsDDInternal(getWires(g)[0], seen, dd);
    dd.addWire(dg, dc);
  }
  break;

  case BooleanGate::IN:
    if(seen.find(g)!=seen.end())
      throw CircuitException("Not an independent circuit");
    seen.insert(g);
    if(getUUID(g).empty())
      dg = dd.setGate(BooleanGate::IN, getProb(g));
    else
      dg = dd.setGate(getUUID(g), BooleanGate::IN, getProb(g));
    break;

  case BooleanGate::MULIN:
  case BooleanGate::MULVAR:
  case BooleanGate::UNDETERMINED:
    throw CircuitException("Unsupported gate in interpretAsDD");
  }

  return dg;
}

dDNNF BooleanCircuit::interpretAsDD(gate_t g) const
{
  dDNNF dd;
  std::set<gate_t> seen;

  dd.setRoot(interpretAsDDInternal(g, seen, dd));

  // The OR-as-NOT(AND(NOT, ...)) De Morgan rewriting above introduces
  // many redundant NOT-NOT pairs and single-child AND/OR gates that the
  // canonical simplify pass folds away; matches what the external-KC
  // and tree-decomposition paths already do.
  dd.simplify();

  return dd;
}

dDNNF BooleanCircuit::makeDD(gate_t g, const std::string &method, const std::string &args) const
{
  if(method=="compilation") {
    return compilation(g, args);
  } else if(method=="tree-decomposition") {
    try {
      TreeDecomposition td(*this);
      return dDNNFTreeDecompositionBuilder{
        *this, g, td}.build();
    } catch(TreeDecompositionException &) {
      provsql_error("Treewidth greater than %u", TreeDecomposition::MAX_TREEWIDTH);
    }
  } else if(method=="interpret-as-dd") {
    return interpretAsDD(g);
  } else {
    dDNNF dd;
    try {
      dd = interpretAsDD(g);
      if(provsql_verbose>=20)
        provsql_notice("Circuit interpreted as dD, %ld gates", dd.getNbGates());
    } catch(CircuitException &) {
      try {
        TreeDecomposition td(*this);
        dd = dDNNFTreeDecompositionBuilder{
          *this, g, td}.build();
        if(provsql_verbose>=20)
          provsql_notice("dD obtained by tree decomposition, %ld gates", dd.getNbGates());
      } catch(TreeDecompositionException &) {
        dd = compilation(g, "d4");
        if(provsql_verbose>=20)
          provsql_notice("dD obtained by compilation using d4, %ld gates", dd.getNbGates());
      }
    }

    return dd;
  }
}
