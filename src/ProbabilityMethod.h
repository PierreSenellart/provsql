/**
 * @file ProbabilityMethod.h
 * @brief Catalog of probability-evaluation methods (Strategy + registry).
 *
 * Each probability-evaluation algorithm is a first-class @c ProbabilityMethod
 * object that declares its own guarantee, applicability and (eventually) cost,
 * instead of that knowledge being smeared across a string-switch dispatcher.
 * The @c MethodCatalog is the registry the dispatcher in
 * @c probability_evaluate.cpp consults: a named request resolves through
 * @c byName(); the default (empty-method) request runs @c chooseAndRun(), which
 * reproduces the historical independent -> inversion-free -> compilation ladder
 * as the @c Exact-tolerance instance of the chooser.
 *
 * This header carries only the public interface.  The concrete method classes,
 * the @c EvalContext that threads the per-evaluation circuit state, and the
 * catalog registration all live in @c probability_evaluate.cpp, where the
 * file-local evaluation helpers they need are in scope.
 *
 * The design (the method-catalog / three-path chooser) is documented at
 * length in @c doc/source/dev/probability-evaluation.rst.
 */
#ifndef PROVSQL_PROBABILITY_METHOD_H
#define PROVSQL_PROBABILITY_METHOD_H

#include <memory>
#include <string>
#include <vector>

// d-DNNF artifact (defined in dDNNF.h). The d-D-producing methods can return it
// via buildDD(), and the catalog's chooseAndBuildDD / makeDDAuto cost-select a
// construction route and return the artifact for makeDD's callers (shapley,
// compile_to_ddnnf, ddnnf_stats) -- the same selection the probability chooser
// already makes among interpret-as-dd / tree-decomposition / compilation.
class dDNNF;
class BooleanCircuit;
enum class gate_t : size_t;

namespace provsql {

/**
 * @brief The contract the user grants -- the "path".
 *
 * @c Exact is tolerance @c (0,0); @c Relative promises @c (1±epsilon) with
 * confidence @c 1-delta; @c Additive promises @c |p̂-p| <= epsilon with the same
 * confidence.  Admissible method sets nest @c Exact ⊂ @c Relative ⊂ @c Additive.
 *
 * Phase 1 plumbs the tolerance but does not yet drive selection: named methods
 * dispatch by name and the empty method runs the exact ladder.  Tolerance-driven
 * selection across the approximate members arrives with the relative/additive
 * paths (later phases).
 */
enum class ToleranceKind { Exact, Relative, Additive };

struct Tolerance {
  ToleranceKind kind = ToleranceKind::Exact;
  double epsilon = 0.;
  double delta = 0.;
};

/**
 * @brief A circuit feature a method's cost/applicability estimate depends on,
 *        but that is not free to acquire.
 *
 * Acquiring a feature has a cost (modelled by @c EvalContext::featureCost), and
 * the chooser acquires one lazily only when no already-known method is cheaper
 * than acquiring it -- so a circuit the cheap methods resolve (read-once via
 * @c independent, certified via @c inversion-free) never pays for analysis that
 * could not change the decision.  Free/O(1) features (\#inputs, an inversion-free
 * certificate) are not modelled here -- they are read eagerly.
 *
 * @c DnfShape is a linear @c dnfShape walk.  @c TreewidthProxy is a cheap
 * (@c O(V+E)) degeneracy lower bound on the circuit's treewidth that gates
 * @c tree-decomposition's cost -- it rules the method out when the bound already
 * exceeds the build's limit, while the bounded-treewidth build can still fail
 * implicitly when the bound is inconclusive.
 */
enum class Feature { DnfShape, TreewidthProxy };

/// Per-evaluation circuit state threaded to a method's @c evaluate (defined in
/// @c probability_evaluate.cpp, where the Boolean/Generic circuit machinery is
/// in scope).
struct EvalContext;

/**
 * @brief Strategy interface: one concrete subclass per probability method.
 *
 * @c evaluate throws @c CircuitException when the method cannot be applied to
 * the circuit; @c chooseAndRun relies on that to fall through the default
 * ladder, while @c byName lets it propagate (matching the historical explicit
 * method behaviour).
 */
class ProbabilityMethod {
public:
  virtual ~ProbabilityMethod() = default;

  /// Stable identifier used for @c byName lookup and the
  /// @c provsql.last_eval_method report.
  virtual std::string name() const = 0;

  /// Which user-facing path this method can serve.
  virtual ToleranceKind guaranteeKind() const = 0;

  /// True iff this method participates in the auto-chooser's portfolio (vs being
  /// reachable only @c byName).
  virtual bool inDefaultChain() const { return false; }

  /// True iff the method's guarantee holds with CERTAINTY (no failure
  /// probability).  The exact methods and the d-tree's certified interval are
  /// deterministic; the (eps,delta) samplers are not.  A request with delta == 0
  /// ("deterministic") admits only deterministic methods -- the samplers cannot
  /// honour it (their sample count is proportional to ln(1/delta), so delta = 0
  /// is infeasible, and their cost model masks this by falling back to a finite
  /// delta).  For delta > 0 the samplers stay admissible but their cost grows as
  /// delta shrinks, so the chooser already migrates to the (delta-independent)
  /// d-tree well before delta reaches 0.
  virtual bool isDeterministic() const { return true; }

