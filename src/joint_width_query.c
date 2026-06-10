/**
 * @file joint_width_query.c
 * @brief Implementation of the planner-time UCQ recogniser (descriptor
 *        extraction).  See @c joint_width_query.h.
 */
#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "joint_width_query.h"
#include "provsql_utils.h"

/** @brief A column reference (range-table index, attribute number). */
typedef struct ColRef {
  Index      rtindex;
  AttrNumber attno;
  int        parent;   /* union-find parent index into the ColRef array */
} ColRef;

/** @brief Find-or-add a (rtindex, attno) column node; returns its index. */
static int colref_get(ColRef *cols, int *ncols, Index rtindex, AttrNumber attno)
{
  int i;
  for (i = 0; i < *ncols; i++)
    if (cols[i].rtindex == rtindex && cols[i].attno == attno)
      return i;
  cols[*ncols].rtindex = rtindex;
  cols[*ncols].attno = attno;
  cols[*ncols].parent = *ncols;
  return (*ncols)++;
}

/** @brief Union-find root with path halving. */
static int uf_find(ColRef *cols, int i)
{
  while (cols[i].parent != i) {
    cols[i].parent = cols[cols[i].parent].parent;
    i = cols[i].parent;
  }
  return i;
}

static void uf_union(ColRef *cols, int a, int b)
{
  int ra = uf_find(cols, a), rb = uf_find(cols, b);
  if (ra != rb)
    cols[ra].parent = rb;
}

/** @brief Collect Var=Var equalities from a WHERE qual tree; false on any
 *         qual that is not such an equality (so the recogniser declines). */
static bool collect_equalities(Node *qual, ColRef *cols, int *ncols,
                               int **pairs, int *npairs)
{
  if (qual == NULL)
    return true;
  if (IsA(qual, BoolExpr)) {
    BoolExpr *b = (BoolExpr *) qual;
    ListCell *lc;
    if (b->boolop != AND_EXPR)
      return false;
    foreach (lc, b->args)
      if (!collect_equalities((Node *) lfirst(lc), cols, ncols, pairs, npairs))
        return false;
    return true;
  }
  if (IsA(qual, OpExpr)) {
    OpExpr *op = (OpExpr *) qual;
    Node *ln, *rn;
    Var *lv, *rv;
    Oid eq;
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
    eq = lookup_type_cache(lv->vartype, TYPECACHE_EQ_OPR)->eq_opr;
    if (eq == InvalidOid || op->opno != eq)
      return false;
    {
      int a = colref_get(cols, ncols, lv->varno, lv->varattno);
      int b = colref_get(cols, ncols, rv->varno, rv->varattno);
      (*pairs)[(*npairs)++] = a;
      (*pairs)[(*npairs)++] = b;
    }
    return true;
  }
  return false;   /* any other qual shape: decline */
}

/** @brief Append @p s as a JSON string literal (escaping @c " and @c \\). */
static void append_json_string(StringInfo buf, const char *s)
{
  appendStringInfoChar(buf, '"');
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') {
      appendStringInfoChar(buf, '\\');
      appendStringInfoChar(buf, *s);
    } else if (*s == '\n') {
      appendStringInfoString(buf, "\\n");
    } else {
      appendStringInfoChar(buf, *s);
    }
  }
  appendStringInfoChar(buf, '"');
}

/** @brief Walker context: does any target-list Var reach a tracked atom? */
typedef struct {
  Index *rtis;
  int    natoms;
  bool   found;
} ExistCtx;

static bool exist_walker(Node *node, void *vctx)
{
  ExistCtx *ctx = (ExistCtx *) vctx;
  if (node == NULL)
    return false;
  if (IsA(node, Var)) {
    Var *v = (Var *) node;
    int i;
    for (i = 0; i < ctx->natoms; i++)
      if (v->varno == ctx->rtis[i]) {
        ctx->found = true;
        return true;
      }
    return false;
  }
  return expression_tree_walker(node, exist_walker, vctx);
}

