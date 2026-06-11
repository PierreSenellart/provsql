/**
 * @file probability_evaluate.cpp
 * @brief SQL function @c provsql.probability_evaluate() – probabilistic circuit evaluation.
 *
 * Implements @c provsql.probability_evaluate(), which computes the
 * probability that a provenance circuit evaluates to @c true under the
 * tuple-independent probabilistic-database model.
 *
 * The @p method argument selects the computation algorithm:
 * - @c "possible-worlds": exact enumeration of all 2^n worlds.
 * - @c "monte-carlo": approximate via random sampling (fast, inexact).
 * - @c "weightmc": approximate using the @c weightmc model counter.
 * - @c "tree-decomposition": exact via tree-decomposition-based d-DNNF.
 * - @c "independent": exact evaluation for disconnected circuits.
 * - @c "inversion-free": exact via the structured-d-DNNF builder over the
 *   query-derived order; errors unless the root carries an inversion-free
 *   certificate.  The default method also tries it (after @c "independent")
 *   when a certificate is present.
 * - Any external compiler name (@c "d4", @c "c2d", @c "minic2d", @c "dsharp").
 *
 * A SIGINT signal sets a process-local flag that causes the evaluation
 * to abort and return @c NULL (used when the user cancels a long-running
 * probability computation).
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "funcapi.h"               // get_call_result_type, BlessTupleDesc
#include "access/htup_details.h"   // heap_form_tuple
#include "provsql_shmem.h"
#include "provsql_utils.h"
#include "utils/guc.h"

PG_FUNCTION_INFO_V1(probability_evaluate);
PG_FUNCTION_INFO_V1(probability_bounds);
}

#include "c_cpp_compatibility.h"
#include <set>
#include <stack>
#include <map>
#include <unordered_map>
#include <limits>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <string>
#include <sstream>
#include <cctype>

#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "GenericCircuit.h"
#include "AnalyticEvaluator.h"
#include "CountCmpEvaluator.h"
#include "MinMaxCmpEvaluator.h"
#include "SumCmpEvaluator.h"
#include "AggMarginalEvaluator.h"
#include "HybridEvaluator.h"
#include "RangeCheck.h"
#include "MonteCarloSampler.h"
#include "dDNNFTreeDecompositionBuilder.h"
#include "TreeDecomposition.h"
#include "StructuredDNNF.h"
#include "DTree.h"
#include "ProbabilityMethod.h"
#include "having_semantics.hpp"
#include "provsql_mmap.h"
#include "safe_query_cert.h"
#include "provsql_utils_cpp.h"
#include "tool_registry_sync.h"
#include "semiring/BoolExpr.h"
#include "mobius_evaluate.h"

using namespace std;

namespace {

// ---------------------------------------------------------------------------
// Möbius-route probability sweep (safe-UCQ Möbius-inversion route).
//
// The Möbius compiler (mobius_evaluate.cpp) materialises a circuit rooted at a
// gate_mobius signed combination over certified-independent Boolean islands.
// Its probability is a single linear sweep: the Boolean islands evaluate
// read-once (independent OR / AND), and at each gate_mobius the children's
// probabilities are summed with the stored integer coefficients.  gate_mobius
// nodes nest (an inner MobiusStep is a child of an outer separator's
// independent-OR), so one unified recursion walks the whole mixed circuit.
// ---------------------------------------------------------------------------
double mobiusEvalRec(GenericCircuit &gc, gate_t g, std::map<gate_t,double> &memo)
{
  auto it = memo.find(g);
  if(it != memo.end()) return it->second;
  CHECK_FOR_INTERRUPTS();
  double r;
  switch(gc.getGateType(g)) {
  case gate_input:    r = gc.getProb(g); break;
  case gate_one:      r = 1.0; break;
  case gate_zero:     r = 0.0; break;
  case gate_times: {
    r = 1.0;
    for(gate_t c : gc.getWires(g)) r *= mobiusEvalRec(gc, c, memo);
    break;
  }
  case gate_plus: {          // independent OR (read-once by construction)
    double pn = 1.0;
    for(gate_t c : gc.getWires(g)) pn *= (1.0 - mobiusEvalRec(gc, c, memo));
    r = 1.0 - pn;
    break;
  }
  case gate_monus: {         // monus(one, x) = NOT x
    const auto &w = gc.getWires(g);
    if(w.size()!=2)
      throw CircuitException("mobius: malformed monus gate");
    r = mobiusEvalRec(gc, w[0], memo) - mobiusEvalRec(gc, w[1], memo);
    if(r < 0.) r = 0.;
    break;
  }
  case gate_mobius: {        // signed Möbius combination
    const auto &w = gc.getWires(g);
    const std::string extra = gc.getExtra(g);
    // extra is a space-separated list of "uuid:coeff": coefficients keyed by
    // child UUID, so wire order / dedup does not matter.
    std::map<std::string,long> co;
    {
      std::size_t i = 0;
      while(i < extra.size()) {
        while(i < extra.size() && (extra[i]==' '||extra[i]=='\t')) ++i;
        if(i >= extra.size()) break;
        std::size_t j = i;
        while(j < extra.size() && extra[j]!=' ' && extra[j]!='\t') ++j;
        const std::string tok = extra.substr(i, j-i);
        const std::size_t colon = tok.rfind(':');
        if(colon != std::string::npos)
          co[tok.substr(0,colon)] =
            std::strtol(tok.substr(colon+1).c_str(), nullptr, 10);
        i = j;
      }
    }
    double v = 0.;
    for(std::size_t i=0;i<w.size();++i) {
      const std::string u = uuid2string(string2uuid(gc.getUUID(w[i])));
      auto cit = co.find(u);
      if(cit == co.end())
        throw CircuitException("mobius: a gate_mobius child has no coefficient");
      v += static_cast<double>(cit->second) * mobiusEvalRec(gc, w[i], memo);
    }
    // The true value is a probability; a small fp excursion is folded away,
    // a larger one means a compiler bug -- warn (a free sanity check).
    constexpr double tol = 1e-9;
    if(v < -tol || v > 1.0 + tol)
      provsql_warning("mobius: signed combination left [0,1] before clamping "
                      "(value %g) -- possible compiler bug", v);
    if(v < 0.) v = 0.; else if(v > 1.) v = 1.;
    r = v;
    break;
  }
  default:
    throw CircuitException("mobius: unsupported gate type in the Möbius-route "
                           "circuit");
  }
  memo[g] = r;
  return r;
}

double mobiusProbabilityImpl(pg_uuid_t token)
{
  // Load the circuit RAW: the load-time simplifier (foldSemiringIdentities /
  // foldBooleanIdentities) rewrites the gate_mobius children's gates, which
  // would break the coefficient<->child association.  The Möbius sweep needs
  // the circuit exactly as the compiler built it.
  const bool s1 = provsql_simplify_on_load;
  const bool s2 = provsql_boolean_provenance;
  const bool s3 = provsql_absorptive_provenance;
  provsql_simplify_on_load = false;
  provsql_boolean_provenance = false;
  provsql_absorptive_provenance = false;
  double r;
  try {
    GenericCircuit gc = getGenericCircuit(token);
    gate_t root = gc.getGate(uuid2string(token));
    std::map<gate_t,double> memo;
    r = mobiusEvalRec(gc, root, memo);
  } catch(...) {
    provsql_simplify_on_load = s1;
    provsql_boolean_provenance = s2;
    provsql_absorptive_provenance = s3;
    throw;
  }
  provsql_simplify_on_load = s1;
  provsql_boolean_provenance = s2;
  provsql_absorptive_provenance = s3;
  return r;
}


/// Trim leading/trailing ASCII spaces and tabs.
string trim_arg(const string &s)
{
  size_t a = s.find_first_not_of(" \t");
  if(a == string::npos)
    return string();
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

/**
 * @brief Parsed probability-method argument string.
 *
 * @c kv holds the @c key=value pairs (key lower-cased, @c eps folded to
 * @c epsilon); @c positional holds the comma-separated tokens that carry no
 * @c '=' -- the historical bare-number (@c monte-carlo) and
 * @c 'delta;epsilon' / @c 'tool;args' (@c weightmc / @c wmc) shortcuts.
 */
struct MethodArgs {
  map<string, string> kv;
  vector<string> positional;
  bool has(const string &k) const { return kv.find(k) != kv.end(); }
  string get(const string &k) const {
    auto it = kv.find(k);
    return it == kv.end() ? string() : it->second;
  }
};

/**
 * @brief Tokenise a probability-method argument string.
 *
 * The grammar is shared by every method: a comma-separated list whose
 * @c key=value items populate @c MethodArgs::kv and whose bare items (no
 * @c '=') go verbatim into @c MethodArgs::positional.  None of the historical
 * shortcuts used a comma, so each survives as a single positional token (a
 * bare integer for @c monte-carlo, @c 'delta;epsilon' for @c weightmc,
 * @c 'tool;args' for @c wmc), letting the canonical @c key=value form and the
 * old form coexist.
 */
MethodArgs parse_method_args(const string &args)
{
  MethodArgs out;
  stringstream ss(args);
  string tok;
  while(getline(ss, tok, ',')) {
    string t = trim_arg(tok);
    if(t.empty())
      continue;
    auto eq = t.find('=');
    if(eq == string::npos) {
      out.positional.push_back(t);
    } else {
      string key = trim_arg(t.substr(0, eq));
      string val = trim_arg(t.substr(eq + 1));
      transform(key.begin(), key.end(), key.begin(),
                [](unsigned char c){ return tolower(c); });
      if(key == "eps")
        key = "epsilon";
      out.kv[key] = val;
    }
  }
  return out;
}

/// Raise if any kv key lies outside @p allowed, naming the method.
void reject_unknown_keys(const MethodArgs &a, const set<string> &allowed,
                         const char *method)
{
  for(const auto &p : a.kv)
    if(allowed.find(p.first) == allowed.end())
      provsql_error("method '%s': unknown argument key '%s'",
                    method, p.first.c_str());
}

/// Parse a non-negative integer that consumes the whole string.
bool parse_ulong_full(const string &v, unsigned long &out)
{
  if(v.empty() || v.find_first_not_of("0123456789") != string::npos)
    return false;
  try {
    size_t pos = 0;
    out = stoul(v, &pos);
    return pos == v.size();
  } catch(const std::exception &) {
    return false;
  }
}

/// Parse a finite double that consumes the whole string.
bool parse_double_full(const string &v, double &out)
{
  if(v.empty())
    return false;
  try {
    size_t pos = 0;
    out = stod(v, &pos);
    return pos == v.size();
  } catch(const std::exception &) {
    return false;
  }
}

/**
 * @brief Validate and read @c epsilon / @c delta from the parsed args.
 *
 * Shared by every approximate method (@c monte-carlo, @c karp-luby,
 * @c weightmc / @c wmc) so the keys, the @c eps alias, and the
 * @c epsilon in (0,1] / @c delta in (0,1) range checks are uniform.  @p eps and
 * @p delta carry the caller's defaults on entry and are overwritten only when
 * the corresponding key is present.
 */
void parse_eps_delta(const MethodArgs &a, const char *method,
                     double &eps, double &delta)
{
  if(a.has("epsilon") && (!parse_double_full(a.get("epsilon"), eps)
                          || eps <= 0. || eps > 1.))
    provsql_error("method '%s': epsilon must be in (0, 1]", method);
  if(a.has("delta") && (!parse_double_full(a.get("delta"), delta)
                        || delta < 0. || delta >= 1.))
    provsql_error("method '%s': delta must be in [0, 1)", method);
}

/**
 * @brief A resolved sampling request: a fixed count, or an @c (eps,delta) target.
 *
 * @c fixed selects @c samples; otherwise the caller turns @c (eps,delta) into a
 * sample count with its own bound (additive for @c monte-carlo, relative for
 * @c karp-luby).
 */
struct SampleSpec {
  bool fixed = false;
  unsigned long samples = 0;
  double eps = 0.1, delta = 0.05;
  bool has_max = false;
  unsigned long max_samples = 0;
};

/**
 * @brief Parse and validate the argument grammar shared by the sampling methods.
 *
 * The grammar is @c samples=N | a bare integer | @c
 * epsilon=E[,delta=D][,max_samples=M].  This routine only resolves *which* path
 * the user asked for and validates the keys / ranges; the @c (eps,delta) -> N
 * conversion is method-specific and applied by the caller.  An empty argument
 * selects the adaptive path with the default @c (eps=0.1, delta=0.05).
 */
SampleSpec parse_sample_spec(const MethodArgs &a, const char *method)
{
  reject_unknown_keys(a, {"samples", "epsilon", "delta", "max_samples"}, method);

  const bool has_samples = a.has("samples") || !a.positional.empty();
  const bool has_adaptive = a.has("epsilon") || a.has("delta");

  if(a.positional.size() > 1)
    provsql_error("method '%s': too many positional arguments", method);
  if(a.has("samples") && !a.positional.empty())
    provsql_error("method '%s': give either samples= or a bare integer, "
                  "not both", method);
  if(has_samples && has_adaptive)
    provsql_error("method '%s': samples is mutually exclusive with "
                  "epsilon/delta", method);
  if(a.has("max_samples") && !has_adaptive)
    provsql_error("method '%s': max_samples applies only to the adaptive "
                  "epsilon/delta path", method);
  if(a.has("delta") && !a.has("epsilon"))
    provsql_error("method '%s': delta requires epsilon", method);

  SampleSpec s;
  if(has_samples) {
    s.fixed = true;
    const string v = a.has("samples") ? a.get("samples") : a.positional[0];
    if(!parse_ulong_full(v, s.samples) || s.samples == 0)
      provsql_error("method '%s': invalid sample count '%s'", method, v.c_str());
    return s;
  }

  parse_eps_delta(a, method, s.eps, s.delta);
  // delta == 0 is a DETERMINISTIC request: valid only on the tolerance paths
  // ('relative'/'additive'), which route it to the d-tree / an exact method.
  // A sampler invoked by name cannot honour it (infinite sample count).
  if(s.delta == 0. && string(method) != "relative"
                   && string(method) != "additive")
    provsql_error("method '%s': delta must be in (0, 1); delta = 0 "
                  "(deterministic) is supported only on the 'relative' / "
                  "'additive' paths, which route to the d-tree or an exact "
                  "method", method);
  if(a.has("max_samples")) {
    s.has_max = true;
    if(!parse_ulong_full(a.get("max_samples"), s.max_samples) || s.max_samples == 0)
      provsql_error("method '%s': invalid max_samples '%s'", method,
                    a.get("max_samples").c_str());
  }
  return s;
}

