/**
 * @file safe_query_cert.h
 * @brief Tractability certificate for the inversion-free UCQ(OBDD) path.
 *
 * Shared between the C planner side (@c src/safe_query.c, which builds the
 * certificate from a query-level analysis) and the C++ evaluation side
 * (@c src/probability_evaluate.cpp, which will, in a later phase, read it back
 * from the annotation gate's @c extra and route probability evaluation through
 * the structured-d-DNNF builder).
 *
 * Phase 1 produces the certificate (the @c SafeCert "recipe") from the detector
 * but does not yet attach or consume it; only the struct/enum and the @c extra
 * discriminator prefixes are needed here.  The compact serialise / parse helpers
 * for the annotation-gate carrier are added in phase 2.
 */
#ifndef SAFE_QUERY_CERT_H
#define SAFE_QUERY_CERT_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Kind of tractability certificate carried on the provenance root. */
typedef enum SafeCertKind {
  CERT_NONE            = 0,  ///< No certificate.
  CERT_INVERSION_FREE  = 1   ///< Inversion-free UCQ(OBDD) over TID inputs.
} SafeCertKind;

/**
 * @brief Query-derived order recipe for the structured-d-DNNF builder.
 *
 * The recipe is the static, query-level half of the Prop. 4.5 order: the class
 * topological order (root class first), a relation-symbol tie-break rank per
 * atom, and the per-atom column->class anchor map.  Combined at evaluation time
 * with the per-input keys (carried on the input annotation gates), it yields a
 * total order consistent with Prop. 4.5.  Arrays are @c palloc'd in the current
 * memory context.
 */
typedef struct SafeCert {
  SafeCertKind kind;

  int  nclasses;             ///< Number of (compacted) equivalence classes.
  int  root_class;           ///< Compacted id of the root class (touches every atom).
  int  natoms;               ///< Number of atoms (range-table entries).

  int *class_topo_order;     ///< Length @c nclasses: classes in @c G_prec topological order, root first.
  int *atom_relation_rank;   ///< Length @c natoms: relation-symbol tie-break rank per atom.

  int  maxarity;             ///< Stride of @c atom_col_class (max columns per atom seen).
  int *atom_col_class;       ///< Flattened [natoms][maxarity]: compacted class anchored at (atom, column), or -1.
} SafeCert;

/**
 * @brief Discriminator prefixes for the annotation gate's @c extra payload.
 *
 * One transparent annotation gate type carries both roles, disambiguated by the
 * first byte of @c extra: a serialised @c SafeCert recipe on the root, and a
 * per-input order key on each certified input leaf.  (Used from phase 2 on.)
 */
#define SAFE_CERT_EXTRA_PREFIX_RECIPE 'C'  ///< Root: serialised SafeCert recipe.
#define SAFE_CERT_EXTRA_PREFIX_KEY    'K'  ///< Input: per-variable order key.

/**
 * @brief Serialise a @c SafeCert recipe to a compact, @c C-prefixed string
 *        (palloc'd in the current memory context).  Inverse of @c safe_cert_parse.
 */
extern char *safe_cert_serialise(const SafeCert *cert);

/**
 * @brief Parse a @c C-prefixed recipe string (as produced by
 *        @c safe_cert_serialise and read back from an annotation gate's
 *        @c extra) into a palloc'd @c SafeCert.  Returns NULL if @p str is
 *        NULL, not @c C-prefixed, or malformed.
 */
extern SafeCert *safe_cert_parse(const char *str);

/**
 * @brief Per-input order key carried on an input leaf's annotation gate.
 *
 * The structured-d-DNNF builder (@c StructuredDNNFBuilder) needs, per Boolean
 * variable, its position in the query hierarchy: @c root the root-class value
 * (one independent block per value), @c sec the secondary-class value (one tile
 * per value within a block), and @c factor which quantified factor the atom
 * belongs to, or @c SAFE_CERT_GUARD_FACTOR for a self-join atom shared by every
 * factor of its tile.  This is the wire form of @c StructuredDNNFBuilder's
 * @c InputKey.
 *
 * @c root and @c sec are the tuple's physical column values; phase-1 keys carry
 * them as @c long (the canonical-int domain of the witness).  Generalising to
 * arbitrary value domains (a comparable text / value-id encoding) is a
 * documented follow-up: grouping needs exact value identity, ordering only a
 * consistent total order.
 */
#define SAFE_CERT_GUARD_FACTOR (-1)  /* factor of a shared self-join guard */

typedef struct SafeCertKey {
  long root;    /* root-class value (block) */
  long sec;     /* secondary-class value (tile) */
  int  factor;  /* factor id, or SAFE_CERT_GUARD_FACTOR for a shared guard */
} SafeCertKey;

/**
 * @brief Serialise a per-input order key to a compact @c K-prefixed string
 *        (palloc'd).  Wire form: @c "K<root> <sec> <factor>".  Inverse of
 *        @c safe_cert_key_parse.  (The planner builds the equivalent string per
 *        row from the tuple's column values; this C form is for the evaluator
 *        and tests.)
 */
extern char *safe_cert_key_serialise(long root, long sec, int factor);

/**
 * @brief Parse a @c K-prefixed order-key string into @p out.  Returns @c false
 *        if @p str is NULL, not @c K-prefixed, or malformed (@p out untouched).
 */
extern bool safe_cert_key_parse(const char *str, SafeCertKey *out);

#ifdef __cplusplus
}
#endif

#endif /* SAFE_QUERY_CERT_H */
