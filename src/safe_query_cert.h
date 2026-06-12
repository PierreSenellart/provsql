/**
 * @file safe_query_cert.h
 * @brief Tractability certificate for the inversion-free UCQ(OBDD) path.
 *
 * Shared between the C planner side (@c src/safe_query.c, which builds the
 * certificate from a query-level analysis) and the C++ evaluation side
 * (@c src/probability_evaluate.cpp, which reads it back from the annotation
 * gate's @c extra and routes probability evaluation through the
 * structured-d-DNNF builder).
 *
 * The planner produces the certificate (the @c SafeCert "recipe") from the
 * detector and stamps it on the per-row provenance root; the compact serialise
 * / parse helpers below are the annotation-gate carrier the evaluator reads.
 */
#ifndef SAFE_QUERY_CERT_H
#define SAFE_QUERY_CERT_H

#include <stddef.h>  /* size_t */

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
 * per-input order key on each certified input leaf.
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
 * @c root and @c sec are the tuple's column values as text (the canonical output
 * of the column type's I/O function), carried verbatim so the key works for any
 * column type, not just integers.  The builder uses them only for grouping
 * (equal text => same block / tile) and a consistent total order, so an exact,
 * type-faithful text rendering is all that is required; @c root / @c sec point
 * into the parsed wire string and are @em not NUL-terminated (use the lengths).
 */
#define SAFE_CERT_GUARD_FACTOR (-1)  /* factor of a shared self-join guard */

typedef struct SafeCertKey {
  const char *root;     /* root-class value text (block); root_len bytes */
  size_t      root_len;
  const char *sec;      /* secondary-class value text (tile); sec_len bytes */
  size_t      sec_len;
  int         factor;   /* factor id, or SAFE_CERT_GUARD_FACTOR for a shared guard */
} SafeCertKey;

/**
 * @brief Parse a @c K-prefixed order-key string into @p out.  On success
 *        @c out->root / @c out->sec point into @p str (valid for its lifetime).
 *        Returns @c false if @p str is NULL, not @c K-prefixed, or malformed
 *        (@p out untouched).
 */
extern bool safe_cert_key_parse(const char *str, SafeCertKey *out);

#ifdef __cplusplus
}
#endif

#endif /* SAFE_QUERY_CERT_H */