/// Clamp a real sample count into @c unsigned @c long, honouring @c max_samples.
unsigned long finalize_adaptive(double nd, const SampleSpec &s)
{
  // Clamp only to stay within unsigned long; the count is otherwise honest,
  // and max_samples / query cancel bound the runtime.
  unsigned long n = (nd >= 9e18) ? static_cast<unsigned long>(9e18)
                                 : static_cast<unsigned long>(nd);
  if(n == 0)
    n = 1;
  if(s.has_max && n > s.max_samples)
    n = s.max_samples;
  return n;
}

/// Format a double compactly for the approximation-guarantee notice.
string fmt_num(double x)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%.6g", x);
  return string(buf);
}

/**
 * @brief Emit the machine-readable approximation-guarantee NOTICE (verbose>=5).
 *
 * An approximate method's estimate carries an @c (eps,delta) error guarantee;
 * we surface it as a structured NOTICE that downstream UIs (Studio floors
 * @c verbose_level at 5 for evaluation) parse and render.  @p kind is
 * @c "additive" (@c |est-p| <= eps) or @c "relative" (@c est within a
 * @c 1±eps factor of @c p), each holding with probability @c >= 1-delta.
 * Optional fields are omitted when not applicable: @p delta @c < 0,
 * @p samples @c == 0, @p clauses @c < 0, @p tool empty.  Gated on
 * @c verbose_level>=5 so plain SQL evaluation (and the regression suite) stay
 * quiet by default.
 */
void emit_guarantee(const char *kind, double eps, double delta,
                    unsigned long samples, long clauses, const char *tool)
{
  if(provsql_verbose < 5)
    return;
  string msg = "approximation-guarantee: kind=" + string(kind)
             + " eps=" + fmt_num(eps);
  if(delta >= 0.)
    msg += " delta=" + fmt_num(delta);
  if(samples > 0)
    msg += " samples=" + std::to_string(samples);
  if(clauses >= 0)
    msg += " clauses=" + std::to_string(clauses);
  if(tool && *tool)
    msg += " tool=" + string(tool);
  provsql_notice("%s", msg.c_str());
}

/// Whether a @c wmc tool is an approximate (multiplicative-guarantee) counter.
bool is_approx_wmc_tool(const string &tool)
{
  return tool == "weightmc" || tool == "approxmc";
}

/// Extract weightmc's epsilon from a @c wmcCount opt string (@c "delta;epsilon").
double eps_from_wmc_opt(const string &opt)
{
  auto semi = opt.find(';');
  if(semi == string::npos)
    return 0.8;   // no epsilon field: wmcCount's own default tolerance
  double e;
  return (parse_double_full(opt.substr(semi + 1), e) && e > 0.) ? e : 0.8;
}

/**
 * @brief Resolve the @c monte-carlo sample count from the parsed args.
 *
 * A fixed @c samples=N (or bare integer), or an *additive* @c (eps,delta)
 * guarantee: the sample mean of the Bernoulli circuit indicator is within
 * @c eps of the true probability with probability at least @c 1-delta after
 * @c N = ceil(ln(2/delta) / (2*eps^2)) samples (Hoeffding's inequality, so the
 * count is independent of the probability being estimated).  This is an
 * *absolute* error bound; @c karp-luby provides the *relative* one needed on
 * rare-event outputs.  Also emits the additive guarantee NOTICE.
 */
unsigned long monte_carlo_samples(const MethodArgs &a)
{
  SampleSpec s = parse_sample_spec(a, "monte-carlo");
  unsigned long n = s.fixed
    ? s.samples
    : finalize_adaptive(ceil(log(2.0 / s.delta) / (2.0 * s.eps * s.eps)), s);
  // For a fixed N, report the additive eps achieved at the conventional
  // delta=0.05; for the adaptive path, report the requested (eps,delta).
  const double eps = s.fixed
    ? sqrt(log(2.0 / 0.05) / (2.0 * static_cast<double>(n))) : s.eps;
  const double delta = s.fixed ? 0.05 : s.delta;
  emit_guarantee("additive", eps, delta, n, -1, nullptr);
  return n;
}

/**
 * @brief Build the @c wmcCount opt string (@c "delta;epsilon") from the args.
 *
 * Canonical form @c epsilon=E[,delta=D], validated through the same
 * @c parse_eps_delta as the sampling methods; the positional @c 'delta;epsilon'
 * is accepted as a documented legacy alias (forwarded verbatim).  @c wmcCount
 * reads only @c epsilon (it drives the @c {pivotAC} approximation tolerance), so
 * @c delta is carried for the legacy two-field order but is presently inert for
 * the tool.
 */
string wmc_opt_from_args(const MethodArgs &a, const char *method)
{
  if(!a.positional.empty()) {
    if(a.has("epsilon") || a.has("delta"))
      provsql_error("method '%s': give either the legacy 'delta;epsilon' or "
                    "epsilon=/delta=, not both", method);
    if(a.positional.size() > 1)
      provsql_error("method '%s': too many positional arguments", method);
    return a.positional[0];
  }
  reject_unknown_keys(a, {"epsilon", "delta"}, method);
  // weightmc's own epsilon default (0.8) applies when epsilon is omitted, so
  // validate but emit only the fields the user gave.
  double eps = 0.8, delta = 0.5;
  parse_eps_delta(a, method, eps, delta);
  return (a.has("delta") ? a.get("delta") : string()) + ";"
       + (a.has("epsilon") ? a.get("epsilon") : string());
}

/**
 * @brief Run Karp-Luby on a DNF-shaped circuit and surface its guarantee.
 *
 * Two paths, selected by the shared argument grammar:
 *
 * - A fixed @c samples=N (or bare integer): the *stratified* fixed-budget
 *   estimator (@c BooleanCircuit::karpLuby).  Reports the relative @c eps that
 *   @c N rounds deliver over @p m clauses at the conventional @c delta=0.05.
 * - An adaptive @c (eps,delta) target (the default @c eps=0.1, @c delta=0.05):
 *   the Dagum-Karp-Luby-Ross self-adjusting *stopping rule*
 *   (@c BooleanCircuit::karpLubyStopping), which samples until the accept
 *   count reaches @c Y1 = 1+(1+eps)*4*(e-2)*ln(2/delta)/eps^2 and reports the
 *   exact @c (eps,delta) over the rounds actually run.  The cap defaults to the
 *   fixed worst-case round count @c ceil(Y1*m) (so the adaptive run never costs
 *   more than ~(1+eps) times the old fixed bound) and is overridden by
 *   @c max_samples; hitting it before the target downgrades the guarantee to
 *   the relative @c eps achieved at the spent budget (with a warning).
 *
 * Unlike @c monte-carlo's additive bound, this controls the *relative* error,
 * which is what stays meaningful on rare-event outputs.
 */
double evaluate_karp_luby(const BooleanCircuit &c,
                          const std::vector<gate_t> &clauses,
                          const std::vector<std::set<gate_t> > &supports,
                          const MethodArgs &a)
{
  const size_t m = clauses.size();
  const double e  = exp(1.0);
  const double mm = (m == 0) ? 1. : static_cast<double>(m);
  SampleSpec s = parse_sample_spec(a, "karp-luby");

  if(s.fixed) {
    double r = c.karpLuby(clauses, supports, s.samples);
    const double eps =
      sqrt(4.0 * (e - 2.0) * mm * log(2.0 / 0.05) / static_cast<double>(s.samples));
    emit_guarantee("relative", eps, 0.05, s.samples, static_cast<long>(m), nullptr);
    return r;
  }

  // Adaptive: the self-adjusting stopping rule, capped at ceil(Y1*m) by default.
  const double Y  = 4.0 * (e - 2.0) * log(2.0 / s.delta) / (s.eps * s.eps);
  const double Y1 = 1.0 + (1.0 + s.eps) * Y;
  const unsigned long cap =
    s.has_max ? s.max_samples : finalize_adaptive(ceil(Y1 * mm), s);
  unsigned long used = 0;
  bool reached = false;
  double r = c.karpLubyStopping(clauses, supports, s.eps, s.delta,
                                cap, used, reached);
  if(reached || used == 0) {
    emit_guarantee("relative", s.eps, s.delta, used, static_cast<long>(m), nullptr);
  } else {
    const double eps =
      sqrt(4.0 * (e - 2.0) * mm * log(2.0 / 0.05) / static_cast<double>(used));
    provsql_warning("method 'karp-luby': the stopping rule reached its "
                    "%lu-sample cap before the (epsilon=%g, delta=%g) target; "
                    "reporting the relative guarantee achieved at the samples "
                    "spent", cap, s.eps, s.delta);
    emit_guarantee("relative", eps, 0.05, used, static_cast<long>(m), nullptr);
  }
  return r;
}

} // anonymous namespace

/// External entry point for the Möbius-route probability sweep (declared in
/// mobius_evaluate.h, used by the stats SRF in mobius_evaluate.cpp).
double mobius_probability_of(pg_uuid_t token)
{
  return mobiusProbabilityImpl(token);
}

/**
 * @brief SIGINT handler that sets the global interrupted flag.
 *
 * The signal number argument is required by the @c signal() API but is
 * not used.
 *
 * In addition to the @c provsql_interrupted flag polled by the long
 * Monte-Carlo / possible-worlds evaluation loops, we drive PG's
 * standard cancel pipeline (@c InterruptPending / @c QueryCancelPending
 * + @c SetLatch) the same way PG's own @c StatementCancelHandler does.
 * That makes a SIGINT delivered to the backend (e.g. via
 * @c pg_cancel_backend) outside of a @c system() wait turn into a
 * proper 57014 cancel at the next @c CHECK_FOR_INTERRUPTS instead of
 * being silently absorbed.  (The matching case where an external
 * compiler is running is handled in @c run_external_tool, which runs the
 * tool in its own process group and @c SIGKILLs that group on a pending
 * cancel, then lets @c CHECK_FOR_INTERRUPTS raise it.)
 */
static void provsql_sigint_handler (int)
{
  provsql_interrupted = true;

  if (!proc_exit_inprogress) {
    InterruptPending = true;
    QueryCancelPending = true;
  }
  SetLatch(MyLatch);
}

/**
 * @brief Collect the inversion-free per-input order keys for the structured
 *        builder.
 *
 * Walks the @c GenericCircuit @p gc for @c K-prefixed annotation gates whose
 * child is a @c gate_input (the per-input order markers attached by the planner
 * on the certified path), parses each key, and maps the wrapped input to its
 * @c BooleanCircuit variable via @p gc_to_bc.  Returns @c true only if every
 * @c BooleanCircuit input reachable from @p bc_root carries a key (the
 * structured builder needs a total order over all variables); a missing marker
 * means the certified markers are absent / incomplete and the caller must not
 * use the structured path.
 */
static bool collect_inversion_free_keys(
  const GenericCircuit &gc, gate_t gc_root,
  const std::unordered_map<gate_t, gate_t> &gc_to_bc,
  const BooleanCircuit &c, gate_t bc_root,
  std::map<gate_t, StructuredDNNFBuilder::InputKey> &out)
{
  std::set<gate_t> seen;
  std::stack<gate_t> st;
  st.push(gc_root);
  while (!st.empty()) {
    gate_t g = st.top(); st.pop();
    if (!seen.insert(g).second) continue;
    if (gc.getGateType(g) == gate_annotation) {
      std::string ex = gc.getExtra(g);   // must outlive the parse (k points into it)
      SafeCertKey k;
      if (safe_cert_key_parse(ex.c_str(), &k)) {
        const auto &w = gc.getWires(g);
        if (!w.empty() && gc.getGateType(w[0]) == gate_input) {
          auto it = gc_to_bc.find(w[0]);
          if (it != gc_to_bc.end())
            out[it->second] = StructuredDNNFBuilder::InputKey{
              std::string(k.root, k.root_len),
              std::string(k.sec, k.sec_len), k.factor };
        }
      }
    }
    for (gate_t ch : gc.getWires(g)) st.push(ch);
  }

  /* every Boolean input must be ordered */
  std::set<gate_t> bseen;
  std::stack<gate_t> bst;
  bst.push(bc_root);
  while (!bst.empty()) {
    gate_t g = bst.top(); bst.pop();
    if (!bseen.insert(g).second) continue;
    if (c.getGateType(g) == BooleanGate::IN) {
      if (out.find(g) == out.end())
        return false;
    } else {
      for (gate_t ch : c.getWires(g)) bst.push(ch);
    }
  }
  return true;
}

/**
 * @brief Flatten the per-input order keys into a total rank for the structured
 *        builder's order-only constructor.
 *
 * Sorts the certified inputs into a Prop. 4.5-consistent order -- root-class
 * value first (one independent block per value), then secondary-class value
 * (one tile per value within a block), then the shared self-join guard before
 * the payloads of its tile, then by factor -- and assigns consecutive ranks.
 * Ties (two inputs with identical keys) keep a deterministic order via the
 * input gate id, so distinct variables always get distinct ranks.  Unlike the
 * keyed (factored-sweep) constructor this makes no single-secondary-axis /
 * one-payload-per-tile assumption, so it certifies every hierarchical
 * inversion-free lineage, including the self-join-free case.
 */
