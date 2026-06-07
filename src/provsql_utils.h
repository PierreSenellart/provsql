/**
 * @file provsql_utils.h
 * @brief Core types, constants, and utilities shared across ProvSQL.
 *
 * This header is included by virtually every source file in the
 * extension.  It provides:
 * - The @c gate_type enumeration listing all circuit-gate kinds
 *   recognised by ProvSQL (input, semiring operations, aggregation, etc.)
 * - The @c constants_t structure caching PostgreSQL OIDs for the types,
 *   functions, and operators that ProvSQL installs, so that OID lookups
 *   happen once per session rather than on every query.
 * - The @c database_constants_t wrapper for per-database OID caches.
 * - Helper declarations for OID lookup and UUID manipulation.
 * - Global flags controlling interrupt handling, where-provenance, and
 *   verbosity.
 * - An implicit inclusion of @c provsql_error.h for the @c provsql_error
 *   / @c provsql_warning / @c provsql_notice / @c provsql_log macros.
 */
#ifndef PROVSQL_UTILS_H
#define PROVSQL_UTILS_H

#include "pg_config.h" // for PG_VERSION_NUM
#include "c.h" // for int16

#include "postgres.h"
#include "utils/uuid.h"

#if PG_VERSION_NUM < 100000
/// Number of bytes in a UUID
#define UUID_LEN 16

/** UUID structure. In versions of PostgreSQL < 10, pg_uuid_t is declared
 * to be an opaque struct pg_uuid_t in uuid.h, so we have to give the
 * definition of struct pg_uuid_t; this problem is resolved in PostgreSQL 10. */
struct pg_uuid_t
{
  unsigned char data[UUID_LEN]; ///< Raw 16-byte UUID storage
};
#endif /* PG_VERSION_NUM */

#include "postgres_ext.h"
#include "nodes/pg_list.h"

/**
 * @brief Possible gate types in the provenance circuit.
 *
 * @warning ON-DISK ABI: this enum's integer values are stored in the
 * @c gates.mmap backing file (see @c MMappedCircuit).  Reordering,
 * inserting, or renumbering existing members will silently invalidate
 * every existing installation's persistent circuit.  New gate types
 * must be appended **at the end**, just before @c gate_invalid.  If an
 * existing gate type ever needs to be removed or renumbered, the mmap
 * format must gain a version header and a migration path.
 */
typedef enum gate_type {
  gate_input,    ///< Input (variable) gate of the circuit
  gate_plus,     ///< Semiring plus
  gate_times,    ///< Semiring times
  gate_monus,    ///< M-Semiring monus
  gate_project,  ///< Project gate (for where provenance)
  gate_zero,     ///< Semiring zero
  gate_one,      ///< Semiring one
  gate_eq,       ///< Equijoin gate (for where provenance)
  gate_agg,      ///< Aggregation operator (for aggregate provenance)
  gate_semimod,  ///< Semimodule scalar multiplication (for aggregate provenance)
  gate_cmp,      ///< Comparison of aggregate values (HAVING-clause provenance)
  gate_delta,    ///< δ-semiring operator (see Amsterdamer, Deutch, Tannen, PODS 2011)
  gate_value,    ///< Scalar value (for aggregate provenance)
  gate_mulinput, ///< Multivalued input (for Boolean provenance)
  gate_update,   ///< Update operation
  gate_rv,       ///< Continuous random-variable leaf (extra encodes distribution)
  gate_arith,    ///< n-ary arithmetic gate over scalar-valued children (info1 holds operator tag)
  gate_mixture,  ///< Probabilistic mixture: three wires [p_token (gate_input Bernoulli), x_token, y_token]; samples x when p is true, y otherwise
  gate_assumed, ///< Structural marker over a single child whose sub-circuit was computed under a Boolean-provenance assumption (e.g. the safe-query rewrite); transparent (identity) for Boolean-compatible evaluators, fatal error for the rest, kept as an explicit node in PROV-XML export
  gate_annotation, ///< Transparent single-child wrapper carrying a query-level annotation in @c extra (inversion-free certificate / per-input order key); identity for EVERY evaluator, and -- unlike the children-only convention -- its UUID folds in @c extra so distinct annotations over the same child are distinct gates.
  gate_invalid,  ///< Invalid gate type
  nb_gate_types  ///< Total number of gate types
} gate_type;