char *provsql_joint_width_descriptor(const constants_t *constants, Query *q,
                                     bool *all_existential,
                                     List **head_var_idx, List **head_exprs)
{
  ListCell  *lc;
  int        natoms = 0;
  Index     *atom_rti;        /* per atom: range-table index */
  Oid       *atom_relid;      /* per atom: relation OID */
  int       *atom_relidx;     /* per atom: index into the distinct relations */
  Oid       *rel_oids;        /* distinct relation OIDs */
  int        nrel = 0;
  ColRef    *cols;
  int        ncols = 0;
  int       *pairs;
  int        npairs = 0;
  int        maxcols;
  StringInfoData buf;
  int        i, a;

  if (all_existential)
    *all_existential = false;

  /* Shape gate: a plain SELECT, single CQ (no set ops, no aggregates),
   * with a jointree.  DISTINCT / GROUP BY are allowed (they are how the
   * existence / per-answer questions are written). */
  if (q->commandType != CMD_SELECT || q->setOperations != NULL ||
      q->jointree == NULL || q->hasAggs ||
      q->havingQual != NULL ||
      q->hasWindowFuncs || q->hasSubLinks || q->cteList != NIL)
    return NULL;

  /* The FROM list must be a flat list of RangeTblRefs to tracked base
   * relations. */
  if (q->jointree->fromlist == NIL)
    return NULL;
  atom_rti   = palloc(list_length(q->rtable) * sizeof(Index));
  atom_relid = palloc(list_length(q->rtable) * sizeof(Oid));
  atom_relidx = palloc(list_length(q->rtable) * sizeof(int));
  rel_oids   = palloc(list_length(q->rtable) * sizeof(Oid));
  foreach (lc, q->jointree->fromlist) {
    Node *n = (Node *) lfirst(lc);
    RangeTblEntry *rte;
    Index rti;
    if (!IsA(n, RangeTblRef))
      return NULL;
    rti = ((RangeTblRef *) n)->rtindex;
    rte = rt_fetch(rti, q->rtable);
    if (rte->rtekind != RTE_RELATION)
      return NULL;
    /* Tracked iff it has a uuid provsql column. */
    if (get_attnum(rte->relid, PROVSQL_COLUMN_NAME) == InvalidAttrNumber)
      return NULL;
    atom_rti[natoms] = rti;
    atom_relid[natoms] = rte->relid;
    natoms++;
  }
  if (natoms == 0)
    return NULL;

  /* Map distinct relation OIDs to relation indices. */
  for (a = 0; a < natoms; a++) {
    int found = -1;
    for (i = 0; i < nrel; i++)
      if (rel_oids[i] == atom_relid[a]) { found = i; break; }
    if (found < 0) { rel_oids[nrel] = atom_relid[a]; found = nrel++; }
    atom_relidx[a] = found;
  }

  /* Upper bound on column nodes: every atom's every column. */
  maxcols = 0;
  for (a = 0; a < natoms; a++) {
    RangeTblEntry *rte = rt_fetch(atom_rti[a], q->rtable);
    maxcols += list_length(rte->eref->colnames);
  }
  cols = palloc(maxcols * sizeof(ColRef));
  pairs = palloc(2 * (list_length(q->rtable) * 8 + 64) * sizeof(int));

  /* Seed a column node for every element column of every atom (so even
   * un-joined columns become singleton variables). */
  for (a = 0; a < natoms; a++) {
    RangeTblEntry *rte = rt_fetch(atom_rti[a], q->rtable);
    AttrNumber attno = 0;
    ListCell *c;
    foreach (c, rte->eref->colnames) {
      char *name = strVal(lfirst(c));
      attno++;
      if (name[0] == '\0' || strcmp(name, PROVSQL_COLUMN_NAME) == 0)
        continue;
      (void) colref_get(cols, &ncols, atom_rti[a], attno);
    }
  }

  /* Equalities -> union-find; decline on any other qual shape. */
  if (!collect_equalities(q->jointree->quals, cols, &ncols, &pairs, &npairs))
    return NULL;
  for (i = 0; i < npairs; i += 2)
    uf_union(cols, pairs[i], pairs[i + 1]);

  /* Assign a query-variable index to every equivalence class (by class
   * root, first-seen order). */
  {
    int *root_var = palloc(ncols * sizeof(int));
    int nvars = 0;
    for (i = 0; i < ncols; i++)
      root_var[i] = -1;
    for (i = 0; i < ncols; i++) {
      int r = uf_find(cols, i);
      if (root_var[r] < 0)
        root_var[r] = nvars++;
    }

    /* Emit the descriptor JSON. */
    initStringInfo(&buf);
    appendStringInfo(&buf, "{\"disjuncts\":[{\"n_vars\":%d,\"atoms\":[", nvars);
    for (a = 0; a < natoms; a++) {
      RangeTblEntry *rte = rt_fetch(atom_rti[a], q->rtable);
      AttrNumber attno = 0;
      ListCell *c;
      bool firstv = true;
      if (a) appendStringInfoChar(&buf, ',');
      appendStringInfo(&buf, "{\"rel\":%d,\"vars\":[", atom_relidx[a]);
      foreach (c, rte->eref->colnames) {
        char *name = strVal(lfirst(c));
        attno++;
        if (name[0] == '\0' || strcmp(name, PROVSQL_COLUMN_NAME) == 0)
          continue;
        {
          int node = colref_get(cols, &ncols, atom_rti[a], attno);
          int var = root_var[uf_find(cols, node)];
          if (!firstv) appendStringInfoChar(&buf, ',');
          appendStringInfo(&buf, "%d", var);
          firstv = false;
        }
      }
      appendStringInfoString(&buf, "]}");
    }
    appendStringInfoString(&buf, "]}],\"relations\":[");
    for (i = 0; i < nrel; i++) {
      char *nsp = get_namespace_name(get_rel_namespace(rel_oids[i]));
      char *rel = get_rel_name(rel_oids[i]);
      StringInfoData qn;
      initStringInfo(&qn);
      appendStringInfo(&qn, "%s.%s", quote_identifier(nsp),
                       quote_identifier(rel));
      if (i) appendStringInfoChar(&buf, ',');
      append_json_string(&buf, qn.data);   /* gather uses it raw (FROM %s) */
      pfree(qn.data);
    }
    appendStringInfoString(&buf, "],\"elem_cols\":[");
    for (i = 0; i < nrel; i++) {
      /* Element columns of relation i: take them from the first atom over
       * it (all atoms over the same relation share its columns). */
      RangeTblEntry *rte = NULL;
      AttrNumber attno = 0;
      ListCell *c;
      bool firstc = true;
      for (a = 0; a < natoms; a++)
        if (atom_relidx[a] == i) { rte = rt_fetch(atom_rti[a], q->rtable); break; }
      if (i) appendStringInfoChar(&buf, ',');
      appendStringInfoChar(&buf, '[');
      foreach (c, rte->eref->colnames) {
        char *name = strVal(lfirst(c));
        attno++;
        if (name[0] == '\0' || strcmp(name, PROVSQL_COLUMN_NAME) == 0)
          continue;
        if (!firstc) appendStringInfoChar(&buf, ',');
        append_json_string(&buf, name);   /* raw name; gather quotes with %I */
        firstc = false;
      }
      appendStringInfoChar(&buf, ']');
    }
    appendStringInfoString(&buf, "]}");

    /* Per-answer heads: every exposed (non-resjunk) target column must be
     * a bare int4 Var over a tracked atom; collect their query-variable
     * indices and the exposing Vars (the per-group head values).  Any
     * other exposed tracked column makes the head set unextractable, so
     * the per-answer substitution declines (head list left empty). */
    if (head_var_idx != NULL) {
      bool clean = true;
      *head_var_idx = NIL;
      if (head_exprs != NULL)
        *head_exprs = NIL;
      foreach (lc, q->targetList) {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        Node *e;
        ExistCtx ec;
        if (te->resjunk)
          continue;
        e = (Node *) te->expr;
        while (IsA(e, RelabelType))
          e = (Node *) ((RelabelType *) e)->arg;
        ec.rtis = atom_rti; ec.natoms = natoms; ec.found = false;
        exist_walker(e, &ec);
        if (!ec.found)
          continue;   /* a constant / untracked column: not a head */
        if (IsA(e, Var) && ((Var *) e)->varlevelsup == 0 &&
            ((Var *) e)->vartype == INT4OID) {
          Var *vv = (Var *) e;
          int node = colref_get(cols, &ncols, vv->varno, vv->varattno);
          int hv = root_var[uf_find(cols, node)];
          if (!list_member_int(*head_var_idx, hv)) {
            *head_var_idx = lappend_int(*head_var_idx, hv);
            if (head_exprs != NULL)
              *head_exprs = lappend(*head_exprs, e);
          }
          continue;
        }
        clean = false;   /* an exposed tracked column we cannot pin */
        break;
      }
      if (!clean) {
        *head_var_idx = NIL;
        if (head_exprs != NULL)
          *head_exprs = NIL;
      }
    }
  }

  /* The query computes the Boolean existence of the UCQ iff no tracked
   * variable is exposed in the (non-resjunk) target list. */
  if (all_existential) {
    ExistCtx ec;
    ec.rtis = atom_rti;
    ec.natoms = natoms;
    ec.found = false;
    foreach (lc, q->targetList) {
      TargetEntry *te = (TargetEntry *) lfirst(lc);
      if (te->resjunk)
        continue;
      exist_walker((Node *) te->expr, &ec);
    }
    *all_existential = !ec.found;
  }

  (void) constants;
  return buf.data;
}