static std::map<gate_t, int> inversion_free_rank(
  const std::map<gate_t, StructuredDNNFBuilder::InputKey> &keys)
{
  std::vector<std::pair<gate_t, StructuredDNNFBuilder::InputKey>> v(
    keys.begin(), keys.end());
  std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
    const auto &ka = a.second, &kb = b.second;
    if (ka.root != kb.root) return ka.root < kb.root;
    if (ka.sec  != kb.sec)  return ka.sec  < kb.sec;
    int ga = (ka.factor == StructuredDNNFBuilder::GUARD_FACTOR) ? 0 : 1;
    int gb = (kb.factor == StructuredDNNFBuilder::GUARD_FACTOR) ? 0 : 1;
    if (ga != gb) return ga < gb;
    if (ka.factor != kb.factor) return ka.factor < kb.factor;
    return a.first < b.first;
  });
  std::map<gate_t, int> rank;
  int r = 0;
  for (const auto &p : v) rank[p.first] = r++;
  return rank;
}

dDNNF buildInversionFreeDDNNF(pg_uuid_t token)
{
  // Compile a query certified inversion-free to its structured d-DNNF (the
  // same artefact the 'inversion-free' probability method builds), so the KC
  // surface can render / measure it.  Mirrors the dispatch in
  // probability_evaluate_internal: the per-input order keys live on the
  // GenericCircuit's annotation markers, so we go through the generic circuit
  // rather than getBooleanCircuit(token, ...) directly.
  GenericCircuit gc = getGenericCircuit(token);
  gate_t gc_root = gc.getGate(uuid2string(token));
  std::string ex = gc.getExtra(gc_root);
  if (ex.empty() || ex[0] != SAFE_CERT_EXTRA_PREFIX_RECIPE)
    throw CircuitException("compile 'inversion-free': the provenance root "
                           "carries no inversion-free certificate");
  gate_t root;
  std::unordered_map<gate_t, gate_t> gc_to_bc;
  BooleanCircuit c = getBooleanCircuit(gc, token, root, gc_to_bc);
  std::map<gate_t, StructuredDNNFBuilder::InputKey> keys;
  if (!collect_inversion_free_keys(gc, gc_root, gc_to_bc, c, root, keys))
    throw CircuitException("compile 'inversion-free': the certificate's inputs "
                           "lack per-input order markers");
  return StructuredDNNFBuilder(c, root, inversion_free_rank(keys)).dnnf();
}

// ---------------------------------------------------------------------------
// Probability-method catalog (see ProbabilityMethod.h).
//
// Each historical dispatch branch becomes a ProbabilityMethod object.  Phase 1
// is a behaviour-preserving refactor: chooseAndRun reproduces the
// independent -> inversion-free -> compilation default ladder exactly, and
// byName reproduces each explicit method.  The RV+monte-carlo special case and
// every probability-side pre-pass stay where they are in
// probability_evaluate_internal.
// ---------------------------------------------------------------------------

// Forward declaration: the whole-circuit (eps,delta)-relative stopping-rule
// estimator (defined below probability_evaluate_internal) is delegated to by the
// portfolio's StoppingRuleMethod, which is defined earlier (in the method-catalog
// block).  It operates on the GenericCircuit so it serves Boolean, RV and HAVING
// circuits uniformly.
static void run_stopping_rule(GenericCircuit &gc, gate_t gc_root,
                              const MethodArgs &a, double &result,
                              std::string &actual_method);

namespace provsql {

/// Sanity bound on the reachable-input count for the auto-chosen 2^N
/// possible-worlds enumeration: above it the method drops out of the portfolio
/// so it is never *attempted* (its 2^N cost already deprioritises it, but this
/// guards against a catastrophic last-resort attempt if every cheaper method
/// failed).  The by-name call ignores it (up to possibleWorlds' own 64 limit).
/// The actual small-N-vs-compile crossover is a cost comparison, not this bound.
static const size_t kPossibleWorldsSanityMax = 30;

/// Largest clause count for which the auto-chosen sieve (2^m inclusion-exclusion)
/// is admitted (matches BooleanCircuit::sieve's internal cap).  The by-name call
/// is unaffected.
static const size_t kSieveSanityMaxClauses = 24;

// ---------------------------------------------------------------------------
// Cost-function constants.  Each method's / feature's estimatedCost models its
// actual asymptotic complexity (see doc / the complexity table) times one of
// these constants -- ARBITRARY placeholders for now, to be replaced by measured
// values in the calibration pass.  Parameters (all O(1) on the EvalContext):
// S = circuit_size (gates), N = n_inputs, m = dnf_num_clauses_, w = tw_proxy_,
// Delta = tw_max_degree_.  Constraints the values must keep (so the lazy chooser
// stays correct): a cheap method must be tried before acquiring the feature that
// would only reveal a costlier one, hence
//   C_independent <= C_dnfShape <= C_twProxy
// (a read-once circuit runs 'independent' before paying for either feature), and
// C_compilation > C_possibleWorlds (same 2^N, but compilation is the subprocess
// last resort).
// Calibrated on this machine so that the value of each cost function is roughly
// the number of MILLISECONDS the work takes (order-of-magnitude only -- the goal
// is to know whether a cost is ~1, ~100, ~1e6 ms, not a precise fit).  See the
// calibration instrumentation (provsql.verbose_level >= 50) and doc.
static const double kCostIndependent     = 5e-5;  // O(S):                ~5e-5 * S
static const double kCostInversionFree   = 5e-5;  // O(S + N log N)       (~ independent)
static const double kCostPossibleWorlds  = 3e-6;  // O(S * 2^N):          ~3e-6 * S*2^N
static const double kCostSieve           = 1e-5;  // O(S * 2^m): ~1e-5 (rarely optimal)
// NB: w is the degeneracy LOWER bound, so 2^w under-costs tree-decomposition when
// the true treewidth exceeds it (a dense, low-degeneracy circuit can run far
// slower than predicted).  The constant is calibrated where the proxy is tight
// (tree-like circuits, where tree-decomposition is the right pick anyway).
static const double kCostTreeDecomp      = 7e-4;  // O(S * (Delta^2+2^w)): ~7e-4 * f
// compilation is an external knowledge compiler: a fixed subprocess startup
// plus a compile that, while it exploits structure on easy shapes (~tens of ms
// here), can struggle badly on others (the worst case is exponential).  So we do
// NOT model it as linear -- that is too optimistic; we use a pessimistic
// super-linear S^1.5 above a startup floor, keeping it a strong last resort.
static const double kCostCompilation     = 2e-3;  // ~2e-3 * S^1.5 ms ...
static const double kCostCompilationFloor= 40.0;  // ... but at least ~40 ms (startup + easy compile)
static const double kCostDnfShapeFeature = 2e-6;  // O(S):                ~2e-6 * S
static const double kCostTwProxyFeature  = 3e-4;  // O(S):                ~3e-4 * S
// Approximate portfolio members (relative & additive paths).  Their cost has the
// sample-budget term C = ln(2/delta)/eps^2 times per-sample O(S) work, but the
// CONSTANT is PESSIMISTIC by design: measurement showed the runtimes depend on the
// result probability p and the clause structure -- NOT static features (the same
// lesson as the d-tree).  karp-luby moved 14x (4 -> 52 ms on one DNF) purely with
// p; the stopping rule's Dagum 1/p factor (it draws ~1/p worlds) took a rare-event
// (p~0.06) circuit to 120-470 ms where a p~1 model predicts ~10.  A single
// constant cannot be accurate across p, so these are upper bounds.  The chooser
// then never UNDER-prices a sampler and picks a slow one: it prefers the
// delta-independent d-tree (no 1/p, deterministic) and exact-when-cheaper, and
// falls to a sampler only when it is the sole admissible option (non-DNF
// relative / additive), where it is picked regardless of cost.  Net ordering: a
// cheap exact method (independent ~5e-5*S, a tiny-m sieve) underbids the
// estimators so the path returns EXACT; a DNF approximation goes to the d-tree;
// only a hard non-DNF approximation reaches the samplers.
static const double kCostMonteCarlo      = 1e-5;  // additive:     ~1e-5 * S * C  (p-independent, the clean one)
static const double kCostStoppingRule    = 1e-3;  // relative univ: ~1e-3 * S * C  (pessimistic: covers the 1/p rare-event blow-up)
static const double kCostKarpLuby        = 3e-6;  // relative DNF:  ~3e-6 * S * m * C (pessimistic: covers the p-dependent slow case)
static const double kCostDTreeExact      = 3e-4;  // d-tree exact:  ~3e-4 * S * m (memoised Shannon; pessimistic vs tree-decomp on low tw)
static const double kCostDTreeApprox     = 4e-4;  // d-tree approx: ~4e-4 * S / eps, DELTA-INDEPENDENT (deterministic -> overtakes samplers as delta shrinks)
// Speculative-execution budget conversion: ms per d-tree subproblem (recursion
// entry).  The chooser's budget is in ms (the next-best method's cost); the
// d-tree counts subproblems, so budget_steps = budget_ms / (ms per subproblem).
// The two recursions have different per-step cost (calibrated on the bench): the
// monotone-DNF clause path pays an O(m^2) subsumption sweep per node (~1.4e-3
// ms/step), the general circuit path only a footprint componentise + pivot scan
// (~5e-4 ms/step).  Using the right one keeps the budget honest -- a single
// (smaller) constant under-charged the DNF path and let it run well past the
// fallback's cost instead of bailing.
static const double kCostDTreeMsPerStepDnf      = 1.4e-3;
static const double kCostDTreeMsPerStepGeneral  = 5e-4;

/// 2^k with the exponent clamped to keep the cost finite (a clamped exponent
/// still sorts the method dead last -- it is then a guaranteed fall-through).
static double pow2_clamped(size_t k)
{
  return std::ldexp(1.0, static_cast<int>(std::min<size_t>(k, 60)));
}

/// Per-evaluation circuit state threaded to a method's evaluate().  The Boolean
/// view @c c is built once in probability_evaluate_internal; methods that need
/// the multivalued rewrite trigger it (idempotently) through this context, so
/// the rewrite fires exactly when the historical post-rewrite branch did.
struct EvalContext {
  // Generic-circuit state is needed only by the methods that consult the
  // original circuit (inversion-free, stopping-rule); the d-D-construction
  // portfolio (chooseAndBuildDD / makeDDAuto) works off the Boolean view alone,
  // so these are pointers a Boolean-only caller can leave null.
  GenericCircuit *gc;
  gate_t gc_root;
  pg_uuid_t token;
  BooleanCircuit &c;
  gate_t gate;
  std::unordered_map<gate_t, gate_t> *gc_to_bc;
  bool inv_free_cert;
  const std::string &args;
  bool explicitly_named;            ///< invoked via byName (vs the default chain)
  size_t n_inputs = 0;              ///< input count N (O(1) cost feature)
  size_t circuit_size = 0;          ///< gate count S, the circuit-size parameter (O(1))
  /// Speculative-execution budget: the estimated cost (in the chooser's ms-ish
  /// units) of the next-cheapest admissible method.  A budget-aware method
  /// (currently the d-tree) runs until its own work exceeds this and then throws,
  /// so the chooser drops it and escalates -- bounding wasted work at ~the cost
  /// of the safe fallback.  Infinity = no budget (the method is the last resort,
  /// or budgeting is off).
  double cost_budget = std::numeric_limits<double>::infinity();
  bool multivalued_rewritten = false;
  std::string actual_method;

  void ensureMultivaluedRewritten() {
    if(!multivalued_rewritten) {
      c.rewriteMultivaluedGates();
      multivalued_rewritten = true;
    }
  }

  // Cached DNF-shape feature: just (is-DNF, clause count) via the cheap
  // dnfShapeInfo -- O(circuit), NO per-clause supports.  The chooser ranks sieve
  // from this; the supports (potentially O(m*N)) are built only if sieve /
  // karp-luby actually runs, inside their evaluate().
  mutable bool dnf_computed_ = false;
  mutable bool dnf_ok_ = false;
  mutable std::size_t dnf_num_clauses_ = 0;

  void ensureDnfShape() const {
    if(!dnf_computed_) {
      dnf_ok_ = c.dnfShapeInfo(gate, dnf_num_clauses_);
      dnf_computed_ = true;
    }
  }

  // Cached treewidth proxy: a cheap degeneracy lower bound and the max degree
  // (both from one O(V+E) pass; see TreeDecomposition::degeneracyLowerBound),
  // computed once when the chooser is about to consider tree-decomposition.
  mutable bool tw_computed_ = false;
  mutable unsigned tw_proxy_ = 0;
  mutable unsigned tw_max_degree_ = 0;

  void ensureTreewidthProxy() const {
    if(!tw_computed_) {
      tw_proxy_ = TreeDecomposition::degeneracyLowerBound(c, tw_max_degree_);
      tw_computed_ = true;
    }
  }

  // --- Feature framework (see ProbabilityMethod.h) ---------------------------
  // The chooser acquires non-free features lazily; these model their cost and
  // perform the acquisition.

  /// Heuristic acquisition cost of @p f, in the same work units as a method's
  /// estimatedCost (so the chooser can compare "run this method" against
  /// "acquire this feature").
  double featureCost(Feature f) const {
    switch(f) {
    case Feature::DnfShape:
      return kCostDnfShapeFeature * static_cast<double>(circuit_size);  // O(S)
    case Feature::TreewidthProxy:
      return kCostTwProxyFeature * static_cast<double>(circuit_size);   // O(S)
    }
    return 0.;
  }

  bool hasFeature(Feature f) const {
    switch(f) {
    case Feature::DnfShape: return dnf_computed_;
    case Feature::TreewidthProxy: return tw_computed_;
    }
    return true;
  }