/**
 * @brief Scalar-aggregation flag, stored in the upper bit of a @c gate_agg's
 *        @c info2 (whose low 31 bits hold the aggregate result-type OID).
 *
 * Set by the query rewriter when the aggregation has no @c GROUP @c BY (a single
 * always-present result row).  It tells the value-aware evaluators that the
 * empty-input world is a real possible world (carrying the aggregate's
 * empty-group value), rather than the "no row" of a grouped query.  Result-type
 * OIDs never use bit 31, so the low 31 bits still recover the type via
 * @c PROVSQL_AGG_TYPE_MASK.
 *
 * @warning The flag is folded into the gate's content UUID (see
 * @c provenance_aggregate), so a scalar and a grouped aggregate over identical
 * children stay distinct gates and their @c set_infos calls do not clobber.
 */
#define PROVSQL_AGG_SCALAR_FLAG 0x80000000u
#define PROVSQL_AGG_TYPE_MASK   0x7FFFFFFFu

/**
 * @brief Arithmetic operator tags used by @c gate_arith.
 *
 * Stored in the gate's @c info1 field.  Local enum (not a PostgreSQL
 * operator OID) because arithmetic in the sampler / evaluator is just
 * C++ doubles, with no need to dispatch through the PG catalog.
 *
 * @warning ON-DISK ABI: like @c gate_type, these integer values are
 * persisted (in @c info1).  Reordering or renumbering existing tags
 * will silently invalidate every existing installation's persistent
 * circuit.  New tags must be appended at the end.
 */
typedef enum provsql_arith_op {
  PROVSQL_ARITH_PLUS  = 0, ///< n-ary, sum of children
  PROVSQL_ARITH_TIMES = 1, ///< n-ary, product of children
  PROVSQL_ARITH_MINUS = 2, ///< binary, child0 - child1
  PROVSQL_ARITH_DIV   = 3, ///< binary, child0 / child1
  PROVSQL_ARITH_NEG   = 4  ///< unary, -child0
} provsql_arith_op;

/** Names of gate types */
extern const char *gate_type_name[];

/**
 * @brief Canonical name of the per-row provenance column installed by
 *        @c add_provenance / @c repair_key.
 *
 * Centralised so the planner-hook entry points (@c src/provsql.c) and
 * the safe-query detector (@c src/safe_query.c) agree on the literal;
 * see the @c provenance_guard trigger in @c sql/provsql.common.sql.
 */
#define PROVSQL_COLUMN_NAME "provsql"

/** Structure to store the value of various constants. This is needed to
 * uniquely identify types, functions, etc., in PostgreSQL through their
 * Object Identifier Types (OIDs). */
