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

/**
 * @brief Collect the equality structure of a WHERE / ON qual tree.
 *
 * A conjunction of equalities (the qual must be @c AND-only).  @c Var=Var
 * is a join: its two columns are unioned (recorded in @p pairs).
 * @c Var=Const is a selection: when @p const_cols is non-NULL the column
 * and the constant are recorded (in @p const_cols / @p const_vals) so the
 * caller can pin the column's variable to the value; when @p const_cols is
 * @c NULL such a selection makes the recogniser decline (callers that
 * cannot apply a pin -- the UNION arms -- pass @c NULL).  Any other qual
 * shape declines.  The equality must be the column type's default @c =.
 */
static bool collect_equalities(Node *qual, ColRef *cols, int *ncols,
                               int **pairs, int *npairs,
                               int *const_cols, Const **const_vals, int *nconst)
{
  if (qual == NULL)
    return true;
  if (IsA(qual, BoolExpr)) {
    BoolExpr *b = (BoolExpr *) qual;
    ListCell *lc;
    if (b->boolop != AND_EXPR)
      return false;
    foreach (lc, b->args)
      if (!collect_equalities((Node *) lfirst(lc), cols, ncols, pairs, npairs,
                              const_cols, const_vals, nconst))
        return false;
    return true;
  }
  if (IsA(qual, OpExpr)) {
    OpExpr *op = (OpExpr *) qual;
    Node *ln, *rn;
    Var  *v;
    Const *c;
    Oid   eq;
    if (list_length(op->args) != 2)
      return false;
    ln = (Node *) linitial(op->args);
    rn = (Node *) lsecond(op->args);
    while (IsA(ln, RelabelType))
      ln = (Node *) ((RelabelType *) ln)->arg;
    while (IsA(rn, RelabelType))
      rn = (Node *) ((RelabelType *) rn)->arg;
    /* Var = Var: a join. */
    if (IsA(ln, Var) && IsA(rn, Var)) {
      Var *lv = (Var *) ln, *rv = (Var *) rn;
      if (lv->varlevelsup != 0 || rv->varlevelsup != 0)
        return false;
      eq = lookup_type_cache(lv->vartype, TYPECACHE_EQ_OPR)->eq_opr;
      if (eq == InvalidOid || op->opno != eq)
        return false;
      (*pairs)[(*npairs)++] = colref_get(cols, ncols, lv->varno, lv->varattno);
      (*pairs)[(*npairs)++] = colref_get(cols, ncols, rv->varno, rv->varattno);
      return true;
    }
    /* Var = Const (either order): a selection pinning the column. */
    if ((IsA(ln, Var) && IsA(rn, Const)) || (IsA(rn, Var) && IsA(ln, Const))) {
      if (const_cols == NULL)
        return false;   /* this caller cannot apply a constant pin */
      if (IsA(ln, Var)) { v = (Var *) ln; c = (Const *) rn; }
      else              { v = (Var *) rn; c = (Const *) ln; }
      if (v->varlevelsup != 0 || c->constisnull || c->consttype != v->vartype)
        return false;   /* NULL or cross-type: not a plain value pin */
      eq = lookup_type_cache(v->vartype, TYPECACHE_EQ_OPR)->eq_opr;
      if (eq == InvalidOid || op->opno != eq)
        return false;
      const_cols[*nconst] = colref_get(cols, ncols, v->varno, v->varattno);
      const_vals[*nconst] = c;
      (*nconst)++;
      return true;
    }
    return false;
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

/**
 * @brief Recursively collect tracked base-relation atoms and the ON quals
 *        of a jointree item (a @c RangeTblRef or an inner @c JoinExpr).
 *
 * Flattens @c a @c JOIN @c b @c ON ... (and nested inner joins) to the
 * same atom set + equality quals as the comma/@c WHERE form.  Declines
 * (returns @c false) on an outer join, a non-relation RTE, an untracked
 * relation, or any other item shape.
 */
static bool collect_jointree(Node *n, Query *q, Index *atom_rti,
                             Oid *atom_relid, int *natoms, List **qual_list)
{
  if (n == NULL)
    return true;
  if (IsA(n, RangeTblRef)) {
    Index rti = ((RangeTblRef *) n)->rtindex;
    RangeTblEntry *rte = rt_fetch(rti, q->rtable);
    if (rte->rtekind != RTE_RELATION)
      return false;
    if (get_attnum(rte->relid, PROVSQL_COLUMN_NAME) == InvalidAttrNumber)
      return false;
    atom_rti[*natoms] = rti;
    atom_relid[*natoms] = rte->relid;
    (*natoms)++;
    return true;
  }
  if (IsA(n, JoinExpr)) {
    JoinExpr *je = (JoinExpr *) n;
    if (je->jointype != JOIN_INNER)
      return false;   /* outer joins change the semantics: decline */
    if (!collect_jointree(je->larg, q, atom_rti, atom_relid, natoms, qual_list))
      return false;
    if (!collect_jointree(je->rarg, q, atom_rti, atom_relid, natoms, qual_list))
      return false;
    if (je->quals != NULL)
      *qual_list = lappend(*qual_list, je->quals);   /* the ON condition */
    return true;
  }
  return false;   /* FromExpr / subquery / other: outside scope */
}

/**
 * @brief A merge of relations across the disjuncts of a UCQ: distinct
 *        relation OIDs (first-seen order) with their element column-name
 *        lists.  The atoms of every disjunct reference relations by their
 *        index here, so a relation shared between disjuncts (a self-join
 *        across a UNION) is gathered once.
 */
typedef struct {
  Oid   *oids;
  List **colnames;   /* element column names (raw), per relation */
  int    n;
  int    cap;
} RelMerge;

/** @brief Find-or-add a relation; returns its global index. */
static int relmerge_get(RelMerge *m, Oid oid, List *colnames)
{
  int i;
  for (i = 0; i < m->n; i++)
    if (m->oids[i] == oid)
      return i;
  m->oids[m->n] = oid;
  m->colnames[m->n] = colnames;
  return m->n++;
}

/**
 * @brief Does @p cq expose no tracked-atom variable in its target list
 *        (i.e. it computes the Boolean existence of its CQ)?
 *
 * @p atom_rti / @p natoms describe the tracked range-table entries of
 * @p cq (as collected by @c collect_jointree).
 */
static bool cq_is_existential(Query *cq, Index *atom_rti, int natoms)
{
  ListCell *lc;
  ExistCtx  ec;
  ec.rtis = atom_rti;
  ec.natoms = natoms;
  ec.found = false;
  foreach (lc, cq->targetList) {
    TargetEntry *te = (TargetEntry *) lfirst(lc);
    if (te->resjunk)
      continue;
    if (te->resname != NULL && strcmp(te->resname, PROVSQL_COLUMN_NAME) == 0)
      continue;   /* the auto-added provenance column of a processed arm */
    exist_walker((Node *) te->expr, &ec);
  }
  return !ec.found;
}

/**
 * @brief Extract one conjunctive query (a flat join over tracked base
 *        relations) as a UCQ disjunct, appending its
 *        @c {"n_vars":N,"atoms":[...]} object to @p dbuf and registering
 *        its relations in @p m (so atom @c "rel" indices are global).
 *
 * The per-disjunct variable space is local (variables 0..N-1 of this
 * disjunct); the element dictionary that binds them to data values is
 * shared across the whole UCQ by the gather.
 *
 * @p head_resno (length @p n_heads) gives the arm output-column positions
 * (1-based @c resno, aligned across the UNION's arms) of the per-answer
 * head variables, in head order.  Each is numbered CANONICALLY -- head
 * @e h becomes query variable @e h in every arm -- so a single
 * @c head_vars = [0,1,…] pins the head in every disjunct (the SQL
 * @c ucq_joint_provenance_answer uses one head-variable index across all
 * disjuncts).  With @p n_heads == 0 the arm must be Boolean-existential.
 * Returns @c false to decline.
 */
static bool emit_cq_disjunct(const constants_t *constants, Query *cq,
                             RelMerge *m, StringInfo dbuf,
                             const int *head_resno, int n_heads)
{
  ListCell *lc;
  int       natoms = 0;
  Index    *atom_rti;
  Oid      *atom_relid;
  ColRef   *cols;
  int       ncols = 0;
  int      *pairs;
  int       npairs = 0;
  int       maxcols;
  List     *qual_list = NIL;
  int      *root_var;
  int       nvars = 0;
  int       a, i, h;

  (void) constants;

  /* Same shape gate as the top-level single-CQ path, per arm. */
  if (cq->commandType != CMD_SELECT || cq->setOperations != NULL ||
      cq->jointree == NULL || cq->hasAggs || cq->havingQual != NULL ||
      cq->hasWindowFuncs || cq->hasSubLinks || cq->cteList != NIL ||
      cq->jointree->fromlist == NIL)
    return false;

  atom_rti   = palloc(list_length(cq->rtable) * sizeof(Index));
  atom_relid = palloc(list_length(cq->rtable) * sizeof(Oid));
  foreach (lc, cq->jointree->fromlist)
    if (!collect_jointree((Node *) lfirst(lc), cq, atom_rti, atom_relid,
                          &natoms, &qual_list))
      return false;
  if (natoms == 0)
    return false;
  if (cq->jointree->quals != NULL)
    qual_list = lappend(qual_list, cq->jointree->quals);

  /* A Boolean disjunct must expose no tracked head; a per-answer disjunct
   * exposes exactly the head columns (resolved below). */
  if (n_heads == 0 && !cq_is_existential(cq, atom_rti, natoms))
    return false;

  maxcols = 0;
  for (a = 0; a < natoms; a++) {
    RangeTblEntry *rte = rt_fetch(atom_rti[a], cq->rtable);
    maxcols += list_length(rte->eref->colnames);
  }
  cols = palloc(maxcols * sizeof(ColRef));
  pairs = palloc(2 * (list_length(cq->rtable) * 8 + 64) * sizeof(int));

  for (a = 0; a < natoms; a++) {
    RangeTblEntry *rte = rt_fetch(atom_rti[a], cq->rtable);
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

  foreach (lc, qual_list)
    if (!collect_equalities((Node *) lfirst(lc), cols, &ncols, &pairs, &npairs,
                            NULL, NULL, NULL))   /* a UNION arm cannot pin a const */
      return false;
  for (i = 0; i < npairs; i += 2)
    uf_union(cols, pairs[i], pairs[i + 1]);

  root_var = palloc(ncols * sizeof(int));
  for (i = 0; i < ncols; i++)
    root_var[i] = -1;

  /* Canonical head numbering: head h -> variable h.  Resolve each head
   * output column to its base Var, then its equivalence class. */
  for (h = 0; h < n_heads; h++) {
    TargetEntry *te = NULL;
    ListCell    *tc;
    Node        *e;
    Var         *hv;
    bool         tracked = false;
    int          node, root;
    foreach (tc, cq->targetList) {
      TargetEntry *t = (TargetEntry *) lfirst(tc);
      if (t->resno == head_resno[h]) { te = t; break; }
    }
    if (te == NULL || te->resjunk)
      return false;
    e = (Node *) te->expr;
    while (IsA(e, RelabelType))
      e = (Node *) ((RelabelType *) e)->arg;
    if (!IsA(e, Var) || ((Var *) e)->varlevelsup != 0)
      return false;   /* a head must be a bare base column to be pinnable */
    hv = (Var *) e;
    for (a = 0; a < natoms; a++)
      if (atom_rti[a] == hv->varno) { tracked = true; break; }
    if (!tracked)
      return false;
    node = colref_get(cols, &ncols, hv->varno, hv->varattno);
    root = uf_find(cols, node);
    if (root_var[root] >= 0 && root_var[root] != h)
      return false;   /* two heads collapse to one variable: decline */
    root_var[root] = h;
  }
  nvars = n_heads;
  for (i = 0; i < ncols; i++) {
    int r = uf_find(cols, i);
    if (root_var[r] < 0)
      root_var[r] = nvars++;
  }

  appendStringInfo(dbuf, "{\"n_vars\":%d,\"atoms\":[", nvars);
  for (a = 0; a < natoms; a++) {
    RangeTblEntry *rte = rt_fetch(atom_rti[a], cq->rtable);
    AttrNumber attno = 0;
    ListCell *c;
    bool firstv = true;
    List *elem_colnames = NIL;
    int   gidx;
    /* Element column names of this atom's relation (raw, in order). */
    foreach (c, rte->eref->colnames) {
      char *name = strVal(lfirst(c));
      if (name[0] == '\0' || strcmp(name, PROVSQL_COLUMN_NAME) == 0)
        continue;
      elem_colnames = lappend(elem_colnames, makeString(name));
    }
    gidx = relmerge_get(m, atom_relid[a], elem_colnames);
    if (a) appendStringInfoChar(dbuf, ',');
    appendStringInfo(dbuf, "{\"rel\":%d,\"vars\":[", gidx);
    attno = 0;
    foreach (c, rte->eref->colnames) {
      char *name = strVal(lfirst(c));
      attno++;
      if (name[0] == '\0' || strcmp(name, PROVSQL_COLUMN_NAME) == 0)
        continue;
      {
        int node = colref_get(cols, &ncols, atom_rti[a], attno);
        int var = root_var[uf_find(cols, node)];
        if (!firstv) appendStringInfoChar(dbuf, ',');
        appendStringInfo(dbuf, "%d", var);
        firstv = false;
      }
    }
    appendStringInfoString(dbuf, "]}");
  }
  appendStringInfoString(dbuf, "]}");
  return true;
}

/**
 * @brief Collect the leaf arm subqueries of a UNION set-operation tree.
 *
 * Walks @p node (a @c SetOperationStmt tree); every internal node must be
 * a @c SETOP_UNION (UNION or UNION ALL -- both compute the same Boolean
 * existence), and every leaf a @c RangeTblRef to an @c RTE_SUBQUERY arm.
 * Appends each arm @c Query* to @p arms.  Returns @c false to decline
 * (INTERSECT / EXCEPT, or an unexpected leaf).
 */
static bool collect_union_arms(Node *node, Query *parent, List **arms)
{
  if (node == NULL)
    return false;
  if (IsA(node, SetOperationStmt)) {
    SetOperationStmt *so = (SetOperationStmt *) node;
    if (so->op != SETOP_UNION)
      return false;
    return collect_union_arms(so->larg, parent, arms) &&
           collect_union_arms(so->rarg, parent, arms);
  }
  if (IsA(node, RangeTblRef)) {
    Index rti = ((RangeTblRef *) node)->rtindex;
    RangeTblEntry *rte = rt_fetch(rti, parent->rtable);
    if (rte->rtekind != RTE_SUBQUERY || rte->subquery == NULL)
      return false;
    *arms = lappend(*arms, rte->subquery);
    return true;
  }
  return false;
}

/**
 * @brief Classify a UNION output column (1-based @p resno): is it a
 *        per-answer head or a constant projection?
 *
 * @return @c 1 if every arm exposes a bare @c Var over a tracked base
 *         relation there (a real head, pinnable), @c 0 if every arm
 *         exposes a constant / non-tracked expression there (a Boolean
 *         projection like @c SELECT @c 1, to be ignored), @c -1 on a mixed
 *         or malformed column (the recogniser then declines).
 */
static int union_position_kind(Query *unionq, int resno)
{
  List     *arms = NIL;
  ListCell *lc;
  int       kind = -2;   /* unset */

  if (!collect_union_arms(unionq->setOperations, unionq, &arms))
    return -1;
  foreach (lc, arms) {
    Query       *arm = (Query *) lfirst(lc);
    TargetEntry *te = NULL;
    ListCell    *tc;
    Node        *e;
    int          k;
    foreach (tc, arm->targetList) {
      TargetEntry *t = (TargetEntry *) lfirst(tc);
      if (t->resno == resno) { te = t; break; }
    }
    if (te == NULL)
      return -1;
    e = (Node *) te->expr;
    while (IsA(e, RelabelType))
      e = (Node *) ((RelabelType *) e)->arg;
    k = 0;
    if (IsA(e, Var) && ((Var *) e)->varlevelsup == 0) {
      RangeTblEntry *rte = rt_fetch(((Var *) e)->varno, arm->rtable);
      if (rte->rtekind == RTE_RELATION &&
          get_attnum(rte->relid, PROVSQL_COLUMN_NAME) != InvalidAttrNumber)
        k = 1;
    }
    if (kind == -2)
      kind = k;
    else if (kind != k)
      return -1;   /* tracked in one arm, constant in another: decline */
  }
  return kind < 0 ? 0 : kind;
}

/**
 * @brief Build the descriptor for a UNION of conjunctive queries (a UCQ
 *        with more than one disjunct): one disjunct per arm, relations
 *        merged across arms.  @p head_resno (length @p n_heads) gives the
 *        per-answer head output positions (canonically numbered in every
 *        arm); @p n_heads == 0 is the Boolean existence case.  Returns
 *        @c NULL to decline.
 */
static char *build_union_descriptor(const constants_t *constants, Query *unionq,
                                    const int *head_resno, int n_heads)
{
  List          *arms = NIL;
  ListCell      *lc;
  RelMerge       m;
  StringInfoData dbuf;
  StringInfoData buf;
  int            i;
  bool           first = true;

  if (!collect_union_arms(unionq->setOperations, unionq, &arms))
    return NULL;
  if (list_length(arms) < 2)
    return NULL;

  m.cap = 0;
  foreach (lc, arms)
    m.cap += list_length(((Query *) lfirst(lc))->rtable);
  m.oids     = palloc(m.cap * sizeof(Oid));
  m.colnames = palloc(m.cap * sizeof(List *));
  m.n = 0;

  initStringInfo(&dbuf);
  foreach (lc, arms) {
    if (!first) appendStringInfoChar(&dbuf, ',');
    if (!emit_cq_disjunct(constants, (Query *) lfirst(lc), &m, &dbuf,
                          head_resno, n_heads))
      return NULL;
    first = false;
  }

  initStringInfo(&buf);
  appendStringInfo(&buf, "{\"disjuncts\":[%s],\"relations\":[", dbuf.data);
  for (i = 0; i < m.n; i++) {
    char *nsp = get_namespace_name(get_rel_namespace(m.oids[i]));
    char *rel = get_rel_name(m.oids[i]);
    StringInfoData qn;
    initStringInfo(&qn);
    appendStringInfo(&qn, "%s.%s", quote_identifier(nsp), quote_identifier(rel));
    if (i) appendStringInfoChar(&buf, ',');
    append_json_string(&buf, qn.data);
    pfree(qn.data);
  }
  appendStringInfoString(&buf, "],\"elem_cols\":[");
  for (i = 0; i < m.n; i++) {
    ListCell *c;
    bool firstc = true;
    if (i) appendStringInfoChar(&buf, ',');
    appendStringInfoChar(&buf, '[');
    foreach (c, m.colnames[i]) {
      if (!firstc) appendStringInfoChar(&buf, ',');
      append_json_string(&buf, strVal(lfirst(c)));
      firstc = false;
    }
    appendStringInfoChar(&buf, ']');
  }
  appendStringInfoString(&buf, "]}");
  return buf.data;
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
  int       *const_cols;       /* per const pin: colref index */
  Const    **const_vals;       /* per const pin: the literal */
  int        nconst = 0;
  int        maxcols;
  List      *qual_list = NIL;
  StringInfoData buf;
  int        i, a;

  if (all_existential)
    *all_existential = false;
  if (head_var_idx)
    *head_var_idx = NIL;
  if (head_exprs)
    *head_exprs = NIL;

  /* UNION of conjunctive queries: a genuine multi-disjunct UCQ.  Reached
   * when this is the body of an aggregated subquery (the recursion from the
   * subquery branch below); a top-level UNION is combined at SR_PLUS, where
   * the recogniser is not invoked.  Boolean existence only (per-answer
   * UNION heads decline inside emit_cq_disjunct). */
  if (q->setOperations != NULL) {
    char *desc = build_union_descriptor(constants, q, NULL, 0);
    if (desc != NULL && all_existential)
      *all_existential = true;
    return desc;
  }

  /* Shape gate: a plain SELECT, single CQ (no aggregates), with a jointree.
   * DISTINCT / GROUP BY are allowed (they are how the existence / per-answer
   * questions are written). */
  if (q->commandType != CMD_SELECT ||
      q->jointree == NULL || q->hasAggs ||
      q->havingQual != NULL ||
      q->hasWindowFuncs || q->hasSubLinks || q->cteList != NIL)
    return NULL;

  /* Subquery form: the sole FROM item is a subquery whose body is the UCQ
   * (SELECT head, probability_evaluate(provenance())
   *    FROM (SELECT base.head FROM ... WHERE ...) s GROUP BY head, and the
   * DISTINCT variant).  Recurse into the subquery for the descriptor and
   * its output heads, then map the outer output columns through the
   * subquery target list (the outer Var becomes the per-group head value). */
  if (list_length(q->jointree->fromlist) == 1 && q->jointree->quals == NULL) {
    Node *only = (Node *) linitial(q->jointree->fromlist);
    if (IsA(only, RangeTblRef)) {
      Index subrti = ((RangeTblRef *) only)->rtindex;
      RangeTblEntry *subrte = rt_fetch(subrti, q->rtable);
      if (subrte->rtekind == RTE_SUBQUERY &&
          subrte->subquery->setOperations != NULL) {
        /* UNION subquery: a multi-disjunct UCQ.  The per-answer heads are the
         * GROUP BY keys (Vars over the subquery); the SELECT list is
         * arbitrary, and the provenance() aggregate is not a head.  With no
         * GROUP BY it is the Boolean existence.  A constant union column
         * (SELECT 1) is ignored. */
        int      *head_resno = palloc((list_length(q->targetList) +
                                       list_length(q->groupClause)) * sizeof(int));
        List     *hexprs = NIL;
        int       n_heads = 0;
        char     *udesc;
        List     *cand = NIL;   /* candidate head target-entries */
        if (q->groupClause != NIL) {
          ListCell *gl, *tc;
          foreach (gl, q->groupClause) {
            SortGroupClause *sgc = (SortGroupClause *) lfirst(gl);
            foreach (tc, q->targetList) {
              TargetEntry *t = (TargetEntry *) lfirst(tc);
              if (t->ressortgroupref == sgc->tleSortGroupRef) {
                cand = lappend(cand, t); break;
              }
            }
          }
        } else {
          foreach (lc, q->targetList) {
            TargetEntry *t = (TargetEntry *) lfirst(lc);
            if (!t->resjunk)
              cand = lappend(cand, t);
          }
        }
        foreach (lc, cand) {
          TargetEntry *te = (TargetEntry *) lfirst(lc);
          Node *e;
          Var  *v;
          TargetEntry *ste = NULL;
          ListCell *sc;
          e = (Node *) te->expr;
          while (IsA(e, RelabelType))
            e = (Node *) ((RelabelType *) e)->arg;
          if (!(IsA(e, Var) && ((Var *) e)->varno == subrti &&
                ((Var *) e)->varlevelsup == 0)) {
            if (q->groupClause != NIL)
              return NULL;   /* an un-pinnable group key: decline */
            continue;        /* the provenance() aggregate or a constant */
          }
          v = (Var *) e;
          foreach (sc, subrte->subquery->targetList) {
            TargetEntry *t = (TargetEntry *) lfirst(sc);
            if (t->resno == v->varattno) { ste = t; break; }
          }
          if (ste != NULL && ste->resname != NULL &&
              strcmp(ste->resname, PROVSQL_COLUMN_NAME) == 0)
            continue;   /* the subquery's provenance column, not a head */
          {
            int kind = union_position_kind(subrte->subquery, v->varattno);
            if (kind == -1)
              return NULL;       /* malformed / mixed head column */
            if (kind == 0)
              continue;          /* a constant projection (SELECT 1 d): ignore */
          }
          {
            bool dup = false; int z;
            for (z = 0; z < n_heads; z++)
              if (head_resno[z] == v->varattno) { dup = true; break; }
            if (dup) continue;
          }
          head_resno[n_heads] = v->varattno;
          hexprs = lappend(hexprs, e);
          n_heads++;
        }
        udesc = build_union_descriptor(constants, subrte->subquery,
                                       head_resno, n_heads);
        if (udesc == NULL)
          return NULL;
        if (n_heads == 0) {
          if (all_existential)
            *all_existential = true;
        } else if (head_var_idx != NULL) {
          int h;
          *head_var_idx = NIL;
          for (h = 0; h < n_heads; h++)
            *head_var_idx = lappend_int(*head_var_idx, h);   /* canonical */
          if (head_exprs != NULL)
            *head_exprs = hexprs;
        }
        return udesc;
      }
      if (subrte->rtekind == RTE_SUBQUERY) {
        List *sub_idx = NIL, *sub_exprs = NIL;
        bool  sub_all = false;
        char *desc = provsql_joint_width_descriptor(constants, subrte->subquery,
                                                    &sub_all, &sub_idx,
                                                    &sub_exprs);
        if (desc == NULL || sub_idx == NIL)
          return NULL;
        /* If the inner CQ carries a constant pin (a Const, not a Var, in its
         * head list), it cannot be remapped through the outer target list:
         * decline (the flat form handles constants directly). */
        {
          ListCell *xc;
          foreach (xc, sub_exprs)
            if (!IsA((Node *) lfirst(xc), Var))
              return NULL;
        }
        if (head_var_idx != NULL) {
          bool clean = true;
          *head_var_idx = NIL;
          if (head_exprs != NULL)
            *head_exprs = NIL;
          foreach (lc, q->targetList) {
            TargetEntry *te = (TargetEntry *) lfirst(lc);
            Node *e;
            AttrNumber outcol;
            TargetEntry *ste = NULL;
            ListCell *sc, *li, *lx;
            Node *se;
            int hv = -1;
            if (te->resjunk)
              continue;
            e = (Node *) te->expr;
            while (IsA(e, RelabelType))
              e = (Node *) ((RelabelType *) e)->arg;
            if (!(IsA(e, Var) && ((Var *) e)->varno == subrti &&
                  ((Var *) e)->varlevelsup == 0))
              continue;   /* the aggregate / a constant: not a head */
            outcol = ((Var *) e)->varattno;
            foreach (sc, subrte->subquery->targetList) {
              TargetEntry *t = (TargetEntry *) lfirst(sc);
              if (t->resno == outcol) { ste = t; break; }
            }
            if (ste == NULL || ste->resjunk) { clean = false; break; }
            se = (Node *) ste->expr;
            while (IsA(se, RelabelType))
              se = (Node *) ((RelabelType *) se)->arg;
            if (!IsA(se, Var)) { clean = false; break; }
            forboth (li, sub_idx, lx, sub_exprs) {
              Var *sv = (Var *) lfirst(lx);
              if (sv->varno == ((Var *) se)->varno &&
                  sv->varattno == ((Var *) se)->varattno) {
                hv = lfirst_int(li);
                break;
              }
            }
            if (hv < 0) { clean = false; break; }
            if (!list_member_int(*head_var_idx, hv)) {
              *head_var_idx = lappend_int(*head_var_idx, hv);
              if (head_exprs != NULL)
                *head_exprs = lappend(*head_exprs, e);
            }
          }
          if (!clean) {
            *head_var_idx = NIL;
            if (head_exprs != NULL)
              *head_exprs = NIL;
          }
        }
        if (all_existential)
          *all_existential = false;
        return desc;
      }
    }
  }

  /* The FROM list must be a flat list of RangeTblRefs to tracked base
   * relations. */
  if (q->jointree->fromlist == NIL)
    return NULL;
  atom_rti   = palloc(list_length(q->rtable) * sizeof(Index));
  atom_relid = palloc(list_length(q->rtable) * sizeof(Oid));
  atom_relidx = palloc(list_length(q->rtable) * sizeof(int));
  rel_oids   = palloc(list_length(q->rtable) * sizeof(Oid));
  foreach (lc, q->jointree->fromlist)
    if (!collect_jointree((Node *) lfirst(lc), q, atom_rti, atom_relid,
                          &natoms, &qual_list))
      return NULL;
  if (natoms == 0)
    return NULL;
  /* The top-level WHERE conjoins with every collected ON condition. */
  if (q->jointree->quals != NULL)
    qual_list = lappend(qual_list, q->jointree->quals);

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
  const_cols = palloc((list_length(q->rtable) * 8 + 64) * sizeof(int));
  const_vals = palloc((list_length(q->rtable) * 8 + 64) * sizeof(Const *));

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

  /* Equalities (WHERE + every collected ON) -> union-find; decline on any
   * other qual shape. */
  foreach (lc, qual_list)
    if (!collect_equalities((Node *) lfirst(lc), cols, &ncols, &pairs, &npairs,
                            const_cols, const_vals, &nconst))
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

    /* Heads = the variables the query pins per answer: the GROUP BY keys
     * (each a bare Var over a tracked atom -- the SELECT list itself is
     * arbitrary, a function of the keys, so what is displayed does not
     * constrain recognition) plus the constant selections from WHERE (a
     * column pinned to a literal).  Both bind a variable to a value via the
     * same Sel mechanism; the value is the GROUP BY column per answer, or
     * the literal.  A query with no such pins is the Boolean existence; an
     * un-pinnable group key (an expression) declines.  Without a GROUP BY,
     * the exposed bare tracked Vars are the heads (the historical form). */
    {
    bool   clean = true;
    List  *hv = NIL, *hx = NIL;
    int    k;
    if (q->groupClause != NIL) {
      ListCell *gl;
      foreach (gl, q->groupClause) {
        SortGroupClause *sgc = (SortGroupClause *) lfirst(gl);
        TargetEntry     *gte = NULL;
        ListCell        *tc;
        Node            *e;
        Var             *vv;
        bool             tracked = false;
        int              node, var;
        foreach (tc, q->targetList) {
          TargetEntry *t = (TargetEntry *) lfirst(tc);
          if (t->ressortgroupref == sgc->tleSortGroupRef) { gte = t; break; }
        }
        if (gte == NULL) { clean = false; break; }
        e = (Node *) gte->expr;
        while (IsA(e, RelabelType))
          e = (Node *) ((RelabelType *) e)->arg;
        if (!(IsA(e, Var) && ((Var *) e)->varlevelsup == 0)) {
          clean = false; break;   /* an expression group key: cannot pin */
        }
        vv = (Var *) e;
        for (a = 0; a < natoms; a++)
          if (atom_rti[a] == vv->varno) { tracked = true; break; }
        if (!tracked)
          continue;   /* grouping on an untracked column: not a head */
        node = colref_get(cols, &ncols, vv->varno, vv->varattno);
        var = root_var[uf_find(cols, node)];
        if (!list_member_int(hv, var)) {
          hv = lappend_int(hv, var);
          hx = lappend(hx, e);
        }
      }
    } else {
      foreach (lc, q->targetList) {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        Node *e;
        ExistCtx ec;
        if (te->resjunk)
          continue;
        if (te->resname != NULL &&
            strcmp(te->resname, PROVSQL_COLUMN_NAME) == 0)
          continue;   /* auto-added provenance column of a processed subquery */
        e = (Node *) te->expr;
        while (IsA(e, RelabelType))
          e = (Node *) ((RelabelType *) e)->arg;
        ec.rtis = atom_rti; ec.natoms = natoms; ec.found = false;
        exist_walker(e, &ec);
        if (!ec.found)
          continue;   /* a constant / untracked column: not a head */
        if (IsA(e, Var) && ((Var *) e)->varlevelsup == 0) {
          Var *vv = (Var *) e;
          int node = colref_get(cols, &ncols, vv->varno, vv->varattno);
          int var = root_var[uf_find(cols, node)];
          if (!list_member_int(hv, var)) {
            hv = lappend_int(hv, var);
            hx = lappend(hx, e);
          }
          continue;
        }
        clean = false;   /* an exposed tracked column we cannot pin */
        break;
      }
    }
    /* Constant pins (skip a variable already pinned as a head -- a grouped
     * column that is also constant-selected agrees by construction). */
    if (clean)
      for (k = 0; k < nconst; k++) {
        int var = root_var[uf_find(cols, const_cols[k])];
        if (!list_member_int(hv, var)) {
          hv = lappend_int(hv, var);
          hx = lappend(hx, (Expr *) const_vals[k]);
        }
      }

    if (head_var_idx != NULL) {
      *head_var_idx = clean ? hv : NIL;
      if (head_exprs != NULL)
        *head_exprs = clean ? hx : NIL;
    }
    /* Boolean existence iff there is no pin at all (and recognition is
     * clean); otherwise the per-answer / constant-pinned substitution. */
    if (all_existential)
      *all_existential = clean && (hv == NIL);
    }
  }

  (void) constants;
  return buf.data;
}