  void acquireFeature(Feature f) {
    switch(f) {
    case Feature::DnfShape: ensureDnfShape(); break;
    case Feature::TreewidthProxy: ensureTreewidthProxy(); break;
    }
  }
};

namespace {

/// Exact, decomposition of disconnected circuits.  Throws when the circuit is
/// not independent, which the default ladder catches to fall through.
class IndependentMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "independent"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool inDefaultChain() const override { return true; }
  // Interprets a mulinput (BID) block natively -- summing the mutually-exclusive
  // alternatives -- so it must see the raw circuit, not the Boolean rewrite.
  bool handlesMultivalued() const override { return true; }
  // O(S): one memoised linear pass over the circuit.
  double estimatedCost(const EvalContext &ctx, const Tolerance &) const override {
    return kCostIndependent * static_cast<double>(ctx.circuit_size);
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    double r = ctx.c.independentEvaluation(ctx.gate);
    ctx.actual_method = "independent";
    return r;
  }
};

/// Exact, structured d-DNNF over an inversion-free certificate.
class InversionFreeMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "inversion-free"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool inDefaultChain() const override { return true; }
  // O(S + N log N): linear structured-d-DNNF build + sorting the per-input keys.
  double estimatedCost(const EvalContext &ctx, const Tolerance &) const override {
    const double N = static_cast<double>(ctx.n_inputs);
    return kCostInversionFree
           * (static_cast<double>(ctx.circuit_size) + N * std::log2(N < 2 ? 2. : N));
  }
  bool applicable(const EvalContext &ctx, const Tolerance &) const override {
    // In the default ladder: only when the certificate is present and the
    // kill-switch is on.  byName ignores applicable() and enforces the explicit
    // rules (hard errors) in evaluate().
    return ctx.inv_free_cert && provsql_inversion_free;
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    if(ctx.explicitly_named && !ctx.inv_free_cert)
      provsql_error("method 'inversion-free' requires an inversion-free "
                    "certificate on the provenance root");
    std::map<gate_t, StructuredDNNFBuilder::InputKey> keys;
    if(!collect_inversion_free_keys(*ctx.gc, ctx.gc_root, *ctx.gc_to_bc, ctx.c,
                                    ctx.gate, keys)) {
      if(ctx.explicitly_named)
        provsql_error("method 'inversion-free': the provenance root carries a "
                      "certificate but its inputs lack per-input order markers");
      // Default-ladder mode: fall through to the next method.
      throw CircuitException("inversion-free: inputs lack per-input order "
                             "markers");
    }
    double r = StructuredDNNFBuilder(ctx.c, ctx.gate, inversion_free_rank(keys))
                 .probability();
    ctx.actual_method = "inversion-free";
    return r;
  }
};

/// The safe-UCQ Möbius-inversion route's evaluation method.  Modelled on
/// 'inversion-free' -- a first-class, by-name-invocable catalog method in the
/// default chain, gated by a feature of the provenance root -- rather than a
/// terminal special-case.  A @c gate_mobius root is a signed combination
/// @f$\sum_i c_i\,P(\text{child}_i)@f$ over certified-independent islands,
/// evaluated by a single linear sweep (@c mobiusProbabilityImpl).  Because that
/// root is not a Boolean gate the circuit never becomes a @c BooleanCircuit, so
/// the dispatcher routes a @c gate_mobius-rooted token straight here (see
/// @c probability_evaluate_internal); @c applicable() also keeps it out of the
/// ordinary chain for Boolean circuits.
class MobiusMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "mobius"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool inDefaultChain() const override { return true; }
  // Linear sweep over the certified-independent islands (the per-element
  // probabilities are read-once); same order as 'independent'.
  double estimatedCost(const EvalContext &ctx, const Tolerance &) const override {
    return kCostIndependent * static_cast<double>(ctx.circuit_size);
  }
  bool applicable(const EvalContext &ctx, const Tolerance &) const override {
    return ctx.gc != nullptr
           && ctx.gc->getGateType(ctx.gc_root) == gate_mobius;
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    if(ctx.gc == nullptr || ctx.gc->getGateType(ctx.gc_root) != gate_mobius)
      provsql_error("method 'mobius' requires a Möbius-route token (a "
                    "gate_mobius signed-combination root)");
    ctx.actual_method = "mobius";
    return mobiusProbabilityImpl(ctx.token);
  }
};

// makeDD's internal interpret-as-dd -> tree-decomposition -> compiler ladder is
// lifted here into three first-class catalog members, so the chooser can see
// and rank the three most cost-divergent exact compilers (linear / treewidth-
// bounded / external subprocess) instead of one opaque "compilation" blob, and
// last_eval_method reports the route actually taken.  makeDD / makeDDByName stay
// for their dD-artifact callers (shapley, compile_to_ddnnf, ddnnf_stats).

/// Exact, interpret the circuit directly as a d-DNNF and read off the
/// probability.  By-name only -- deliberately NOT in the default chain: for a
/// probability *number* this is redundant with (indeed strictly weaker than)
/// 'independent'.  interpretAsDD treats OR as independent-OR (De Morgan over a
/// decomposable AND), AND as a product, and throws "Not an independent circuit"
/// on a shared input -- exactly independentEvaluation's computation -- while
/// also rejecting the multivalued inputs and 0/1 constants that 'independent'
/// accepts.  Since 'independent' runs first in the chain it always wins, so this
/// would be dead there.  Kept as an explicit method for parity with the
/// dD-artifact surfaces / debugging.
class InterpretAsDdMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "interpret-as-dd"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool producesDD() const override { return true; }
  // O(S), the same cost as 'independent' -- for a probability *number* this IS
  // independentEvaluation (interpretAsDD treats OR as independent-OR, AND as a
  // product, throws on a shared input), which is why it stays out of the
  // probability default chain.  In the d-D portfolio it is the cheapest route
  // and the artifact-producing twin of 'independent' (which yields no d-DNNF),
  // so it must carry this cost to be ranked first by chooseAndBuildDD.
  double estimatedCost(const EvalContext &ctx, const Tolerance &) const override {
    return kCostIndependent * static_cast<double>(ctx.circuit_size);
  }
  dDNNF buildDD(EvalContext &ctx) const override {
    dDNNF dd = ctx.c.interpretAsDD(ctx.gate);
    ctx.actual_method = "interpret-as-dd";
    return dd;
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    return buildDD(ctx).probabilityEvaluation();
  }
};

/// Exact d-DNNF via min-fill tree decomposition.  Default-chain member (after
/// inversion-free) and by-name "tree-decomposition".  Throws above the treewidth
/// bound: in the chain that falls through to compilation; an explicit call
/// errors with the treewidth message (mirroring makeDD).
class TreeDecompositionMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "tree-decomposition"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool inDefaultChain() const override { return true; }
  bool producesDD() const override { return true; }
  // Cost/applicability are gated by a cheap degeneracy lower bound on the
  // treewidth: if it already exceeds the build's MAX_TREEWIDTH, the bounded
  // min-fill build would certainly fail, so the method is ruled out (skipped
  // before the costly attempt).  Otherwise the d-DNNF cost is exponential in the
  // treewidth; tw_proxy_ is a lower bound, so 2^tw_proxy * n is an optimistic
  // lower bound on the real cost (the build can still fail if the true treewidth
  // turns out above the bound -- the implicit half of the feature).
  std::vector<Feature> requiredFeatures() const override {
    return {Feature::TreewidthProxy};
  }
  bool applicable(const EvalContext &ctx, const Tolerance &) const override {
    return ctx.tw_proxy_ <= static_cast<unsigned>(TreeDecomposition::MAX_TREEWIDTH);
  }
  // O(S * 2^w): the d-DNNF is exponential in the treewidth (lower-bounded by the
  // degeneracy proxy w), and the min-fill build is poly and bounded by the S
  // factor.  NB an earlier model multiplied in the max degree Delta^2 to charge
  // the build's per-step fill-in -- but for a DNF the root OR's fan-in IS the
  // clause count, so Delta^2 exploded (a 300-clause DNF -> Delta=300 -> cost
  // ~90000x too high) and the chooser fled a 7 ms tree-decomposition for a
  // 1900 ms compilation.  The build is fast even at high fan-in (measured), so
  // Delta is dropped; 2^w (capped at the MAX_TREEWIDTH applicability bound) is
  // the real cost driver.
  double estimatedCost(const EvalContext &ctx, const Tolerance &) const override {
    return kCostTreeDecomp * static_cast<double>(ctx.circuit_size)
           * pow2_clamped(ctx.tw_proxy_);
  }
  dDNNF buildDD(EvalContext &ctx) const override {
    try {
      TreeDecomposition td(ctx.c);
      // Speculative execution: the (poly) min-fill build has now discovered the
      // EXACT treewidth, where the cost estimate above used only the degeneracy
      // LOWER bound (which under-costs).  Before paying the exponential d-DNNF
      // build, recompute the real cost from the discovered width; if it exceeds
      // the next-best method's cost, bail so the chooser escalates -- the
      // build's own MAX_TREEWIDTH cap is the hard ceiling, this is the
      // competitive refinement.  A by-name call runs unbounded.
      if(!ctx.explicitly_named && std::isfinite(ctx.cost_budget)) {
        const double real_cost = kCostTreeDecomp
          * static_cast<double>(ctx.circuit_size)
          * pow2_clamped(td.getTreewidth());
        if(real_cost > ctx.cost_budget)
          throw CircuitException(
            "tree-decomposition: discovered treewidth exceeds the budget");
      }
      dDNNF dd = dDNNFTreeDecompositionBuilder{ctx.c, ctx.gate, td}.build();
      ctx.actual_method = "tree-decomposition";
      return dd;
    } catch(TreeDecompositionException &) {
      if(ctx.explicitly_named)
        provsql_error("Treewidth greater than %u",
                      TreeDecomposition::MAX_TREEWIDTH);
      // Default chain: fall through to the compilation terminal.
      throw CircuitException("tree-decomposition: treewidth above the bound");
    }
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    return buildDD(ctx).probabilityEvaluation();
  }
};

/// Exact d-DNNF via an external knowledge compiler (d4 / c2d / minic2d / dsharp,
/// or a KCMCP server).  Default-chain terminal (after tree-decomposition) and
/// by-name "compilation".  An empty compiler argument auto-selects the
/// highest-preference available compiler (provsql.fallback_compiler / registry);
/// a non-empty @c args names the compiler and its options.
class CompilationMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "compilation"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool inDefaultChain() const override { return true; }
  bool producesDD() const override { return true; }
  // Subprocess: the compilers exploit structure, so the typical cost is the
  // d-DNNF compile (~linear in the serialized circuit) plus a fixed startup, not
  // the 2^N worst case.  Modelled as max(startup_floor, slope * S) ms.  (It is
  // still the last resort: cheaper in-process methods, when they apply, undercut
  // it; when none does, it is the only candidate and runs regardless.)
  double estimatedCost(const EvalContext &ctx, const Tolerance &) const override {
    return std::max(kCostCompilationFloor,
                    kCostCompilation * static_cast<double>(ctx.circuit_size));
  }
  dDNNF buildDD(EvalContext &ctx) const override {
    // On a chooser path (exact / relative / additive) ctx.args carries the
    // path's TOLERANCE string (epsilon=...,delta=...), not a compiler name, so
    // auto-select the compiler.  Only a by-name 'compilation' call passes an
    // explicit compiler in ctx.args.  (Without this, a relative/additive request
    // makes compilation try to use "epsilon=...,delta=..." as a compiler name,
    // which throws -- silently dropping compilation from the chooser and sending
    // the request to a worse method.)
    const std::string compiler =
      ctx.explicitly_named ? ctx.args : std::string();
    std::string used;
    dDNNF dd = ctx.c.compilation(ctx.gate, compiler, &used);
    // Report WHICH compiler ran (e.g. "compilation:d4"), not just "compilation":
    // on a chooser path the tool is auto-selected, so the bare label hid it.
    ctx.actual_method = used.empty() ? "compilation" : "compilation:" + used;
    return dd;
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    return buildDD(ctx).probabilityEvaluation();
  }
};

/// Exact, naive 2^N enumeration.  In the default chain for small circuits only
/// (cheap exact, preferred over tree-decomposition / compilation when N is
/// small); always available by name.
class PossibleWorldsMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "possible-worlds"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool inDefaultChain() const override { return true; }
  // O(S * 2^N): enumerate 2^N worlds, evaluate the circuit (O(S)) in each.
  double estimatedCost(const EvalContext &ctx, const Tolerance &) const override {
    return kCostPossibleWorlds * static_cast<double>(ctx.circuit_size)
           * pow2_clamped(ctx.n_inputs);
  }
  bool applicable(const EvalContext &ctx, const Tolerance &) const override {
    return ctx.n_inputs > 0 && ctx.n_inputs <= kPossibleWorldsSanityMax;
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    // Only flag ignored args for an EXPLICIT `possible-worlds` request; when the
    // chooser auto-picks it on a relative/additive path, the args carry the path's
    // (eps,delta) tolerance, which an exact method legitimately ignores.
    if(ctx.explicitly_named && !ctx.args.empty())
      provsql_warning("Argument '%s' ignored for method possible-worlds",
                      ctx.args.c_str());
    double r = ctx.c.possibleWorlds(ctx.gate);
    ctx.actual_method = "possible-worlds";
    return r;
  }
};

/// Additive Monte Carlo (the non-RV path; the RV path is handled directly on
/// the GenericCircuit in probability_evaluate_internal before the catalog).
class MonteCarloMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "monte-carlo"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Additive; }
  // Additive portfolio member: the universal fixed-sample estimator on the Boolean
  // view, serving the 'additive' path (and any-name).
  bool inDefaultChain() const override { return true; }
  bool isDeterministic() const override { return false; } // (eps,delta) sampler
  // O(S / eps^2 * ln(1/delta)) -- Hoeffding, p-independent.
  double estimatedCost(const EvalContext &ctx, const Tolerance &tol) const override {
    if(tol.epsilon <= 0.) return std::numeric_limits<double>::infinity();
    return kCostMonteCarlo * static_cast<double>(ctx.circuit_size)
           * std::log(2.0 / tol.delta)
           / (tol.epsilon * tol.epsilon);
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    unsigned long samples = monte_carlo_samples(parse_method_args(ctx.args));
    double r = ctx.c.monteCarlo(ctx.gate, static_cast<unsigned>(samples));
    ctx.actual_method = "monte-carlo";
    return r;
  }
};

