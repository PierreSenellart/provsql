/**
 * @file classify_having.c
 * @brief Query-time HAVING-trichotomy classifier.  See classify_having.h.
 *
 * For each @c alpha(y) @c theta @c k comparison in @c q->havingQual, emits
 * a @c NOTICE giving the Ré-Suciu trichotomy verdict, combining the static
 * @c (alpha, theta) overlay with the skeleton-safety axis (computed once
 * per query by @c safe_query_skeleton_is_hierarchical).  Read-only.
 */
#include "postgres.h"
#include "fmgr.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "utils/lsyscache.h"

#include "classify_having.h"
#include "safe_query.h"
#include "provsql_utils.h"
#include "provsql_error.h"

/** @brief Backing storage for the @c provsql.classify_having GUC. */
bool provsql_classify_having = false;

/** @brief Aggregate function of a HAVING comparison. */
typedef enum {
  HAGG_MIN, HAGG_MAX, HAGG_COUNT, HAGG_COUNT_DISTINCT,
  HAGG_SUM, HAGG_AVG, HAGG_OTHER
} having_agg;

/** @brief Comparison operator of a HAVING comparison. */
typedef enum {
  HOP_EQ, HOP_NE, HOP_LT, HOP_LE, HOP_GT, HOP_GE, HOP_OTHER
} having_op;

/** @brief Map an @c Aggref to a @c having_agg (NULL aggfnoid name → OTHER). */
static having_agg agg_of(const Aggref *ar) {
  char *name = get_func_name(ar->aggfnoid);
  having_agg a = HAGG_OTHER;
  if (name == NULL)
    return HAGG_OTHER;
  if (strcmp(name, "count") == 0)
    a = (ar->aggdistinct != NIL) ? HAGG_COUNT_DISTINCT : HAGG_COUNT;
  else if (strcmp(name, "sum") == 0) a = HAGG_SUM;
  else if (strcmp(name, "min") == 0) a = HAGG_MIN;
  else if (strcmp(name, "max") == 0) a = HAGG_MAX;
  else if (strcmp(name, "avg") == 0) a = HAGG_AVG;
  pfree(name);
  return a;
}

/** @brief Map an operator name to a @c having_op. */
static having_op op_of_name(const char *n) {
  if (n == NULL)            return HOP_OTHER;
  if (strcmp(n, "=")  == 0) return HOP_EQ;
  if (strcmp(n, "<>") == 0) return HOP_NE;
  if (strcmp(n, "<")  == 0) return HOP_LT;
  if (strcmp(n, "<=") == 0) return HOP_LE;
  if (strcmp(n, ">")  == 0) return HOP_GT;
  if (strcmp(n, ">=") == 0) return HOP_GE;
  return HOP_OTHER;
}

/** @brief Flip an operator (for the @c k @c theta @c alpha(y) operand order). */
static having_op flip(having_op op) {
  switch (op) {
    case HOP_LT: return HOP_GT;
    case HOP_LE: return HOP_GE;
    case HOP_GT: return HOP_LT;
    case HOP_GE: return HOP_LE;
    default:     return op;       /* =, <>, OTHER are symmetric */
  }
}

static const char *agg_label(having_agg a) {
  switch (a) {
    case HAGG_MIN:            return "MIN";
    case HAGG_MAX:            return "MAX";
    case HAGG_COUNT:          return "COUNT";
    case HAGG_COUNT_DISTINCT: return "COUNT(DISTINCT)";
    case HAGG_SUM:            return "SUM";
    case HAGG_AVG:            return "AVG";
    default:                  return "?";
  }
}

static const char *op_label(having_op op) {
  switch (op) {
    case HOP_EQ: return "=";
    case HOP_NE: return "<>";
    case HOP_LT: return "<";
    case HOP_LE: return "<=";
    case HOP_GT: return ">";
    case HOP_GE: return ">=";
    default:     return "?";
  }
}

/* Layer-2 (approximation) overlay for an unsafe skeleton, where the exact
 * problem is #P-hard.  Direction-asymmetric; see the trichotomy tables in
 * doc/source/dev/probability-evaluation.rst. */