  /// Features the method's @c estimatedCost / @c applicable need acquired before
  /// they are meaningful (empty = a free estimate).  The chooser will not call
  /// @c estimatedCost / @c applicable until these are acquired, and acquires
  /// them lazily per the cost rule above.
  virtual std::vector<Feature> requiredFeatures() const { return {}; }

  /// Heuristic estimate of this method's cost on the current circuit (lower is
  /// cheaper).  Only called once @c requiredFeatures are acquired.  The chooser sorts admissible portfolio members by this and tries
  /// them cheapest-first -- there is deliberately NO fixed ordinal: the order
  /// emerges from the estimates, and a calibrated cost model can later replace
  /// the heuristic bodies without touching the chooser.  Only consulted for
  /// portfolio members; by-name-only methods keep the default.
  virtual double estimatedCost(const EvalContext &, const Tolerance &) const
  { return 0.; }

  /// Cheap admissibility check for the portfolio (e.g. an inversion-free
  /// certificate must be present, or a 2^N method's N is within a sanity
  /// bound).  @c byName ignores it.
  virtual bool applicable(const EvalContext &, const Tolerance &) const
  { return true; }

  /// True iff the method evaluates the *raw* circuit, including multivalued
  /// (BID / @c gate_mulinput) gates, and must therefore NOT have them rewritten
  /// to independent Booleans first.  The dispatcher rewrites multivalued gates
  /// (@c EvalContext::ensureMultivaluedRewritten) before every method that
  /// returns @c false, so a Boolean-only method needs no per-@c evaluate rewrite
  /// of its own and a new one is BID-correct by default.  Only @c "independent"
  /// overrides this: it interprets a @c mulinput block natively (summing the
  /// mutually-exclusive alternatives) to keep the exact disjoint-OR structure
  /// the rewrite would dissolve.  (Methods gated to TID-only circuits -- the
  /// DNF samplers via @c Feature::DnfShape, @c inversion-free via its
  /// certificate -- never see a @c mulinput, so the central rewrite is a no-op
  /// for them and they keep the default.)
  virtual bool handlesMultivalued() const { return false; }

  /// Run the method, returning the probability.  May mutate @p ctx (build the
  /// Boolean view lazily, trigger the multivalued rewrite, set the reported
  /// method name).
  virtual double evaluate(EvalContext &ctx, const Tolerance &tol) const = 0;

  /// True iff this method constructs a d-DNNF artifact (and so can serve the
  /// makeDD route-chooser via @c buildDD).  The three d-D constructors --
  /// interpret-as-dd, tree-decomposition, compilation -- override this; the
  /// scalar methods (independent, possible-worlds, the samplers, the d-tree)
  /// do not.  This is the @c producesDD() portfolio @c chooseAndBuildDD ranks.
  virtual bool producesDD() const { return false; }

  /// Build the d-DNNF this method constructs (only when @c producesDD()).
  /// @c evaluate() of a d-D method is exactly @c buildDD(ctx).probabilityEvaluation(),
  /// so the cost/route logic stays single-sourced.  Sets @c ctx.actual_method.
  virtual dDNNF buildDD(EvalContext &ctx) const;
};

/**
 * @brief Registry of @c ProbabilityMethod objects.
 *
 * Mirrors the external-tool registry: adding a method is a new subclass plus one
 * @c registerMethod call -- the dispatcher never changes (open/closed).
 */
class MethodCatalog {
public:
  /// The process-wide catalog, lazily populated with the built-in methods.
  static const MethodCatalog &instance();

  void registerMethod(std::unique_ptr<ProbabilityMethod> m);

  /// Exact match on @c name(); nullptr if absent.
  const ProbabilityMethod *byName(const std::string &name) const;

  /// Run the auto-chooser for @p tol: the portfolio methods admissible for the
  /// tolerance and @c applicable, sorted cheapest-first by @c estimatedCost and
  /// tried until one succeeds.  The costliest method's exception propagates (the
  /// portfolio is exhausted).
  double chooseAndRun(EvalContext &ctx, const Tolerance &tol) const;

  /// The d-DNNF analogue of @c chooseAndRun: cost-select among the
  /// @c producesDD() portfolio (interpret-as-dd / tree-decomposition /
  /// compilation) with the same uniform-cost search and speculative budget, and
  /// return the chosen d-DNNF artifact (rather than a probability).  This is the
  /// makeDD route optimizer -- the same selection the probability chooser makes
  /// among these routes, surfaced for the d-D-artifact callers.
  dDNNF chooseAndBuildDD(EvalContext &ctx, const Tolerance &tol) const;

private:
  std::vector<std::unique_ptr<ProbabilityMethod>> methods_;
};

/// Cost-select a d-DNNF construction route for gate @p g of Boolean circuit
/// @p c and build it -- the default makeDD route.  A thin entry point over
/// @c MethodCatalog::chooseAndBuildDD that builds the @c EvalContext from the
/// Boolean view alone (the d-D portfolio needs no generic-circuit state); the
/// callers that have a method/compiler request route the empty / "default" /
/// "auto" case here and keep @c BooleanCircuit::makeDD for the named routes and
/// the "ladder" (old fixed interpret -> tree-decomposition -> compiler) escape.
dDNNF makeDDAuto(BooleanCircuit &c, gate_t g);

}  // namespace provsql

#endif  // PROVSQL_PROBABILITY_METHOD_H