/// Karp-Luby FPRAS over a DNF-shaped monotone circuit.  Relative portfolio member
/// (chosen on DNFs the cheap exact methods do not resolve) and by-name; runs before
/// the multivalued rewrite (it rejects multivalued inputs anyway).
class KarpLubyMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "karp-luby"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Relative; }
  bool inDefaultChain() const override { return true; }
  bool isDeterministic() const override { return false; } // (eps,delta) sampler
  // Cost / applicability need the DNF-shape feature; the chooser acquires it
  // (lazily) before calling them, so a read-once circuit never pays the walk.
  std::vector<Feature> requiredFeatures() const override {
    return {Feature::DnfShape};
  }
  bool applicable(const EvalContext &ctx, const Tolerance &) const override {
    return ctx.dnf_ok_;
  }
  // O(S*m / eps^2 * ln(1/delta)) -- relative, p-independent (the m clauses
  // replace the 1/p of plain MC).
  double estimatedCost(const EvalContext &ctx, const Tolerance &tol) const override {
    if(tol.epsilon <= 0.) return std::numeric_limits<double>::infinity();
    const double m = static_cast<double>(ctx.dnf_num_clauses_ > 0
                                         ? ctx.dnf_num_clauses_ : 1);
    return kCostKarpLuby * static_cast<double>(ctx.circuit_size) * m
           * std::log(2.0 / tol.delta)
           / (tol.epsilon * tol.epsilon);
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    std::vector<gate_t> clauses;
    std::vector<std::set<gate_t> > supports;
    if(!ctx.c.dnfShape(ctx.gate, clauses, supports)) {
      provsql_warning("method 'karp-luby' applies only to a DNF-shaped circuit "
                      "(a monotone OR-of-ANDs over input leaves); negation, "
                      "comparison, aggregation, random-variable and "
                      "multivalued-input gates are not supported");
      provsql_error("method 'karp-luby' requires a DNF-shaped provenance "
                    "circuit");
    }
    double r = evaluate_karp_luby(ctx.c, clauses, supports,
                                  parse_method_args(ctx.args));
    ctx.actual_method = "karp-luby";
    return r;
  }
};

/// Whole-circuit (eps,delta)-RELATIVE estimate via the Dagum-Karp-Luby-Ross stopping
/// rule -- the universal relative fallback (plain Boolean / RV / HAVING agg alike).
/// Relative portfolio member and by-name ('stopping-rule').  Operates on the
/// GenericCircuit (ctx.gc / ctx.gc_root), so it applies to every circuit regardless
/// of whether the Boolean view built; delegates to run_stopping_rule for the
/// max_samples cap, the relative->additive degradation, and the guarantee NOTICE.
class StoppingRuleMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "stopping-rule"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Relative; }
  bool inDefaultChain() const override { return true; }
  bool isDeterministic() const override { return false; } // (eps,delta) sampler
  // O(S / (p*eps^2) * ln(1/delta)); p is not a static feature, modelled
  // optimistically at p ~ 1 (see the kCost block).  Above karp-luby on DNFs and
  // above plain MC, but below the cheap exact methods so "exact when cheaper" wins.
  double estimatedCost(const EvalContext &ctx, const Tolerance &tol) const override {
    if(tol.epsilon <= 0.) return std::numeric_limits<double>::infinity();
    return kCostStoppingRule * static_cast<double>(ctx.circuit_size)
           * std::log(2.0 / tol.delta)
           / (tol.epsilon * tol.epsilon);
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    double r = 0.;
    run_stopping_rule(*ctx.gc, ctx.gc_root, parse_method_args(ctx.args), r,
                      ctx.actual_method);
    return r;
  }
};

/// Exact inclusion-exclusion over a monotone DNF.  Portfolio member (runs before
/// the multivalued rewrite, like karp-luby; dnfShape rejects multivalued inputs)
/// and by-name.  Work-weighted cost N*2^m in the clause count m: the chooser
/// picks it over possible-worlds when there are fewer clauses than inputs
/// (m < N), and over the compilers when m is small -- yet it stays behind
/// linear-exact 'independent' on a read-once DNF.
class SieveMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "sieve"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool inDefaultChain() const override { return true; }
  // Cost / applicability need the DNF-shape feature; the chooser acquires it
  // (lazily) before calling them, so a read-once circuit never pays the walk.
  std::vector<Feature> requiredFeatures() const override {
    return {Feature::DnfShape};
  }
  bool applicable(const EvalContext &ctx, const Tolerance &) const override {
    return ctx.dnf_ok_ && ctx.dnf_num_clauses_ <= kSieveSanityMaxClauses;
  }
  // O(S * 2^m): inclusion-exclusion over 2^m clause subsets, each a product over
  // the union of supports (bounded by the circuit).
  double estimatedCost(const EvalContext &ctx, const Tolerance &) const override {
    if(!ctx.dnf_ok_)
      return std::numeric_limits<double>::infinity();
    return kCostSieve * static_cast<double>(ctx.circuit_size)
           * pow2_clamped(ctx.dnf_num_clauses_);
  }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    // Auto-chosen on a relative/additive path, the args are the path's (eps,delta)
    // tolerance, legitimately ignored by an exact method -- only warn when sieve was
    // requested explicitly.
    if(ctx.explicitly_named && !ctx.args.empty())
      provsql_warning("Argument '%s' ignored for method sieve",
                      ctx.args.c_str());
    // Build the full clauses + supports now (only paid because sieve was
    // chosen); the cheap feature only validated the shape and counted clauses.
    std::vector<gate_t> clauses;
    std::vector<std::set<gate_t> > supports;
    if(!ctx.c.dnfShape(ctx.gate, clauses, supports)) {
      provsql_warning("method 'sieve' applies only to a DNF-shaped circuit "
                      "(a monotone OR-of-ANDs over input leaves); negation, "
                      "comparison, aggregation, random-variable and "
                      "multivalued-input gates are not supported");
      provsql_error("method 'sieve' requires a DNF-shaped provenance circuit");
    }
    double r = ctx.c.sieve(clauses, supports);
    ctx.actual_method = "sieve";
    return r;
  }
};

/// d-tree: deterministic anytime interval bounds for a monotone DNF (Olteanu-
/// Huang).  Refines the cheap leaf bound by independent-or decomposition and
/// Shannon expansion until the tolerance is met (exact when run to a zero-width
/// interval), returning a certified interval -- no failure probability.  Phase
/// 1: reachable by name only (inDefaultChain() == false), so it does not yet
/// perturb the calibrated auto-chooser; toleranceAdmits still lets an explicit
/// 'd-tree' serve the exact, relative and additive paths.
class DTreeBoundsMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "d-tree"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool inDefaultChain() const override { return true; }
  // isDeterministic() defaults to true: the certified interval carries no
  // failure probability.  That is the d-tree's reason for being in the chain --
  // it is the ONLY non-exact method admissible for a delta == 0 request, and its
  // cost is delta-INDEPENDENT, so it overtakes the (eps,delta) samplers as delta
  // shrinks.  On low treewidth the exact compilers still win on cost; the d-tree
  // is auto-selected for deterministic / low-delta approximation and for exact
  // where the treewidth exceeds tree-decomposition's cap.
  std::vector<Feature> requiredFeatures() const override {
    // DnfShape selects the optimised monotone-DNF clause path and supplies the
    // clause count for the exact cost; a non-DNF circuit uses the general
    // circuit recursion (dtreeBoundsCircuit), so the method is applicable either
    // way -- the feature is a hint, not a gate.
    return {Feature::DnfShape};
  }
  bool applicable(const EvalContext &, const Tolerance &) const override {
    // Applies to any Boolean circuit.  A multivalued (BID) circuit is handled
    // too: handlesMultivalued() is false, so the dispatcher rewrites the blocks
    // to independent Booleans before evaluate() and the general recursion never
    // meets a mulinput (the throw in footprintOf is now only a defensive net).
    return true;
  }
  double estimatedCost(const EvalContext &ctx, const Tolerance &tol) const override {
    const double S = static_cast<double>(ctx.circuit_size);
    // Approximate (DNF or general circuit): the anytime early stop caps the work
    // (and it is delta-independent); grows as eps tightens.  This is the d-tree's
    // edge on a non-DNF circuit -- it returns certified bounds where the exact
    // compilers would do full work.  NB the treewidth proxy is NOT used -- it
    // mispredicts this engine (cliques collapse fast under Shannon + subsumption,
    // low-w cycles do not).
    if(tol.kind != ToleranceKind::Exact && tol.epsilon > 0.)
      return kCostDTreeApprox * S / tol.epsilon;
    // Exact: memoised Shannon compilation, ~S*m.  Pessimistic vs tree-
    // decomposition (tighter constant) on low treewidth, so it is picked for
    // exact only where tree-decomposition bails (treewidth above its cap).  Only
    // the monotone-DNF fast path competes for exact auto-selection; a non-DNF
    // exact request leaves the well-understood compilers (tree-decomposition /
    // d4 / possible-worlds) to choose, with the general recursion reachable
    // by-name -- generalising the *shape* of the bounds engine without retuning
    // the exact cost model (a separate item).
    if(!ctx.dnf_ok_)
      return std::numeric_limits<double>::infinity();
    const double m = static_cast<double>(ctx.dnf_num_clauses_ > 0
                                         ? ctx.dnf_num_clauses_ : 1);
    return kCostDTreeExact * S * m;
  }
  double evaluate(EvalContext &ctx, const Tolerance &tol) const override {
    // Monotone-DNF circuits take the optimised clause path; everything else
    // (negation / EXCEPT, nested AND/OR, arbitrary sharing) takes the general
    // circuit recursion.  Both are the same Olteanu-Huang-Koch anytime engine.
    std::vector<gate_t> clause_roots;
    std::vector<std::set<gate_t> > supports;
    const bool is_dnf = ctx.c.dnfShape(ctx.gate, clause_roots, supports);
    ctx.actual_method = "d-tree";

    // Effective tolerance: a relative/additive PATH supplies it via `tol`; an
    // explicit by-name 'd-tree' is exact, unless an epsilon arg is given, which
    // is read as an additive interval half-width target.
    ToleranceKind kind = tol.kind;
    double eps = tol.epsilon;
    if(kind == ToleranceKind::Exact) {
      MethodArgs a = parse_method_args(ctx.args);
      if(a.has("epsilon")) {
        double dummy_delta = 0.;
        parse_eps_delta(a, "d-tree", eps, dummy_delta); // validates eps in (0,1]
        kind = ToleranceKind::Additive;
      }
    }

    // Tolerance -> absolute interval-width target for the recursion.
    double max_width;
    if(kind == ToleranceKind::Additive && eps > 0.) {
      // Additive eps: half-width <= eps means |est - p| <= eps.
      max_width = 2. * eps;
    } else if(kind == ToleranceKind::Relative && eps > 0.) {
      // Relative eps: with p >= L (the cheap lower bound), a half-width <=
      // eps*L gives |est - p| <= eps*L <= eps*p.  The cheap lower bound is
      // dnfBounds on the DNF path; on the general path it is the leaf bound the
      // recursion returns for a trivially-wide target.
      double l0;
      if(is_dnf) {
        double u0;
        ctx.c.dnfBounds(supports, l0, u0);
      } else {
        l0 = provsql::dtreeBoundsCircuit(ctx.c, ctx.gate, 1.0).lower;
      }
      max_width = 2. * eps * l0;
    } else {
      max_width = 0.; // exact
    }

    // Speculative-execution budget: convert the chooser's ms budget (the
    // next-best method's cost) into a subproblem cap, so the d-tree bails (throws,
    // chooser escalates) rather than blowing up on a high-treewidth circuit its
    // cheap-feature cost estimate mis-rated.  An explicit by-name call runs
    // unbounded (it is the user's deliberate choice, with no chooser fallback);
    // the debug GUC provsql.dtree_max_subproblems imposes an extra hard cap.
    unsigned long budget_steps = 0; // 0 = unbounded
    if(!ctx.explicitly_named && std::isfinite(ctx.cost_budget)) {
      const double ms_per_step = is_dnf ? kCostDTreeMsPerStepDnf
                                        : kCostDTreeMsPerStepGeneral;
      budget_steps = static_cast<unsigned long>(
        std::max(1.0, ctx.cost_budget / ms_per_step));
    }
    if(provsql_dtree_max_subproblems > 0) {
      unsigned long cap = static_cast<unsigned long>(provsql_dtree_max_subproblems);
      budget_steps = (budget_steps == 0) ? cap : std::min(budget_steps, cap);
    }

    unsigned long steps = 0;
    provsql::DTreeInterval iv = is_dnf
      ? provsql::dtreeBounds(ctx.c, std::move(supports), max_width, budget_steps, &steps)
      : provsql::dtreeBoundsCircuit(ctx.c, ctx.gate, max_width, budget_steps, &steps);
    if(provsql_verbose >= 50)
      provsql_notice("calibrate kind=dtree path=%s S=%zu N=%zu steps=%lu budget=%lu",
                     is_dnf ? "dnf" : "circuit", ctx.circuit_size, ctx.n_inputs,
                     steps, budget_steps);
    const double est = 0.5 * (iv.lower + iv.upper);

    // Deterministic certificate (delta = 0) whenever an approximation was
    // actually returned (the interval did not collapse to a point).
    if(iv.upper > iv.lower) {
      const double half = 0.5 * (iv.upper - iv.lower);
      if(kind == ToleranceKind::Relative && est > 0.)
        emit_guarantee("relative", half / est, 0., 0, -1, nullptr);
      else
        emit_guarantee("additive", half, 0., 0, -1, nullptr);
    }
    return est;
  }
};

/// weightmc: backward-compatible alias for the weighted-model-counter path.
class WeightmcMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "weightmc"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Relative; }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    std::string opt = wmc_opt_from_args(parse_method_args(ctx.args), "weightmc");
    emit_guarantee("relative", eps_from_wmc_opt(opt), -1., 0, -1, "weightmc");
    double r = ctx.c.wmcCount(ctx.gate, "weightmc", opt);
    ctx.actual_method = "weightmc";
    return r;
  }
};

