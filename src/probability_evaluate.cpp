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
#include "provsql_shmem.h"
#include "provsql_utils.h"
#include "utils/guc.h"

PG_FUNCTION_INFO_V1(probability_evaluate);
}

#include "c_cpp_compatibility.h"
#include <set>
#include <stack>
#include <map>
#include <unordered_map>
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
#include "StructuredDNNF.h"
#include "ProbabilityMethod.h"
#include "having_semantics.hpp"
#include "provsql_mmap.h"
#include "safe_query_cert.h"
#include "provsql_utils_cpp.h"
#include "tool_registry_sync.h"
#include "semiring/BoolExpr.h"

using namespace std;

namespace {

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
                        || delta <= 0. || delta >= 1.))
    provsql_error("method '%s': delta must be in (0, 1)", method);
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
namespace provsql {

/// Per-evaluation circuit state threaded to a method's evaluate().  The Boolean
/// view @c c is built once in probability_evaluate_internal; methods that need
/// the multivalued rewrite trigger it (idempotently) through this context, so
/// the rewrite fires exactly when the historical post-rewrite branch did.
struct EvalContext {
  GenericCircuit &gc;
  gate_t gc_root;
  pg_uuid_t token;
  BooleanCircuit &c;
  gate_t gate;
  std::unordered_map<gate_t, gate_t> &gc_to_bc;
  bool inv_free_cert;
  const std::string &args;
  bool explicitly_named;            ///< invoked via byName (vs the default chain)
  bool multivalued_rewritten = false;
  std::string actual_method;

  void ensureMultivaluedRewritten() {
    if(!multivalued_rewritten) {
      c.rewriteMultivaluedGates();
      multivalued_rewritten = true;
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
  int chainOrder() const override { return 0; }
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
  int chainOrder() const override { return 1; }
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
    if(!collect_inversion_free_keys(ctx.gc, ctx.gc_root, ctx.gc_to_bc, ctx.c,
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

/// Exact, generic d-DNNF construction (tree-decomposition / interpret-as-dd /
/// external compiler), parameterised by the makeDD method string and the name
/// reported through provsql.last_eval_method.  One instance per invocation
/// spelling; the chain terminal is the "default" instance.
class CompilationMethod : public ProbabilityMethod {
  std::string name_, makedd_arg_, report_as_;
  bool in_chain_;
  int chain_order_;
public:
  CompilationMethod(std::string n, std::string makedd_arg, std::string report_as,
                    bool in_chain = false, int order = 0)
    : name_(std::move(n)), makedd_arg_(std::move(makedd_arg)),
      report_as_(std::move(report_as)), in_chain_(in_chain), chain_order_(order)
  {}
  std::string name() const override { return name_; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  bool inDefaultChain() const override { return in_chain_; }
  int chainOrder() const override { return chain_order_; }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    ctx.ensureMultivaluedRewritten();
    dDNNF dd = ctx.c.makeDD(ctx.gate, makedd_arg_, ctx.args);
    double r = dd.probabilityEvaluation();
    ctx.actual_method = report_as_;
    return r;
  }
};

/// Exact, naive 2^N enumeration.  By-name only (not in the default ladder).
class PossibleWorldsMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "possible-worlds"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Exact; }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    ctx.ensureMultivaluedRewritten();
    if(!ctx.args.empty())
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
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    unsigned long samples = monte_carlo_samples(parse_method_args(ctx.args));
    ctx.ensureMultivaluedRewritten();
    double r = ctx.c.monteCarlo(ctx.gate, static_cast<unsigned>(samples));
    ctx.actual_method = "monte-carlo";
    return r;
  }
};

/// Karp-Luby FPRAS over a DNF-shaped monotone circuit.  By-name only; runs
/// before the multivalued rewrite (it rejects multivalued inputs anyway).
class KarpLubyMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "karp-luby"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Relative; }
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

/// weightmc: backward-compatible alias for the weighted-model-counter path.
class WeightmcMethod : public ProbabilityMethod {
public:
  std::string name() const override { return "weightmc"; }
  ToleranceKind guaranteeKind() const override { return ToleranceKind::Relative; }
  double evaluate(EvalContext &ctx, const Tolerance &) const override {
    std::string opt = wmc_opt_from_args(parse_method_args(ctx.args), "weightmc");
    emit_guarantee("relative", eps_from_wmc_opt(opt), -1., 0, -1, "weightmc");
    ctx.ensureMultivaluedRewritten();
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
    ctx.ensureMultivaluedRewritten();
    double r = ctx.c.wmcCount(ctx.gate, tool, tool_args);
    ctx.actual_method = "wmc";
    return r;
  }
};

}  // anonymous namespace

void MethodCatalog::registerMethod(std::unique_ptr<ProbabilityMethod> m)
{
  methods_.push_back(std::move(m));
}

const ProbabilityMethod *MethodCatalog::byName(const std::string &n) const
{
  for(const auto &m : methods_)
    if(m->name() == n)
      return m.get();
  return nullptr;
}

double MethodCatalog::chooseAndRun(EvalContext &ctx, const Tolerance &tol) const
{
  std::vector<const ProbabilityMethod *> chain;
  for(const auto &m : methods_)
    if(m->inDefaultChain() && m->applicable(ctx, tol))
      chain.push_back(m.get());
  std::sort(chain.begin(), chain.end(),
            [](const ProbabilityMethod *a, const ProbabilityMethod *b) {
              return a->chainOrder() < b->chainOrder();
            });
  if(chain.empty())
    throw CircuitException("no applicable probability method in the default "
                           "chain");
  // Try each in order; the terminal method's exception propagates (the chain is
  // exhausted), reproducing the historical "compilation runs uncaught" tail.
  for(size_t i = 0; i + 1 < chain.size(); ++i) {
    try {
      return chain[i]->evaluate(ctx, tol);
    } catch(CircuitException &) { /* fall through to the next ladder method */ }
  }
  return chain.back()->evaluate(ctx, tol);
}

const MethodCatalog &MethodCatalog::instance()
{
  static const MethodCatalog cat = [] {
    MethodCatalog c;
    // Default exact ladder, in chainOrder: independent -> inversion-free ->
    // compilation (the "default" CompilationMethod is the terminal, == the
    // historical empty-method makeDD(gate, "", args)).
    c.registerMethod(std::make_unique<IndependentMethod>());
    c.registerMethod(std::make_unique<InversionFreeMethod>());
    c.registerMethod(std::make_unique<CompilationMethod>(
                       "default", "", "tree-decomposition", true, 2));
    // By-name-only compilation spellings.
    c.registerMethod(std::make_unique<CompilationMethod>(
                       "compilation", "compilation", "compilation"));
    c.registerMethod(std::make_unique<CompilationMethod>(
                       "tree-decomposition", "tree-decomposition",
                       "tree-decomposition"));
    c.registerMethod(std::make_unique<CompilationMethod>(
                       "interpret-as-dd", "interpret-as-dd", "interpret-as-dd"));
    // By-name-only methods.
    c.registerMethod(std::make_unique<PossibleWorldsMethod>());
    c.registerMethod(std::make_unique<MonteCarloMethod>());
    c.registerMethod(std::make_unique<KarpLubyMethod>());
    c.registerMethod(std::make_unique<WeightmcMethod>());
    c.registerMethod(std::make_unique<WmcMethod>());
    return c;
  }();
  return cat;
}

}  // namespace provsql