static const char *overlay_unsafe(having_agg a, having_op op) {
  switch (a) {
    case HAGG_MIN:
      if (op == HOP_LT || op == HOP_LE) return "apx-safe (#P-hard exact, FPRAS exists)";
      if (op == HOP_NE)                 return "open (approximability not classified)";
      return "hazardous (#P-hard exact, no FPRAS)";
    case HAGG_MAX:
      if (op == HOP_GT || op == HOP_GE) return "apx-safe (#P-hard exact, FPRAS exists)";
      if (op == HOP_NE)                 return "open (approximability not classified)";
      return "hazardous (#P-hard exact, no FPRAS)";
    case HAGG_COUNT:
      if (op == HOP_LT || op == HOP_LE || op == HOP_EQ)
        return "apx-safe or hazardous (#P-hard exact; decidable in PTIME)";
      return "open (#P-hard exact; approximability not classified)";
    case HAGG_SUM:
      if (op == HOP_LT || op == HOP_LE || op == HOP_EQ)
        return "hazardous (#P-hard exact, no FPRAS)";
      return "open (#P-hard exact; approximability not classified)";
    case HAGG_AVG:
    case HAGG_COUNT_DISTINCT:
      return "#P-hard exact; approximability open (Sect. 6 covers only MIN/MAX/SUM)";
    default:
      return "not classified";
  }
}

/* Full verdict for (alpha, theta) given the skeleton-safety bit. */
static const char *verdict(having_agg a, having_op op, bool skeleton_safe) {
  if (skeleton_safe) {
    switch (a) {
      case HAGG_MIN: case HAGG_MAX: case HAGG_COUNT:
        return "safe -- exact PTIME";
      case HAGG_SUM: case HAGG_AVG:
        return "exact PTIME if alpha-safe, else #P-hard (alpha-safety not checked)";
      case HAGG_COUNT_DISTINCT:
        return "exact PTIME if COUNT(DISTINCT)-safe, else #P-hard";
      default:
        return "not classified";
    }
  }
  return overlay_unsafe(a, op);
}

/** @brief Walker context: the per-query skeleton-safety bit. */
typedef struct {
  bool skeleton_safe;
} having_walk_ctx;

/* Detect alpha(y) theta k comparisons (either operand order) and emit a
 * NOTICE for each.  Does not recurse into Aggref arguments. */
static bool classify_walker(Node *node, void *vctx) {
  if (node == NULL)
    return false;
  if (IsA(node, OpExpr)) {
    OpExpr *oe = (OpExpr *) node;
    if (list_length(oe->args) == 2) {
      Node *l = (Node *) linitial(oe->args);
      Node *r = (Node *) lsecond(oe->args);
      Node *ln = (Node *) strip_implicit_coercions(l);
      Node *rn = (Node *) strip_implicit_coercions(r);
      Aggref *ar = NULL;
      bool agg_on_left = false;
      if (IsA(ln, Aggref)) { ar = (Aggref *) ln; agg_on_left = true; }
      else if (IsA(rn, Aggref)) { ar = (Aggref *) rn; agg_on_left = false; }

      if (ar != NULL) {
        having_walk_ctx *ctx = (having_walk_ctx *) vctx;
        having_agg a  = agg_of(ar);
        char     *opn = get_opname(oe->opno);
        having_op op  = op_of_name(opn);
        if (opn) pfree(opn);
        if (!agg_on_left)
          op = flip(op);
        if (a != HAGG_OTHER && op != HOP_OTHER)
          provsql_notice("HAVING (%s, %s), skeleton %s: %s",
                         agg_label(a), op_label(op),
                         ctx->skeleton_safe ? "safe" : "unsafe",
                         verdict(a, op, ctx->skeleton_safe));
        /* don't recurse into the aggregate's own arguments */
        return false;
      }
    }
  }
  return expression_tree_walker(node, classify_walker, vctx);
}

void provsql_emit_having_classification(Query *q, const constants_t *constants) {
  having_walk_ctx ctx;
  if (q == NULL || q->havingQual == NULL)
    return;
  ctx.skeleton_safe = safe_query_skeleton_is_hierarchical(constants, q);
  classify_walker(q->havingQual, &ctx);
}