/// wmc: any registered weighted model counter, selected by tool=<name>.
class WmcMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "wmc"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Relative; }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    MethodArgs a = parse_method_args(ctx.args);
    std::string tool, tool_args;
    if(a.has("tool") || a.has("epsilon") || a.has("delta")) {
      reject_unknown_keys(a, {"tool", "epsilon", "delta"}, "wmc");
      tool = a.get("tool");
      if(tool.empty() && !a.positional.empty())
        tool = a.positional[0];
      if(tool.empty())
        provsql_error("method 'wmc' requires a tool (tool=<name>)");
      if(a.has("epsilon") || a.has("delta")) {
        double eps = 0.8, delta = 0.5;  // validate ranges uniformly
        parse_eps_delta(a, "wmc", eps, delta);
        tool_args = a.get("delta") + ";" + a.get("epsilon");
      }
    } else {
      auto sep = ctx.args.find(';');
      tool = (sep == std::string::npos) ? ctx.args : ctx.args.substr(0, sep);
      tool_args = (sep == std::string::npos) ? std::string()
                                             : ctx.args.substr(sep + 1);
    }
    if(is_approx_wmc_tool(tool))
      emit_guarantee("relative", eps_from_wmc_opt(tool_args), -1., 0, -1,
                     tool.c_str());
    double r = ctx.c.wmcCount(ctx.gate, tool, tool_args);
    // Report WHICH counter ran (e.g. "wmc:ganak"), mirroring "compilation:d4".
    ctx.actual_method = tool.empty() ? "wmc" : "wmc:" + tool;
    return r;
  }
};

}  // anonymous namespace

void MethodCatalog::registerMethod(std::unique_ptr<ProbabilityMethod> m)
{
  methods_.push_back(std::move(m));
}

// Base implementation: only the producesDD() methods override this.
dDNNF ProbabilityMethod::buildDD(EvalContext &) const
{
  throw CircuitException("method '" + name()
                         + "' does not construct a d-DNNF");
}

dDNNF makeDDAuto(BooleanCircuit &c, gate_t g)
{
  // The d-D portfolio reads only the Boolean view, so the generic-circuit
  // pointers are null (never dereferenced by interpret-as-dd /
  // tree-decomposition / compilation) and the request is the exact path.
  std::string no_args;
  EvalContext ctx{/*gc=*/nullptr, /*gc_root=*/gate_t{}, /*token=*/pg_uuid_t{},
                  c, g, /*gc_to_bc=*/nullptr, /*inv_free_cert=*/false, no_args,
                  /*explicitly_named=*/false,
                  /*n_inputs=*/c.getInputs().size(),
                  /*circuit_size=*/c.getNbGates()};
  return MethodCatalog::instance().chooseAndBuildDD(ctx, Tolerance{});
}

const ProbabilityMethod *MethodCatalog::byName(const std::string &n) const
{
  for(const auto &m : methods_)
    if(m->name() == n)
      return m.get();
  return nullptr;
}

/// Admissibility of a method's guarantee under a requested tolerance.  The paths
/// nest Exact ⊂ Relative ⊂ Additive: a method is admissible iff its guarantee is at
/// least as tight as the request (an exact method serves any path -- "exact when
/// cheaper"; a relative method serves relative & additive; an additive method serves
/// only additive).  This both widens the relative/additive portfolios to the
/// approximate members AND keeps the exact path (which calls chooseAndRun with an
/// Exact tolerance) from ever selecting an approximate method.
static bool toleranceAdmits(ToleranceKind request, ToleranceKind method)
{
  switch(request) {
  case ToleranceKind::Exact:    return method == ToleranceKind::Exact;
  case ToleranceKind::Relative: return method != ToleranceKind::Additive;
  case ToleranceKind::Additive: return true;
  }
  return false;
}

namespace {

/// Uniform-cost search shared by chooseAndRun (returns a probability) and
/// chooseAndBuildDD (returns a d-DNNF artifact).  @p run performs the chosen
/// method's work and returns the result of type @c R (calling @c evaluate or
/// @c buildDD respectively); everything else -- lazy feature acquisition, the
/// cheapest-first ranking, the speculative budget, and dropping a method that
/// throws -- is identical for both, so it lives here once.
template<class R, class Run>
R runPortfolio(EvalContext &ctx, const Tolerance &tol,
               std::vector<const ProbabilityMethod *> portfolio, Run run)
{
  // Each step either RUNS the cheapest ready method or ACQUIRES the cheapest
  // pending feature -- and we acquire a feature only when no ready method is
  // already cheaper than acquiring it (a feature-gated method then costs at
  // least that much anyway).  So a circuit the cheap methods resolve never pays
  // to compute features (dnfShape, later a treewidth proxy) that could not have
  // changed the decision.  A method that throws when attempted is dropped (its
  // implicit feature -- e.g. 'independent' learning the circuit is not
  // independent); the last such error propagates if the portfolio is exhausted.
  std::set<Feature> acquired;
  std::string last_error;
  bool have_last_error = false;

  while(true) {
    // The cheapest ready (all required features acquired) and applicable method,
    // and the set of features still gating the not-ready ones.
    const ProbabilityMethod *best = nullptr;
    double best_cost = std::numeric_limits<double>::infinity();
    double second_cost = std::numeric_limits<double>::infinity();
    std::set<Feature> pending;
    for(const ProbabilityMethod *m : portfolio) {
      bool ready = true;
      for(Feature f : m->requiredFeatures())
        if(acquired.find(f) == acquired.end()) { ready = false; pending.insert(f); }
      if(!ready || !m->applicable(ctx, tol))
        continue;
      double cost = m->estimatedCost(ctx, tol);
      if(cost < best_cost) { second_cost = best_cost; best_cost = cost; best = m; }
      else if(cost < second_cost) { second_cost = cost; }
    }
    // Speculative-execution budget: bound the chosen method's work at the cost of
    // the next-cheapest ready alternative (infinity if it is the only one).  A
    // budget-aware method (the d-tree) bails past this and the catch below drops
    // it to that alternative, so wasted work is at most ~the safe fallback's cost.
    ctx.cost_budget = second_cost;

    // The cheapest feature we could acquire to reveal more method costs.
    bool have_pending = false;
    Feature cheapest_f = Feature::DnfShape;
    double cheapest_fc = std::numeric_limits<double>::infinity();
    for(Feature f : pending) {
      double fc = ctx.featureCost(f);
      if(fc < cheapest_fc) { cheapest_fc = fc; cheapest_f = f; have_pending = true; }
    }

    if(best != nullptr && (!have_pending || best_cost <= cheapest_fc)) {
      // Run the cheapest method: nothing cheaper could be revealed by acquiring
      // a feature first.  A Boolean-only method (handlesMultivalued() == false)
      // gets multivalued / BID blocks rewritten to independent Booleans first;
      // this is the single, declarative enforcement point (idempotent, and a
      // no-op on a circuit with no mulinput gates).
      if(!best->handlesMultivalued())
        ctx.ensureMultivaluedRewritten();
      try {
        // Calibration (provsql.verbose_level >= 50): emit the raw cost parameters
        // and elapsed ms so each kCost can be fit so that cost ~ ms.
        if(provsql_verbose >= 50) {
          auto t0 = std::chrono::steady_clock::now();
          R r = run(best);
          double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
          provsql_notice("calibrate kind=method which=%s S=%zu N=%zu m=%zu w=%u "
                         "D=%u cost=%g ms=%g", best->name().c_str(),
                         ctx.circuit_size, ctx.n_inputs, ctx.dnf_num_clauses_,
                         ctx.tw_proxy_, ctx.tw_max_degree_, best_cost, ms);
          return r;
        }
        return run(best);
      } catch(CircuitException &e) {
        if(provsql_interrupted)
          throw;  // a cancel / timeout -- do not silently try another method
        last_error = e.what();
        have_last_error = true;
        portfolio.erase(std::remove(portfolio.begin(), portfolio.end(), best),
                        portfolio.end());
      }
    } else if(have_pending) {
      if(provsql_verbose >= 50) {
        auto t0 = std::chrono::steady_clock::now();
        ctx.acquireFeature(cheapest_f);
        double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
        provsql_notice("calibrate kind=feature which=%s S=%zu cost=%g ms=%g",
                       cheapest_f == Feature::DnfShape ? "DnfShape"
                                                       : "TreewidthProxy",
                       ctx.circuit_size, cheapest_fc, ms);
      } else {
        ctx.acquireFeature(cheapest_f);
      }
      acquired.insert(cheapest_f);
    } else {
      // No ready method and nothing left to acquire: the portfolio is exhausted.
      if(have_last_error)
        throw CircuitException(last_error);
      throw CircuitException("no applicable probability method in the portfolio");
    }
  }
}

}  // anonymous namespace

double MethodCatalog::chooseAndRun(EvalContext &ctx, const Tolerance &tol) const
{
  std::vector<const ProbabilityMethod *> portfolio;
  for(const auto &m : methods_)
    if(m->inDefaultChain() && toleranceAdmits(tol.kind, m->guaranteeKind())
       // A delta == 0 ("deterministic") request admits only deterministic
       // methods: the (eps,delta) samplers cannot honour delta = 0 (their cost
       // model even masks this by falling back to a finite delta), so they must
       // be excluded by admissibility, not left to lose on cost.
       && (tol.delta > 0. || m->isDeterministic()))
      portfolio.push_back(m.get());
  return runPortfolio<double>(ctx, tol, std::move(portfolio),
    [&](const ProbabilityMethod *m){ return m->evaluate(ctx, tol); });
}

dDNNF MethodCatalog::chooseAndBuildDD(EvalContext &ctx, const Tolerance &tol) const
{
  // The d-D portfolio is the producesDD() methods (interpret-as-dd /
  // tree-decomposition / compilation): all exact, so no tolerance filtering.
  // interpret-as-dd is by-name-only for the probability chain (independent
  // subsumes it there) but IS the cheapest artifact route here, so it is
  // included via producesDD(), not inDefaultChain().
  std::vector<const ProbabilityMethod *> portfolio;
  for(const auto &m : methods_)
    if(m->producesDD())
      portfolio.push_back(m.get());
  return runPortfolio<dDNNF>(ctx, tol, std::move(portfolio),
    [&](const ProbabilityMethod *m){ return m->buildDD(ctx); });
}

const MethodCatalog &MethodCatalog::instance()
{
  static const MethodCatalog cat = [] {
    MethodCatalog c;
    // Exact portfolio (registration order is irrelevant -- the chooser sorts by
    // estimatedCost): independent, inversion-free, possible-worlds (2^N),
    // tree-decomposition, compilation.  interpret-as-dd is NOT in the portfolio
    // -- it is redundant with independent (see InterpretAsDd).
    c.registerMethod(std::make_unique<IndependentMethod>());
    c.registerMethod(std::make_unique<InversionFreeMethod>());
    c.registerMethod(std::make_unique<MobiusMethod>());
    c.registerMethod(std::make_unique<TreeDecompositionMethod>());
    c.registerMethod(std::make_unique<CompilationMethod>());
    c.registerMethod(std::make_unique<PossibleWorldsMethod>());
    c.registerMethod(std::make_unique<SieveMethod>());
    c.registerMethod(std::make_unique<DTreeBoundsMethod>());
    // Approximate portfolio members.  Admissibility (toleranceAdmits) keeps them out
    // of the exact path: monte-carlo (additive) serves only 'additive';
    // karp-luby / stopping-rule (relative) serve 'relative' and 'additive'.
    c.registerMethod(std::make_unique<MonteCarloMethod>());
    c.registerMethod(std::make_unique<KarpLubyMethod>());
    c.registerMethod(std::make_unique<StoppingRuleMethod>());
    // By-name-only methods (out of the auto-chooser): interpret-as-dd is redundant
    // with independent; weightmc / wmc are external subprocess counters needing a
    // tool argument, so they are not auto-spawned on a relative request.
    c.registerMethod(std::make_unique<InterpretAsDdMethod>());
    c.registerMethod(std::make_unique<WeightmcMethod>());
    c.registerMethod(std::make_unique<WmcMethod>());
    return c;
  }();
  return cat;
}

}  // namespace provsql

// ---------------------------------------------------------------------------
// Three-path tolerance surface (exact / relative / additive).
//
// The user grants a tolerance via the method name -- "exact" (alias for the
// empty/default method), "relative" (a (1±eps) guarantee), "additive"
// (|p̂-p| <= eps) -- and the system picks the mechanism, rather than naming an
// algorithm (named methods stay available as the EXPLAIN-level escape hatch).
// Admissibility nests exact ⊂ relative ⊂ additive, so every path returns an
// EXACT value when one is cheaply available ("exact when cheaper"): an exact
// result satisfies any (eps,delta).
//
// The relative/additive estimators run on the GenericCircuit (RV-aware), so they
// live here rather than in the BooleanCircuit catalog; folding them in behind a
// lazy Boolean build is the clean follow-up.
// ---------------------------------------------------------------------------