typedef struct constants_t {
  Oid OID_SCHEMA_PROVSQL; ///< OID of the provsql SCHEMA
  Oid OID_TYPE_GATE_TYPE; ///< OID of the provenance_gate TYPE
  Oid OID_TYPE_AGG_TOKEN; ///< OID of the agg_token TYPE
  Oid OID_TYPE_UUID; ///< OID of the uuid TYPE
  Oid OID_TYPE_UUID_ARRAY; ///< OID of the uuid[] TYPE
  Oid OID_TYPE_BOOL; ///< OID of the BOOL TYPE
  Oid OID_TYPE_INT; ///< OID of the INT TYPE
  Oid OID_TYPE_INT_ARRAY; ///< OID of the INT[] TYPE
  Oid OID_TYPE_FLOAT; ///< OID of the FLOAT TYPE
  Oid OID_TYPE_VARCHAR; ///< OID of the VARCHAR TYPE
  Oid OID_TYPE_TSTZMULTIRANGE; ///< OID of the tstzmultirange TYPE (PG14+, InvalidOid otherwise)
  Oid OID_TYPE_NUMMULTIRANGE; ///< OID of the nummultirange TYPE (PG14+, InvalidOid otherwise)
  Oid OID_TYPE_INT4MULTIRANGE; ///< OID of the int4multirange TYPE (PG14+, InvalidOid otherwise)
  Oid OID_FUNCTION_ARRAY_AGG; ///< OID of the array_agg FUNCTION
  Oid OID_FUNCTION_PROVENANCE_PLUS; ///< OID of the provenance_plus FUNCTION
  Oid OID_FUNCTION_PROVENANCE_TIMES; ///< OID of the provenance_times FUNCTION
  Oid OID_FUNCTION_PROVENANCE_MONUS; ///< OID of the provenance_monus FUNCTION
  Oid OID_FUNCTION_PROVENANCE_PROJECT; ///< OID of the provenance_project FUNCTION
  Oid OID_FUNCTION_PROVENANCE_EQ;///< OID of the provenance_eq FUNCTION
  Oid OID_FUNCTION_PROVENANCE_CMP; ///< OID of the provenance_cmp FUNCTION
  Oid OID_FUNCTION_PROVENANCE; ///< OID of the provenance FUNCTION
  Oid GATE_TYPE_TO_OID[nb_gate_types]; ///< Array of the OID of each provenance_gate ENUM value
  Oid OID_FUNCTION_PROVENANCE_DELTA; ///< OID of the provenance_delta FUNCTION
  Oid OID_FUNCTION_PROVENANCE_AGGREGATE; ///< OID of the provenance_aggregate FUNCTION
  Oid OID_FUNCTION_PROVENANCE_SEMIMOD; ///< OID of the provenance_semimod FUNCTION
  Oid OID_FUNCTION_GATE_ZERO; ///< OID of the provenance_zero FUNCTION
  Oid OID_FUNCTION_GATE_ONE; ///< OID of the provenance_one FUNCTION
  Oid OID_OPERATOR_NOT_EQUAL_UUID; ///< OID of the <> operator on UUIDs FUNCTION
  Oid OID_FUNCTION_NOT_EQUAL_UUID; ///< OID of the = operator on UUIDs FUNCTION
  Oid OID_FUNCTION_AGG_TOKEN_UUID; ///< OID of the agg_token_uuid FUNCTION
  Oid OID_FUNCTION_GET_CHILDREN; ///< OID of the get_children FUNCTION
  Oid OID_FUNCTION_GET_EXTRA;    ///< OID of the get_extra FUNCTION
  Oid OID_UNNEST; ///< OID of the unnest(anyarray) FUNCTION
  Oid OID_TYPE_RANDOM_VARIABLE; ///< OID of the random_variable TYPE
  Oid OID_FUNCTION_RV_AGGREGATE_SEMIMOD; ///< OID of rv_aggregate_semimod helper (uuid, rv -> rv) used to wrap each per-row argument of an RV-returning aggregate (sum, avg, ...)
  Oid OID_FUNCTION_CHOOSE; ///< OID of the choose(anyelement) aggregate (keeps the first non-NULL value); used to decorrelate scalar subqueries into a LEFT JOIN + GROUP BY
  /** @brief OID of @c provsql.assume_boolean(uuid)->uuid.
   *
   *  Installed by the @c 1.5.0--1.6.0 upgrade script.  Wraps its child
   *  in a fresh @c gate_assumed and returns the wrapper's UUID.
   *  When @c InvalidOid the safe-query rewriter (and any other
   *  Boolean-only rewrite that needs the marker) is effectively
   *  disabled even if @c provsql.boolean_provenance is on: the
   *  rewriter refuses to produce unmarked roots on a schema that
   *  cannot enforce the semiring-compatibility check. */
  Oid OID_FUNCTION_ASSUME_BOOLEAN;
  /** @brief OID of @c provsql.annotate(uuid,text)->uuid.
   *
   *  Wraps its child in a fresh transparent @c gate_annotation whose UUID
   *  folds in the @c extra text, and returns the wrapper's UUID.  Used to
   *  attach the inversion-free tractability certificate (on the root) and the
   *  per-input order keys.  @c InvalidOid on a schema predating the gate
   *  (the inversion-free carrier is then disabled). */
  Oid OID_FUNCTION_ANNOTATE;
  /** @brief OID of @c provsql.inversion_free_key(text,text,int)->text.
   *
   *  Builds the @c K-prefixed per-input order-key string the planner attaches
   *  (via @c annotate) to each certified atom's provenance on the
   *  inversion-free path.  @c InvalidOid on a schema predating it (markers are
   *  then not attached; the path declines and falls back). */
  Oid OID_FUNCTION_INVERSION_FREE_KEY;
  /** OIDs of the @c random_variable_{eq,ne,le,lt,ge,gt} comparison
   * procedure functions, indexed by the @c ComparisonOperator enum
   * (@c EQ=0, @c NE=1, @c LE=2, @c LT=3, @c GE=4, @c GT=5; matches the
   * order in @c src/Aggregation.h).  Used by the planner hook to detect
   * RV-comparison @c OpExpr nodes in WHERE clauses. */
  Oid OID_FUNCTION_RV_CMP[6];
  bool ok; ///< true if constants were loaded
} constants_t;




