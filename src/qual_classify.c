/**
 * @file qual_classify.c
 * @brief Implementation of the shared predicate-tree classification
 *        helpers.  See @c qual_classify.h.
 */
#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#if PG_VERSION_NUM >= 120000
#include "optimizer/optimizer.h"
#else
#include "optimizer/clauses.h"          /* contain_volatile_functions (PG <12) */
#endif

#include "qual_classify.h"
#include "provsql_utils.h"

bool qc_collect_vars_walker(Node *node, qc_vars_ctx *ctx)
{
  ListCell *lc;
  if (node == NULL)
    return false;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->varlevelsup != 0)
      return false;
    foreach (lc, ctx->vars) {
      Var *existing = (Var *) lfirst(lc);
      if (existing->varno == v->varno && existing->varattno == v->varattno)
        return false;
    }
    ctx->vars = lappend(ctx->vars, v);
    return false;
  }
  return expression_tree_walker(node, qc_collect_vars_walker, ctx);
}

int qc_var_index(List *vars, Index varno, AttrNumber varattno)
{
  ListCell *lc;
  int i = 0;
  foreach (lc, vars) {
    Var *v = (Var *) lfirst(lc);
    if (v->varno == varno && v->varattno == varattno)
      return i;
    i++;
  }
  return -1;
}

bool qc_is_var_eq(Expr *qual, Var **l, Var **r)
{
  OpExpr *op;
  Node   *ln, *rn;
  Var    *lv, *rv;
  Oid     expected;

  if (!IsA(qual, OpExpr))
    return false;
  op = (OpExpr *) qual;
  if (list_length(op->args) != 2)
    return false;
  ln = (Node *) linitial(op->args);
  rn = (Node *) lsecond(op->args);
  while (IsA(ln, RelabelType))
    ln = (Node *) ((RelabelType *) ln)->arg;
  while (IsA(rn, RelabelType))
    rn = (Node *) ((RelabelType *) rn)->arg;
  if (!IsA(ln, Var) || !IsA(rn, Var))
    return false;
  lv = (Var *) ln;
  rv = (Var *) rn;
  if (lv->varlevelsup != 0 || rv->varlevelsup != 0)
    return false;
  expected = find_equality_operator(lv->vartype, rv->vartype);
  if (expected == InvalidOid || op->opno != expected)
    return false;
  *l = lv;
  *r = rv;
  return true;
}

bool qc_is_var_const_eq(Expr *qual, Var **var, Const **konst)
{
  OpExpr *op;
  Node   *ln, *rn;
  Var    *v;
  Const  *k;
  Oid     expected;

  if (!IsA(qual, OpExpr))
    return false;
  op = (OpExpr *) qual;
  if (list_length(op->args) != 2)
    return false;
  ln = (Node *) linitial(op->args);
  rn = (Node *) lsecond(op->args);
  while (IsA(ln, RelabelType))
    ln = (Node *) ((RelabelType *) ln)->arg;
  while (IsA(rn, RelabelType))
    rn = (Node *) ((RelabelType *) rn)->arg;
  if (IsA(ln, Var) && IsA(rn, Const)) {
    v = (Var *) ln;
    k = (Const *) rn;
  } else if (IsA(ln, Const) && IsA(rn, Var)) {
    k = (Const *) ln;
    v = (Var *) rn;
  } else {
    return false;
  }
  if (v->varlevelsup != 0)
    return false;
  if (k->constisnull)
    return false;
  expected = find_equality_operator(v->vartype, k->consttype);
  if (expected == InvalidOid || op->opno != expected)
    return false;
  *var   = v;
  *konst = k;
  return true;
}

void qc_collect_equalities(Node *quals, List **out)
{
  if (quals == NULL)
    return;
  if (IsA(quals, List)) {
    ListCell *lc;
    foreach (lc, (List *) quals)
      qc_collect_equalities((Node *) lfirst(lc), out);
    return;
  }
  if (IsA(quals, BoolExpr)) {
    BoolExpr *be = (BoolExpr *) quals;
    if (be->boolop == AND_EXPR) {
      ListCell *lc;
      foreach (lc, be->args)
        qc_collect_equalities((Node *) lfirst(lc), out);
    }
    return;
  }
  {
    Var *l, *r;
    if (qc_is_var_eq((Expr *) quals, &l, &r)) {
      *out = lappend(*out, l);
      *out = lappend(*out, r);
    }
  }
}

bool qc_collect_varnos_walker(Node *node, qc_varnos_ctx *ctx)
{
  if (node == NULL)
    return false;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    if (v->varlevelsup == 0 && (int) v->varno >= 1)
      ctx->varnos = bms_add_member(ctx->varnos, (int) v->varno);
    return false;
  }
  return expression_tree_walker(node, qc_collect_varnos_walker, ctx);
}

void qc_flatten_and(Node *n, List **out)
{
  if (n == NULL)
    return;
  if (IsA(n, List)) {
    ListCell *lc;
    foreach (lc, (List *) n)
      qc_flatten_and((Node *) lfirst(lc), out);
    return;
  }
  if (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop == AND_EXPR) {
    ListCell *lc;
    foreach (lc, ((BoolExpr *) n)->args)
      qc_flatten_and((Node *) lfirst(lc), out);
    return;
  }
  *out = lappend(*out, n);
}

void qc_split_quals(Node *quals, int natoms,
                    List **per_atom_out, Node **out_residual)
{
  List     *conjuncts = NIL;
  List     *residual  = NIL;
  ListCell *lc;
  int       i;

  for (i = 0; i < natoms; i++)
    per_atom_out[i] = NIL;

  qc_flatten_and(quals, &conjuncts);

  foreach (lc, conjuncts) {
    Node *qual = (Node *) lfirst(lc);
    qc_varnos_ctx vctx = { NULL };
    int nmembers;

    if (contain_volatile_functions(qual)) {
      residual = lappend(residual, qual);
      continue;
    }

    qc_collect_varnos_walker(qual, &vctx);
    nmembers = bms_num_members(vctx.varnos);
    if (nmembers == 1) {
      int v = bms_singleton_member(vctx.varnos);
      if (v >= 1 && v <= natoms) {
        per_atom_out[v - 1] = lappend(per_atom_out[v - 1], qual);
        bms_free(vctx.varnos);
        continue;
      }
    }
    bms_free(vctx.varnos);
    residual = lappend(residual, qual);
  }

  if (residual == NIL)
    *out_residual = NULL;
  else if (list_length(residual) == 1)
    *out_residual = (Node *) linitial(residual);
  else
    *out_residual = (Node *) makeBoolExpr(AND_EXPR, residual, -1);
}