/**
 * @brief Core implementation of probability evaluation for a circuit token.
 * @param token   UUID of the root provenance gate.
 * @param method  Evaluation method name (e.g. "independent", "monte-carlo").
 * @param args    Additional arguments for the chosen method.
 * @return        Float8 Datum containing the computed probability.
 */
static Datum probability_evaluate_internal
  (pg_uuid_t token, const string &method, const string &args)
{
  // Load the GenericCircuit once: we need it for the RV-detection
  // dispatch below, and getBooleanCircuit() reuses it internally so we
  // pay no extra cost compared to the previous flow.  Universal
  // cmp-resolution passes (RangeCheck) have already been applied
  // inside getGenericCircuit when the provsql.simplify_on_load GUC is
  // on (the default), so the circuit we receive here is already
  // peephole-pruned for any "always true / always false" comparator.
  GenericCircuit gc = getGenericCircuit(token);
  gate_t gc_root = gc.getGate(uuid2string(token));

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
  if (method != "monte-carlo" && provsql::circuitHasRV(gc, gc_root)) {
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
    // RV-aware Monte Carlo: when the circuit contains continuous
    // random-variable leaves, the BoolExpr translation in
    // getBooleanCircuit drops them, so we sample directly on the
    // GenericCircuit instead.  Other probability methods are not
    // (yet) defined over RV circuits.
    if(method == "monte-carlo" && provsql::circuitHasRV(gc, gc_root)) {
      unsigned long samples = monte_carlo_samples(parse_method_args(args));
      result = provsql::monteCarloRV(gc, gc_root, static_cast<int>(samples));
    } else {
      // Existing Boolean-circuit path: applies HAVING semantics and
      // BoolExpr translation, then dispatches across the legacy
      // probability methods.
      gate_t gate;
      std::unordered_map<gate_t, gate_t> gc_to_bc;
      BooleanCircuit c = getBooleanCircuit(gc, token, gate, gc_to_bc);

      // Method-catalog dispatch (see ProbabilityMethod.h): the empty method
      // runs the default exact ladder; a named method dispatches by name.  The
      // ladder and the per-method behaviour are a 1:1 port of the historical
      // if/else chain (Phase 1, behaviour-preserving).
      provsql::EvalContext ctx{gc, gc_root, token, c, gate, gc_to_bc,
                               inv_free_cert, args,
                               /*explicitly_named=*/!method.empty()};
      const provsql::MethodCatalog &catalog = provsql::MethodCatalog::instance();
      if(method.empty()) {
        result = catalog.chooseAndRun(ctx, provsql::Tolerance{});
      } else {
        const provsql::ProbabilityMethod *m = catalog.byName(method);
        if(m == nullptr)
          provsql_error("Wrong method '%s' for probability evaluation",
                        method.c_str());
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

    return probability_evaluate_internal(*DatumGetUUIDP(token), method, args);
  } catch(const std::exception &e) {
    provsql_error("probability_evaluate: %s", e.what());
  } catch(...) {
    provsql_error("probability_evaluate: Unknown exception");
  }

  PG_RETURN_NULL();
}