/// Whole-circuit (eps,delta)-relative estimate via the stopping rule (shared by
/// the explicit 'stopping-rule' method and the 'relative' path's estimator).
///
/// Complexity O(S / (p * eps^2) * ln(1/delta)): the Dagum rule draws ~Y1/p
/// whole-circuit worlds, each an O(S) evalBool.  The 1/p factor makes the cost
/// NOT a priori computable from static features -- a precise cost needs a
/// p-lower-bound feature -- which is why this estimator stays a path fallback
/// rather than a cost-ranked portfolio member for now.
static void run_stopping_rule(GenericCircuit &gc, gate_t gc_root,
                              const MethodArgs &a, double &result,
                              std::string &actual_method)
{
  SampleSpec s = parse_sample_spec(a, "stopping-rule");
  if(s.fixed)
    provsql_error("the relative / stopping-rule estimator is adaptive: give "
                  "epsilon=E[,delta=D][,max_samples=M], not a fixed sample "
                  "count");
  const unsigned long cap = s.has_max ? s.max_samples : 10000000UL;
  unsigned long used = 0;
  bool reached = false;
  result = provsql::monteCarloRVStopping(gc, gc_root, s.eps, s.delta, cap, used,
                                         reached);
  if(reached || used == 0) {
    emit_guarantee("relative", s.eps, s.delta, used, -1, "stopping-rule");
  } else {
    const double eps_add = sqrt(log(2.0 / 0.05) / (2.0 * used));
    provsql_warning("relative estimate: reached the %lu-sample cap before the "
                    "(epsilon=%g, delta=%g) relative target; reporting the "
                    "additive guarantee at the samples spent (the event is "
                    "likely rarer than this budget resolves -- raise "
                    "max_samples)", cap, s.eps, s.delta);
    emit_guarantee("additive", eps_add, 0.05, used, -1, "stopping-rule");
  }
  actual_method = "stopping-rule";
}

/**
 * @brief Core implementation of probability evaluation for a circuit token.
 * @param token   UUID of the root provenance gate.
 * @param method  Evaluation method name (e.g. "independent", "monte-carlo").
 * @param args    Additional arguments for the chosen method.
 * @param isnull  Out-param set to @c true when the result is SQL NULL (a
 *                conditioned token whose evidence has probability zero); may
 *                be @c NULL when the caller does not need null-propagation.
 * @return        Float8 Datum containing the computed probability (undefined
 *                when @p isnull is set to @c true).
 */
static Datum probability_evaluate_internal
  (pg_uuid_t token, const string &method, const string &args, bool *isnull)
{
  if(isnull != nullptr)
    *isnull = false;
  // Load the GenericCircuit once: we need it for the RV-detection
  // dispatch below, and getBooleanCircuit() reuses it internally so we
  // pay no extra cost compared to the previous flow.  Universal
  // cmp-resolution passes (RangeCheck) have already been applied
  // inside getGenericCircuit when the provsql.simplify_on_load GUC is
  // on (the default), so the circuit we receive here is already
  // peephole-pruned for any "always true / always false" comparator.
  GenericCircuit gc = getGenericCircuit(token);
  gate_t gc_root = gc.getGate(uuid2string(token));

  // Conditioning gate (the | / cond operator, uuid carrier): a terminal
  // gate_conditioned with children [target, evidence, joint], joint =
  // times(target, evidence).  Its probability is the conditional
  // P(target ∧ evidence) / P(evidence) = P(joint) / P(evidence).  Both
  // sub-tokens are ordinary semiring gates already in the store (the joint
  // is materialised at construction), so each is evaluated by an ordinary
  // recursive call -- correlation between target and evidence is exact
  // because content-addressing makes a shared base tuple the same input
  // gate in both circuits.  Impossible evidence (P(evidence) = 0) yields
  // SQL NULL.  The gate is terminal: a conditioned token can never be a
  // child of a semiring gate (the constructors refuse it), so we only ever
  // meet it at the root here.
  if(gc.getGateType(gc_root) == gate_conditioned) {
    const auto &w = gc.getWires(gc_root);
    if(w.size() == 2)
      provsql_error("probability_evaluate: this is a conditioned distribution "
                    "(a random_variable / agg_token X | C), not a Boolean "
                    "event; query it with expected / variance / moment / "
                    "support, which report the conditional distribution");
    if(w.size() != 3)
      provsql_error("probability_evaluate: malformed conditioned gate "
                    "(expected 3 children [target, evidence, joint], got %zu)",
                    w.size());
    pg_uuid_t evidence = string2uuid(gc.getUUID(w[1]));
    pg_uuid_t joint    = string2uuid(gc.getUUID(w[2]));
    bool ev_null = false, jt_null = false;
    double pe = DatumGetFloat8(
                  probability_evaluate_internal(evidence, method, args, &ev_null));
    if(ev_null || pe == 0.) {
      if(isnull != nullptr)
        *isnull = true;
      return (Datum) 0;          // impossible (or undefined) evidence -> NULL
    }
    double pj = DatumGetFloat8(
                  probability_evaluate_internal(joint, method, args, &jt_null));
    if(jt_null) {
      if(isnull != nullptr)
        *isnull = true;
      return (Datum) 0;
    }
    double r = pj / pe;
    if(r > 1.) r = 1.; else if(r < 0.) r = 0.;
    PG_RETURN_FLOAT8(r);
  }

  // Möbius-inversion route (safe-UCQ Möbius cancellation): a gate_mobius root
  // is a signed combination Σ_i coeff_i · P(child_i) over certified-independent
  // islands.  It is a non-Boolean measure gate, so it cannot become a
  // BooleanCircuit and is routed straight to the dedicated 'mobius' method --
  // modelled on 'inversion-free' (a first-class, by-name-invocable catalog
  // method), not the gate_conditioned terminal special-case.  The default /
  // exact / empty request and an explicit 'mobius' both run it; any other
  // explicit method is an error (the token is not a Boolean circuit).
  if(gc.getGateType(gc_root) == gate_mobius) {
    const bool is_path =
      method.empty() || method == "default" || method == "exact";
    if(!is_path && method != "mobius")
      provsql_error("method '%s' cannot evaluate a Möbius-route token: it is a "
                    "signed combination over certified-independent islands; use "
                    "'mobius' (the default for such tokens)", method.c_str());
    BooleanCircuit dummy;
    gate_t dummygate{};
    std::unordered_map<gate_t, gate_t> dummymap;
    provsql::EvalContext ctx{&gc, gc_root, token, dummy, dummygate, &dummymap,
                             /*inv_free_cert=*/false, args,
                             /*explicitly_named=*/!is_path, 0, 0};
    PG_RETURN_FLOAT8(
      provsql::MethodCatalog::instance().byName("mobius")->evaluate(
        ctx, provsql::Tolerance{}));
  }

  // Inversion-free tractability certificate: the planner wraps the per-row
  // provenance root in a transparent annotation gate carrying the serialised
  // SafeCert recipe.  Its presence routes the default probability chain through
  // the structured-d-DNNF builder (after independentEvaluation, before
  // tree-decomposition) and is required by the explicit 'inversion-free'
  // method.  The recipe is read here (early, before the simplifier passes); the
  // per-input order keys are collected at the dispatch point, where the
  // GenericCircuit->BooleanCircuit mapping is available.
  bool inv_free_cert = false;
  {
    std::string ex = gc.getExtra(gc_root);
    if (!ex.empty() && ex[0] == SAFE_CERT_EXTRA_PREFIX_RECIPE) {
      SafeCert *cert = safe_cert_parse(ex.c_str());
      if (cert != nullptr && cert->kind == CERT_INVERSION_FREE) {
        inv_free_cert = true;
        // Internal per-evaluation diagnostic (the certificate round-trips from
        // the planner), not a result-comprehension message: keep it at the
        // detector's debug-trace level (>= 30) so it stays out of the level-5
        // floor the Studio eval strip applies.
        if (provsql_verbose >= 30)
          provsql_notice("inversion-free certificate read back from circuit "
                         "root: %d atoms, %d classes, root_class=%d",
                         cert->natoms, cert->nclasses, cert->root_class);
      }
    }
  }

  // Hybrid-evaluator simplifier: constant-fold gate_arith subtrees,
  // drop identity wires (0 from PLUS, 1 from TIMES), and collapse
  // PLUS over independent normals or i.i.d. exponentials into a
  // single gate_rv with the closed-form distribution.  Gated by
  // provsql.hybrid_evaluation (default on) so the unfolded DAG can
  // still be exercised end-to-end through the MC fallback during
  // A/B-testing.  Runs before AnalyticEvaluator so newly-bare normal
  // / Erlang leaves unlock the closed-form CDF on the surrounding
  // cmp gate.  Runs before a re-pass of RangeCheck so that the
  // joint-conjunction pass also benefits from constant folding
  // (e.g. a cmp's `arith(NEG, value:100)` operand becomes a bare
  // `value:-100` that the asRvVsConstCmp shape match accepts).
  if (provsql_hybrid_evaluation) {
    provsql::runHybridSimplifier(gc);
    provsql::runRangeCheck(gc);
  }

  // Hybrid-evaluator island decomposer: handles continuous-island
  // comparators by grouping them via base-RV footprint overlap.
  // Multi-cmp shared-island groups get a joint-distribution table
  // inlined as a mulinput block - via the monotone-shared-scalar
  // fast path (k+1 mulinputs; interval probabilities exact via
  // cdfAt when the shared scalar is a bare gate_rv, MC binning over
  // k+1 intervals when it is a gate_arith composite) when all cmps
  // share an lhs gate_t and have gate_value rhs, falling through to
  // the generic 2^k MC joint table otherwise.  Singleton bare-RV
  // cmps are left for AnalyticEvaluator (closed-form CDF on its own
  // is cheaper than per-cmp MC); singleton gate_arith cmps get a
  // per-cmp MC marginalisation here.  Either way the rewritten
  // cmps become gate_plus over mulinputs (or gate_input
  // Bernoullis), so the surrounding circuit is purely Boolean for
  // the downstream pass.
  //
  // Runs BEFORE AnalyticEvaluator so shared bare-RV cmps reach the
  // grouping logic - AnalyticEvaluator would otherwise resolve each
  // independently into a Bernoulli, silently using the independence
  // approximation on shared base RVs.  The trade-off: the fast
  // path's mulinput block is a dependent circuit that
  // BooleanCircuit::independentEvaluation rejects when the cmps
  // combine via AND ('Not an independent circuit').  Callers that
  // need shared-island dependence handling must use
  // 'tree-decomposition' / 'monte-carlo' / external compilation;
  // 'independent' remains correct only for circuits that ARE
  // independent.
  if (provsql_hybrid_evaluation) {
    provsql::runHybridDecomposer(
      gc, static_cast<unsigned>(provsql_rv_mc_samples));
  }

  // Probability-specific peephole: AnalyticEvaluator decides any
  // residual continuous-RV comparators the decomposer left alone
  // (singleton bare gate_rv vs gate_value, or two bare normals) by
  // replacing them with Bernoulli gate_input gates carrying the
  // analytical probability.  Always sound for probability
  // evaluation; produces fractional probabilities so it is
  // meaningful only on this path (not in getGenericCircuit, which
  // is shared with semiring evaluators).
  // Count gates reachable from the root before / after the
  // probability-side pre-pass, so the user can see how much the
  // shortcut shrank the circuit the downstream method actually sees.
  auto count_reachable = [&](gate_t r) {
    std::set<gate_t> seen;
    std::stack<gate_t> stk;
    stk.push(r);
    while (!stk.empty()) {
      gate_t g = stk.top(); stk.pop();
      if (!seen.insert(g).second) continue;
      for (gate_t c : gc.getWires(g)) stk.push(c);
    }
    return seen.size();
  };
  size_t gates_before = count_reachable(gc_root);
  unsigned analytic_resolved = provsql::runAnalyticEvaluator(gc);

  /* Closed-form cmp-probability evaluators : the Poisson-binomial
   * pre-pass for HAVING COUNT(*) op C, the MIN / MAX closed forms, and
   * the weighted-sum DP for SUM(a) op C, all over independent private
   * contributors.  Each replaces a matched gate_cmp
   * with a Bernoulli gate_input carrying the closed-form probability,
   * so the surrounding circuit can skip the DNF that
   * provsql_having's enumerate_valid_worlds would otherwise build.
   * Same probability-side sound-only caveat as runAnalyticEvaluator :
   * the gate_input carries a fractional probability so the rewrite
   * lives here, not in getGenericCircuit.  Hidden behind
   * provsql.cmp_probability_evaluation for developer A/B testing ;
   * on by default. */
  unsigned count_cmp_resolved = 0;
  unsigned minmax_cmp_resolved = 0;
  unsigned sum_cmp_resolved = 0;
  unsigned agg_marginal_resolved = 0;
  if (provsql_cmp_probability_evaluation) {
    count_cmp_resolved = provsql::runCountCmpEvaluator(gc);
    minmax_cmp_resolved = provsql::runMinMaxCmpEvaluator(gc);
    sum_cmp_resolved = provsql::runSumCmpEvaluator(gc);
    /* Safe-join COUNT / SUM / MIN / MAX: the hierarchical marginal-vector
     * engine for the join shapes the flat pre-passes above cannot certify
     * independent.  Runs last so it only ever sees the join-shaped cmps
     * the flat passes left behind. */
    agg_marginal_resolved = provsql::runAggMarginalEvaluator(gc);
  }

  /* Always-true HAVING rewrite (runs regardless of the Poisson-binomial
   * GUC): catches @c COUNT <= K with @c K >= N (and dual cases for
   * other aggregators) by rewriting the cmp to @c gate_plus over the
   * agg's K-gates -- the "group is non-empty" indicator.  This is the
   * sound TRUE-decision arm that @c runRangeCheck deliberately leaves
   * undone (gate_one would credit the empty world); restricting it to
   * the probability-evaluate path keeps the absorptive-semiring
   * precondition satisfied. */
  unsigned always_true_resolved = provsql::runHavingAlwaysTrueRewriter(gc);

  /* If any probability-side pre-pass replaced a cmp with a closed-form
   * Bernoulli or an OR rewrite, the formula the downstream tool sees
   * is not the formula the user wrote: it has had part (or all) of its
   * comparison structure folded before any d-DNNF compiler / weighted
   * model counter is invoked. Emit a NOTICE (gated on
   * provsql.verbose_level >= 5) so the user knows the requested
   * method's reported timing and structure may not reflect work on
   * the original formula. */
  if (analytic_resolved + count_cmp_resolved + minmax_cmp_resolved
        + sum_cmp_resolved + agg_marginal_resolved + always_true_resolved > 0
      && provsql_verbose >= 5) {
    size_t gates_after = count_reachable(gc_root);
    std::vector<std::string> parts;
    if (analytic_resolved > 0)
      parts.push_back(std::to_string(analytic_resolved) + " analytic");
    if (count_cmp_resolved > 0)
      parts.push_back(std::to_string(count_cmp_resolved) + " Poisson-binomial");
    if (minmax_cmp_resolved > 0)
      parts.push_back(std::to_string(minmax_cmp_resolved) + " min/max");
    if (sum_cmp_resolved > 0)
      parts.push_back(std::to_string(sum_cmp_resolved) + " weighted-sum");
    if (agg_marginal_resolved > 0)
      parts.push_back(std::to_string(agg_marginal_resolved) + " safe-join aggregate");
    if (always_true_resolved > 0)
      parts.push_back(std::to_string(always_true_resolved) + " always-true");
    std::string breakdown;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) breakdown += " + ";
      breakdown += parts[i];
    }
    provsql_notice(
      "gate_cmp expression was shortcut by probability-side pre-pass "
      "(%s): provenance circuit reduced from %zu to %zu gates",
      breakdown.c_str(), gates_before, gates_after);
  }

  /* After every resolution pass has run, any gate_rv left in the
   * circuit reaches the BoolExpr translation in getBooleanCircuit
   * unchanged; that walk recurses into the surrounding gate_cmp and
   * calls semiring.value() on the gate_value side, producing the
   * generic "This semiring does not support value gates." error.
   * Detect that here and raise a message that names the actual
   * root cause: the analytical evaluators couldn't fold the RV
   * leaves away, and the MC fallback that would have decided the
   * surrounding cmp is either disabled (rv_mc_samples = 0) or
   * wasn't able to close the gap.  HAVING-style cmps over gate_agg
   * don't contain gate_rv, so this check leaves them for
   * provsql_having. */
  if (method != "monte-carlo" && method != "stopping-rule"
      && method != "relative" && method != "additive"
      && provsql::circuitHasRV(gc, gc_root)) {
    if (provsql_rv_mc_samples <= 0) {
      provsql_error(
        "probability_evaluate: a comparison over random variables "
        "could not be resolved analytically; raise "
        "provsql.rv_mc_samples above 0 to enable the Monte Carlo "
        "fallback, or call probability_evaluate(..., 'monte-carlo', "
        "<n>) directly");
    } else {
      provsql_error(
        "probability_evaluate: a comparison over random variables "
        "could not be resolved analytically and the hybrid evaluator "
        "left it unresolved; call probability_evaluate(..., "
        "'monte-carlo', <n>) directly for an MC estimate");
    }
  }

  double result;
  // Records which probability method actually produced the result, so it can
  // be exposed through the provsql.last_eval_method GUC (useful when method
  // is left empty and the default auto-selection picks one).
  string actual_method;

  provsql_interrupted = false;

  void (*prev_sigint_handler)(int);
  prev_sigint_handler = signal(SIGINT, provsql_sigint_handler);

  try {
    // GenericCircuit-level estimators (the relative / additive paths and their
    // explicit-method aliases) run before the BoolExpr translation in
    // getBooleanCircuit (which drops gate_rv and rejects RV gate_cmp), so they
    // sit at the top here rather than in the BooleanCircuit method catalog.
    //
    // 'relative' / 'stopping-rule': whole-circuit (eps,delta)-RELATIVE
    // probability via the Dagum-Karp-Luby-Ross stopping rule (the universal
    // relative estimator -- plain Boolean / RV / HAVING agg).  The 'relative'
    // path first tries an exact result when one is cheaply available.
    if(method == "relative" || method == "additive") {
      // Three-path tolerance request, routed through the SAME cost chooser as the
      // exact path -- just with a wider admissible set (toleranceAdmits): a
      // 'relative' request's portfolio is the exact methods (exact when cheaper) +
      // the relative estimators (karp-luby on a DNF, the universal stopping rule);
      // 'additive' additionally admits fixed-sample monte-carlo.  The chooser picks
      // the cheapest, so a tuple-independent circuit resolves exactly via
      // 'independent', a small DNF exactly via 'sieve', and a hard/large circuit
      // falls to the bounded-cost estimator -- generalising the old
      // independent-only "exact when cheaper" to the whole exact portfolio.
      const provsql::ToleranceKind tk = (method == "relative")
          ? provsql::ToleranceKind::Relative
          : provsql::ToleranceKind::Additive;
      SampleSpec s = parse_sample_spec(parse_method_args(args), method.c_str());
      provsql::Tolerance tol{tk, s.eps, s.delta};

      // The estimators (and the Boolean exact methods) need the Boolean view; it
      // drops gate_rv and rejects RV gate_cmp, so it cannot build on an RV / HAVING
      // circuit.  Build it when possible and run the unified portfolio; otherwise
      // fall back to the generic GenericCircuit estimator (the only option there),
      // exactly as before -- the stopping rule for 'relative', fixed-sample
      // monteCarloRV for 'additive'.
      //
      // A surviving sample-faithful HAVING comparator (SUM / AVG / MIN / MAX /
      // COUNT, that the exact closed-form / marginal-vector pre-passes bailed
      // on) is the apx-safe corner of the trichotomy: in practice only the
      // first four ever bail (COUNT's value-support is small, always resolved
      // exactly).  Building the Boolean view would force provsql_having's
      // threshold-lineage expansion, which does not terminate for a large-
      // magnitude / large-support aggregate.  For an APPROXIMATE (delta > 0)
      // request we skip the Boolean build and sample the comparator directly via
      // the GenericCircuit estimator (the gate_agg arm of the sampler) -- a sound
      // (eps,delta) FPRAS, magnitude-independent.  An exact (delta == 0)
      // 'relative' request still attempts the Boolean view (the expansion is the
      // only exact route).  See circuitHasUnresolvedSampleableAgg for why COUNT
      // is excluded.
      const bool sampleable_agg =
        provsql::circuitHasUnresolvedSampleableAgg(gc, gc_root);
      bool boolean_built = false;
      gate_t gate{};
      std::unordered_map<gate_t, gate_t> gc_to_bc;
      BooleanCircuit c;
      if(!provsql::circuitHasRV(gc, gc_root)
         && !(sampleable_agg && tol.delta > 0.)) {
        try {
          c = getBooleanCircuit(gc, token, gate, gc_to_bc);
          boolean_built = true;
        } catch(CircuitException &) {
          boolean_built = false;
        }
      }

      if(boolean_built) {
        provsql::EvalContext ctx{&gc, gc_root, token, c, gate, &gc_to_bc,
                                 inv_free_cert, args,
                                 /*explicitly_named=*/false,
                                 /*n_inputs=*/c.getInputs().size(),
                                 /*circuit_size=*/c.getNbGates()};
        result = provsql::MethodCatalog::instance().chooseAndRun(ctx, tol);
        actual_method = ctx.actual_method;
      } else if(tol.delta == 0.) {
        // No Boolean view (random-variable / HAVING-aggregate circuit): only the
        // (eps,delta) samplers apply here, none of which can honour delta = 0.
        provsql_error("a deterministic (delta = 0) '%s' guarantee is not "
                      "available for this circuit: it carries random-variable "
                      "or HAVING-aggregate gates, for which only the (eps,delta) "
                      "samplers apply -- use delta > 0", method.c_str());
      } else if(method == "relative") {
        run_stopping_rule(gc, gc_root, parse_method_args(args), result,
                          actual_method);
      } else {
        unsigned long samples = monte_carlo_samples(parse_method_args(args));
        result = provsql::monteCarloRV(gc, gc_root, static_cast<int>(samples));
        actual_method = "monte-carlo";
      }
    } else if(method == "stopping-rule") {
      run_stopping_rule(gc, gc_root, parse_method_args(args), result,
                        actual_method);
    } else if(method == "monte-carlo"
              && (provsql::circuitHasRV(gc, gc_root)
                  || provsql::circuitHasUnresolvedSampleableAgg(gc, gc_root))) {
      // RV-aware (fixed-sample, additive) Monte Carlo.  Also the route for a
      // surviving sample-faithful HAVING comparator (any aggregate -- the
      // apx-safe corner): the sampler evaluates the gate_agg directly, so a
      // large-magnitude aggregate is estimated without the non-terminating
      // threshold-lineage expansion.
      unsigned long samples = monte_carlo_samples(parse_method_args(args));
      result = provsql::monteCarloRV(gc, gc_root, static_cast<int>(samples));
    } else {
      // Boolean-circuit path: applies HAVING semantics and BoolExpr translation,
      // then dispatches through the method catalog.  The empty method (and its
      // 'exact'/'default' aliases) runs the cost-ordered exact ladder
      // (chooseAndRun); a named method dispatches by name.
      gate_t gate;
      std::unordered_map<gate_t, gate_t> gc_to_bc;
      BooleanCircuit c = getBooleanCircuit(gc, token, gate, gc_to_bc);

      const bool is_path = method.empty() || method == "default"
                           || method == "exact";
      provsql::EvalContext ctx{&gc, gc_root, token, c, gate, &gc_to_bc,
                               inv_free_cert, args,
                               /*explicitly_named=*/!is_path,
                               /*n_inputs=*/c.getInputs().size(),
                               /*circuit_size=*/c.getNbGates()};
      const provsql::MethodCatalog &catalog = provsql::MethodCatalog::instance();
      if(is_path) {
        result = catalog.chooseAndRun(ctx, provsql::Tolerance{});
      } else {
        const provsql::ProbabilityMethod *m = catalog.byName(method);
        if(m == nullptr)
          provsql_error("Wrong method '%s' for probability evaluation",
                        method.c_str());
        // Same declarative rewrite point as chooseAndRun: a Boolean-only method
        // named explicitly gets multivalued / BID blocks rewritten first.
        if(!m->handlesMultivalued())
          ctx.ensureMultivaluedRewritten();
        result = m->evaluate(ctx, provsql::Tolerance{});
      }
      actual_method = ctx.actual_method;
    }
  } catch(CircuitException &e) {
    // If the exception was raised because a query cancel or statement
    // timeout is pending (the in-process loops throw "Interrupted" off the
    // provsql_interrupted flag rather than longjmp through their C++ stack),
    // let PG report its native 57014 with the specific reason instead of the
    // generic "Interrupted".  For any other CircuitException no cancel is
    // pending, so CHECK_FOR_INTERRUPTS is a no-op and we report it as-is.
    CHECK_FOR_INTERRUPTS();
    provsql_error("%s", e.what());
  }

  // Record the method just used in provsql.last_eval_method (comma-separated,
  // deduplicated across calls in the session) so callers can inspect which
  // evaluation strategy the default auto-selection settled on.
  if(!actual_method.empty()) {
    string current = provsql_last_eval_method ? provsql_last_eval_method : "";
    if(current.find(actual_method) == string::npos) {
      if(!current.empty()) current += ",";
      current += actual_method;
      SetConfigOption("provsql.last_eval_method", current.c_str(),
                      PGC_USERSET, PGC_S_SESSION);
    }
  }

  provsql_interrupted = false;
  signal (SIGINT, prev_sigint_handler);

  // Avoid rounding errors that make probability outside of [0,1]
  if(result>1.)
    result=1.;
  else if(result<0.)
    result=0.;

  PG_RETURN_FLOAT8(result);
}