/** Structure to store the value of various constants for a specific
 * database. */
typedef struct database_constants_t {
  Oid database;          ///< OID of the database these constants belong to
  constants_t constants; ///< Cached OID constants for this database
} database_constants_t;

/**
 * @brief Retrieve the cached OID constants for the current database.
 *
 * On first call (or after a cache miss) this function looks up the OIDs
 * of all ProvSQL-specific types, functions, and operators in the system
 * catalogs and stores them in a per-database cache.  Subsequent calls
 * return the cached values without touching the catalogs.
 *
 * @param failure_if_not_possible  If @c true, call @c provsql_error when
 *        the ProvSQL schema cannot be found (e.g. the extension is not
 *        installed in the current database).  If @c false, return a
 *        constants_t with @c ok==false instead of aborting.
 * @return A @c constants_t whose @c ok field is @c true on success.
 */
constants_t get_constants(bool failure_if_not_possible);

/**
 * @brief Find the equality operator OID for two given types.
 *
 * Searches @c pg_operator for the @c = operator that accepts
 * @p ltypeId on the left and @p rtypeId on the right.
 *
 * @param ltypeId  OID of the left operand type.
 * @param rtypeId  OID of the right operand type.
 * @return The operator OID, or @c InvalidOid if none is found.
 */
Oid find_equality_operator(Oid ltypeId, Oid rtypeId);

/** Global variable that becomes true if this particular backend received
 * an interrupt signal. */
extern bool provsql_interrupted;

/** Global variable that indicates if where-provenance support has been
 * activated through the provsql.where_provenance run-time configuration
 * parameter. */
extern bool provsql_where_provenance;

/** Global variable that indicates the verbosity level set by the
 * provsql.verbose_level run-time configuration parameter was set */
extern int provsql_verbose;

/** Global variable holding the probability evaluation method(s) used by the
 * most recent probability_evaluate call, exposed via the
 * provsql.last_eval_method run-time configuration parameter. */
extern char *provsql_last_eval_method;

/** Global flag controlling agg_token text output: when true,
 * agg_token_out emits the underlying provenance UUID instead of the
 * default "value (*)" display string. Driven by the
 * provsql.aggtoken_text_as_uuid GUC. */
extern bool provsql_aggtoken_text_as_uuid;

/** Colon-separated list of directories prepended to PATH when ProvSQL
 * spawns external tools (d4, c2d, minic2d, dsharp, weightmc, graph-easy),
 * set by the provsql.tool_search_path run-time configuration parameter.
 * NULL or empty means rely on the server's PATH alone. */
extern char *provsql_tool_search_path;

/** Compiler invoked as the final fallback in BooleanCircuit::makeDD when
 * both interpretAsDD() and the in-process tree-decomposition path fail
 * (the latter typically on treewidth blow-up). Defaults to "d4"; set by
 * the provsql.fallback_compiler run-time configuration parameter to any
 * compiler accepted by BooleanCircuit::compilation (d4 / d4v2 / c2d /
 * minic2d / dsharp / panini-*). */
extern char *provsql_fallback_compiler;

/** Launch command for the managed KCMCP knowledge-compiler server, set by
 * the provsql.kcmcp_server run-time configuration parameter (PGC_SIGHUP).
 * The literal substring "{endpoint}" is replaced by the Unix-socket path the
 * supervisor background worker chooses, e.g. "tdkc --kcmcp unix:{endpoint}".
 * NULL or empty means no managed server is launched. */
extern char *provsql_kcmcp_server;

/** Read the live endpoint of the managed KCMCP server from shared memory
 * (e.g. "unix:/tmp/..."), or an empty string when none is running. */
const char *provsql_kcmcp_managed_endpoint(void);

/** Register the supervisor background worker that launches and supervises the
 * managed KCMCP server; called from _PG_init alongside the mmap worker. */
void RegisterProvSQLKCMCPWorker(void);

