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
 * The design is documented at length in
 * @c doc/TODO/having-trichotomy.md (the method-catalog / three-path chooser
 * section).
 */
#ifndef PROVSQL_PROBABILITY_METHOD_H
#define PROVSQL_PROBABILITY_METHOD_H

#include <memory>
#include <string>
#include <vector>

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
 * could not change the decision.  Free/O(1) features (#inputs, an inversion-free
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

  /// Run the method, returning the probability.  May mutate @p ctx (build the
  /// Boolean view lazily, trigger the multivalued rewrite, set the reported
  /// method name).
  virtual double evaluate(EvalContext &ctx, const Tolerance &tol) const = 0;
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

private:
  std::vector<std::unique_ptr<ProbabilityMethod>> methods_;
};

}  // namespace provsql

#endif  // PROVSQL_PROBABILITY_METHOD_H