/** @brief PostgreSQL-callable wrapper for probability_evaluate(). */
Datum probability_evaluate(PG_FUNCTION_ARGS)
{
  provsql_sync_tool_registry();  // honour persisted tool-registry overrides
  try {
    Datum token = PG_GETARG_DATUM(0);
    string method;
    string args;

    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();

    if(!PG_ARGISNULL(1)) {
      text *t = PG_GETARG_TEXT_P(1);
      method = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    if(!PG_ARGISNULL(2)) {
      text *t = PG_GETARG_TEXT_P(2);
      args = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    bool isnull = false;
    Datum result =
      probability_evaluate_internal(*DatumGetUUIDP(token), method, args, &isnull);
    if(isnull)
      PG_RETURN_NULL();
    return result;
  } catch(const std::exception &e) {
    provsql_error("probability_evaluate: %s", e.what());
  } catch(...) {
    provsql_error("probability_evaluate: Unknown exception");
  }

  PG_RETURN_NULL();
}

/**
 * @brief PostgreSQL-callable wrapper for the d-tree leaf bound:
 *        @c probability_bounds(token uuid, OUT lower float8, OUT upper float8).
 *
 * Returns a cheap certified interval @c [lower,upper] with @c lower ≤ Pr ≤ upper
 * for the probability of the DNF-shaped circuit rooted at @p token, via
 * @c BooleanCircuit::dnfBounds (Olteanu-Huang-Koch Fig. 3).  Errors when the circuit
 * is not a monotone DNF over input leaves (the leaf-bound heuristic is
 * DNF-specific); the future d-tree engine will recurse on non-DNF roots.
 */
Datum probability_bounds(PG_FUNCTION_ARGS)
{
  provsql_sync_tool_registry();
  try {
    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();
    pg_uuid_t token = *DatumGetUUIDP(PG_GETARG_DATUM(0));

    gate_t root;
    BooleanCircuit c = getBooleanCircuit(token, root);

    std::vector<gate_t> clause_roots;
    std::vector<std::set<gate_t> > supports;
    if(!c.dnfShape(root, clause_roots, supports))
      provsql_error("probability_bounds applies only to a DNF-shaped circuit "
                    "(a monotone OR-of-ANDs over input leaves); negation, "
                    "comparison, aggregation, random-variable and "
                    "multivalued-input gates are not supported");

    double lower, upper;
    c.dnfBounds(supports, lower, upper);

    TupleDesc tupdesc;
    if(get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      provsql_error("probability_bounds: expected composite return type");
    tupdesc = BlessTupleDesc(tupdesc);

    Datum values[2] = { Float8GetDatum(lower), Float8GetDatum(upper) };
    bool nulls[2] = { false, false };
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  } catch(const std::exception &e) {
    provsql_error("probability_bounds: %s", e.what());
  } catch(...) {
    provsql_error("probability_bounds: Unknown exception");
  }

  PG_RETURN_NULL();
}