/** Seed for the Monte Carlo sampler, set by the provsql.monte_carlo_seed
 * run-time configuration parameter.  -1 (default) means seed from
 * std::random_device for non-deterministic sampling; any other value
 * (including 0) is a literal seed for std::mt19937_64.  Used by both
 * the Bernoulli path (BooleanCircuit::monteCarlo) and the continuous
 * path (gate_rv sampling), so a single GUC controls reproducibility
 * end-to-end. */
extern int provsql_monte_carlo_seed;

/** Default sample count for Monte Carlo fallbacks when an analytical
 * evaluator (Expectation, future hybrid evaluator, ...) cannot
 * decompose a sub-circuit structurally.  Unlike
 * @c probability_evaluate(token, 'monte-carlo', n) where the sample
 * count is an explicit argument, these implicit MC paths have no
 * natural place to take @c n from.
 *
 * Set by the @c provsql.rv_mc_samples run-time configuration
 * parameter; default 10000.  Setting it to 0 disables the implicit
 * MC fallback entirely: callers must then raise an exception rather
 * than sampling.  Useful for callers that want to guarantee
 * analytical-only evaluation. */
extern int provsql_rv_mc_samples;

/* Debug/safety hard cap on d-tree subproblems before the method bails to the
 * next (0 = off).  The chooser auto-budgets the d-tree at the next-best
 * method's estimated cost regardless; this imposes an extra fixed cap. */
extern int provsql_dtree_max_subproblems;

/** @brief When @c true (default), every @c GenericCircuit returned by
 * @c getGenericCircuit is run through the universal cmp-resolution
 * passes (RangeCheck for now, plus any future passes that decide
 * comparators to certain Boolean values).  Decisions become Bernoulli
 * @c gate_input gates with probability 0 or 1, transparent to every
 * downstream consumer (semiring evaluators, MC, view_circuit, PROV
 * export, etc.).  Set @c provsql.simplify_on_load to @c off when
 * inspecting a circuit's raw structure (e.g. debugging gate-creation
 * code paths). */
extern bool provsql_simplify_on_load;

/** @brief Run the hybrid evaluator (simplifier + per-cmp island
 *         decomposer) before dispatching a probability_evaluate query.
 *
 * Debug-only GUC, hidden from @c SHOW @c ALL and from
 * @c postgresql.conf.sample (registered with
 * @c GUC_NO_SHOW_ALL @c | @c GUC_NOT_IN_SAMPLE).  When on (default),
 * @c probability_evaluate runs the @c HybridEvaluator simplifier
 * between @c RangeCheck and @c AnalyticEvaluator and the per-cmp
 * MC island decomposer after @c AnalyticEvaluator: @c gate_arith
 * subtrees are constant-folded and family-closed (normals, Erlang),
 * and residual continuous-island comparators are MC-marginalised
 * into Bernoulli @c gate_input leaves so the surrounding circuit
 * becomes purely Boolean.
 *
 * Set to @c off to bypass both passes: undecidable comparators
 * then fall through to whole-circuit MC (for the @c monte-carlo
 * method) or raise (for @c independent / @c tree-decomposition).
 * End users have no reason to flip this -- on is strictly better
 * for them.  Exists for developer A/B testing of the analytic
 * path against the raw MC path and as a bisection knob if a
 * closure rule turns out to be unsound on some workload. */
extern bool provsql_hybrid_evaluation;

/** @brief Hidden diagnostic flag for the family of closed-form /
 *  analytic probability evaluators that resolve @c gate_cmps inside
 *  @c probability_evaluate ; see the
 *  @c provsql.cmp_probability_evaluation GUC.
 *
 *  When on (default), @c probability_evaluate runs pre-passes that
 *  recognise specific @c gate_cmp shapes and replace each cmp with
 *  a Bernoulli @c gate_input carrying the closed-form probability,
 *  bypassing the DNF that @c provsql_having's
 *  @c enumerate_valid_worlds would otherwise emit.  The first
 *  implementation in this family is the Poisson-binomial pre-pass
 *  for HAVING @c COUNT(*) @c op @c C over distinct @c gate_input
 *  leaves (see @c CountCmpEvaluator.h) ; future MIN / MAX / SUM
 *  evaluators will gate on the same flag.  Off forces every cmp to
 *  fall through to the enumeration path.  End users have no reason
 *  to flip this ; exists for developer A/B testing and as a
 *  bisection escape valve. */
extern bool provsql_cmp_probability_evaluation;

/** @brief Kill-switch for the inversion-free structured-d-DNNF probability
 *  path; see the @c provsql.inversion_free GUC.
 *
 *  When on (default), @c probability_evaluate, on a query carrying an
 *  inversion-free tractability certificate, tries the structured-d-DNNF
 *  builder after @c independentEvaluation and before tree-decomposition / d4.
 *  Off disables only that automatic insertion (for A/B testing); the explicit
 *  @c probability_evaluate(token,'inversion-free') method ignores this flag.
 *  The path is self-gating on the certificate, which is attached only to
 *  certified queries, so leaving it on is safe. */
extern bool provsql_inversion_free;

/** @brief Opt-in safe-query optimisation for hierarchical conjunctive
 *  queries; see the @c provsql.boolean_provenance GUC.
 *
 *  When @c true, the planner is permitted to rewrite self-join-free
 *  hierarchical CQs (and independent UCQs) over TID / BID tables to
 *  a read-once form whose probability is computable in linear time.
 *  The rewriter tags the resulting root gate so that semiring
 *  evaluations incompatible with this rewrite refuse to run on the
 *  produced circuit. */
extern bool provsql_boolean_provenance;

/** @brief Derived flag of the @c provsql.provenance GUC: the session's
 *  provenance class is 'absorptive' or 'boolean', licensing
 *  constructions sound for absorptive semirings only (cyclic recursion
 *  stopped at the absorptive value fixpoint, tagged tokens). */
extern bool provsql_absorptive_provenance;

#include "MMappedTableInfo.h"

/**
 * @brief Look up per-table provenance metadata with a backend-local cache.
 *
 * Resolves to a cached value when the relation's relcache entry has
 * not been invalidated since the last fetch; otherwise issues one
 * @c 's' IPC to the background worker.  The cache is invalidated
 * via @c CacheRegisterRelcacheCallback, so concurrent
 * @c add_provenance / @c repair_key / @c remove_provenance in other
 * backends are reflected here without polling.
 *
 * Safe to call from the planner hot path.
 *
 * @param relid  pg_class OID of the relation to look up.
 * @param out    On @c true return, filled with the stored record.
 * @return @c true if a record exists for @p relid, @c false otherwise.
 */
extern bool provsql_lookup_table_info(Oid relid, ProvenanceTableInfo *out);

/**
 * @brief Raw IPC fetch (no cache).
 *
 * Implementation detail of @c provsql_lookup_table_info, exposed only
 * so the cache layer in @c provsql_utils.c can reach it.  Callers in
 * the planner hot path should go through @c provsql_lookup_table_info.
 */
extern bool provsql_fetch_table_info(Oid relid, ProvenanceTableInfo *out);

/**
 * @brief Look up the base-ancestor set of a tracked relation.
 *
 * Per-backend cached over IPC.  Returns the ancestor set when
 * @p relid is tracked and the registry has a non-empty entry for it.
 * @c false either when @p relid has no metadata record at all (the
 * relation was never run through @c add_provenance / @c repair_key)
 * or when the record exists but @c ancestor_n @c == @c 0 (the CTAS
 * hook hasn't populated the lineage yet, or the registry was
 * explicitly cleared).  The two failure modes share the false
 * return because both make the safe-query rewriter take the
 * conservative refuse path -- there is no use case for treating
 * them differently.
 *
 * Backed by the same per-backend cache as
 * @c provsql_lookup_table_info and invalidated through the same
 * relcache-invalidation callback, so concurrent
 * @c set_ancestors / @c add_provenance / @c repair_key calls in
 * other backends are reflected here without polling.
 *
 * @param relid           pg_class OID of the relation to look up.
 * @param ancestor_n_out  On @c true return, count of valid entries
 *                        in @p ancestors_out.
 * @param ancestors_out   On @c true return, the sorted-deduplicated
 *                        ancestor OIDs (caller-allocated buffer of
 *                        @c PROVSQL_TABLE_INFO_MAX_ANCESTORS @c Oid).
 * @return @c true on a non-empty ancestor set; @c false otherwise.
 */
extern bool provsql_lookup_ancestry(Oid relid,
                                    uint16 *ancestor_n_out,
                                    Oid *ancestors_out);

/**
 * @brief Raw IPC fetch for the ancestry half (no cache).
 *
 * Implementation detail of @c provsql_lookup_ancestry, exposed so
 * the cache layer in @c provsql_utils.c can reach it.  Callers in
 * the planner hot path should go through @c provsql_lookup_ancestry.
 */
extern bool provsql_fetch_ancestry(Oid relid,
                                   uint16 *ancestor_n_out,
                                   Oid *ancestors_out);

/**
 * @brief Upper bounds for the relation-key cache.
 *
 * Each relation contributes at most @c PROVSQL_KEY_CACHE_MAX_KEYS
 * distinct PRIMARY-KEY / NOT-NULL-UNIQUE column-sets, each over at
 * most @c PROVSQL_KEY_CACHE_MAX_KEY_COLS columns.  These bounds keep
 * the cache entry fixed-size (so the backend-local sorted-array
 * representation can reuse the @c provsql_lookup_table_info pattern
 * verbatim); relations with more or wider keys silently drop the
 * overflow, treating the missing keys as if they did not exist
 * (over-conservative -- the §2 FD-aware detector simply misses an
 * optimisation, never produces an unsound rewrite).
 */
#define PROVSQL_KEY_CACHE_MAX_KEYS      4
#define PROVSQL_KEY_CACHE_MAX_KEY_COLS  8

/**
 * @brief One PRIMARY-KEY or NOT-NULL-UNIQUE key on a relation.
 *
 * @c col_n is the number of valid entries in @c cols (in
 * @c pg_index.indkey order, i.e. column position in the key, not
 * @c pg_attribute.attnum order).  All columns are NOT NULL by
 * construction: PRIMARY KEY enforces this implicitly, and UNIQUE
 * constraints are admitted only when @c pg_attribute.attnotnull is
 * @c true for every constituent column (the §2 soundness trap on
 * nullable UNIQUE).
 */
typedef struct ProvenanceRelationKey {
  uint16     col_n;
  AttrNumber cols[PROVSQL_KEY_CACHE_MAX_KEY_COLS];
} ProvenanceRelationKey;

/**
 * @brief Per-relation set of PRIMARY-KEY and NOT-NULL-UNIQUE keys.
 *
 * Populated by @c provsql_lookup_relation_keys from @c pg_constraint
 * (filtered by @c contype @c IN @c ('p','u')) joined to
 * @c pg_index and @c pg_attribute (for the NOT-NULL check).  The
 * detector's §2 PK-FD pass walks @c keys and, for every key @c K it
 * recognises among the query's equijoin equivalence classes, tags
 * the determined columns as functionally fixed inside the relevant
 * RTE.
 */
typedef struct ProvenanceRelationKeys {
  Oid                    relid;
  uint16                 key_n;
  ProvenanceRelationKey  keys[PROVSQL_KEY_CACHE_MAX_KEYS];
} ProvenanceRelationKeys;

/**
 * @brief Look up the PRIMARY-KEY and NOT-NULL-UNIQUE keys of a
 *        relation with a backend-local cache.
 *
 * Companion to @c provsql_lookup_table_info.  The cache lives in a
 * separate backing array with its own relcache-invalidation
 * callback so that a future @c ALTER @c TABLE that adds / drops a
 * constraint refreshes the next lookup without polling.  Returns
 * @c true when the relation has at least one PRIMARY KEY or
 * NOT-NULL UNIQUE constraint; @c false otherwise (in which case
 * @p *out is filled with @c key_n @c = @c 0).  Safe to call from
 * the planner hot path.
 *
 * @param relid  pg_class OID of the relation to inspect.
 * @param out    Filled on return.  @c out->relid is set to @p relid
 *               regardless of return value; @c out->keys holds up to
 *               @c PROVSQL_KEY_CACHE_MAX_KEYS keys.
 */
extern bool provsql_lookup_relation_keys(Oid relid,
                                         ProvenanceRelationKeys *out);

#include "provsql_error.h"

#ifdef __cplusplus
/* Neutralise the PostgreSQL macros (gettext family, port.h's printf-family
 * replacements) that break STL / Boost headers included after this point;
 * see c_cpp_compatibility.h.  Done here so every C++ translation unit that
 * pulls in the PostgreSQL headers through provsql_utils.h is covered
 * without having to mind its include order. */
#include "c_cpp_compatibility.h"
#endif

#endif /* PROVSQL_UTILS_H */
