/**
 * @file safe_query_cert.c
 * @brief Serialise / parse for the inversion-free tractability certificate.
 *
 * The recipe travels from the planner (which builds it in @c safe_query.c) to
 * the evaluator (@c probability_evaluate.cpp) as the @c extra string of the
 * annotation gate wrapping the provenance root.  The wire format is a compact,
 * space-separated list of integers, prefixed by @c SAFE_CERT_EXTRA_PREFIX_RECIPE
 * ('C') so a consumer can tell a recipe apart from a per-input order key ('K'):
 *
 *   C kind nclasses root_class natoms maxarity
 *     <class_topo_order[0..nclasses-1]>
 *     <atom_relation_rank[0..natoms-1]>
 *     <atom_col_class[0..natoms*maxarity-1]>
 *
 * A per-input order key uses the sibling @c SAFE_CERT_EXTRA_PREFIX_KEY ('K')
 * form @c "K<root> <sec> <factor>" (see @c safe_cert_key_serialise).
 *
 * All functions are pure (no SPI / no catalog access) so the parsers are safe
 * to call from the C++ evaluation side.
 */
#include "postgres.h"
#include "lib/stringinfo.h"
#include "utils/palloc.h"

#include "safe_query_cert.h"

#include <stdlib.h>

char *safe_cert_serialise(const SafeCert *cert)
{
  StringInfoData s;
  int i, ncol;

  if (cert == NULL)
    return NULL;

  initStringInfo(&s);
  appendStringInfoChar(&s, SAFE_CERT_EXTRA_PREFIX_RECIPE);
  appendStringInfo(&s, "%d %d %d %d %d",
                   (int) cert->kind, cert->nclasses, cert->root_class,
                   cert->natoms, cert->maxarity);
  for (i = 0; i < cert->nclasses; i++)
    appendStringInfo(&s, " %d", cert->class_topo_order[i]);
  for (i = 0; i < cert->natoms; i++)
    appendStringInfo(&s, " %d", cert->atom_relation_rank[i]);
  ncol = cert->natoms * cert->maxarity;
  for (i = 0; i < ncol; i++)
    appendStringInfo(&s, " %d", cert->atom_col_class[i]);
  return s.data;
}

SafeCert *safe_cert_parse(const char *str)
{
  const char *p;
  char *end;
  SafeCert *cert;
  int i, ncol;
  long v;

  if (str == NULL || str[0] != SAFE_CERT_EXTRA_PREFIX_RECIPE)
    return NULL;

  p = str + 1;
  cert = (SafeCert *) palloc0(sizeof(SafeCert));

  /* helper: read the next integer, bailing (return NULL) on malformed input */
#define READ_INT(dst)                                       \
  do {                                                      \
    v = strtol(p, &end, 10);                                \
    if (end == p) { pfree(cert); return NULL; }             \
    (dst) = (int) v;                                        \
    p = end;                                                \
  } while (0)

  {
    int kind;
    READ_INT(kind);
    cert->kind = (SafeCertKind) kind;
    READ_INT(cert->nclasses);
    READ_INT(cert->root_class);
    READ_INT(cert->natoms);
    READ_INT(cert->maxarity);
  }

  if (cert->nclasses < 0 || cert->natoms < 0 || cert->maxarity < 0) {
    pfree(cert);
    return NULL;
  }

  cert->class_topo_order = (int *) palloc(sizeof(int) * (cert->nclasses > 0 ? cert->nclasses : 1));
  for (i = 0; i < cert->nclasses; i++)
    READ_INT(cert->class_topo_order[i]);

  cert->atom_relation_rank = (int *) palloc(sizeof(int) * (cert->natoms > 0 ? cert->natoms : 1));
  for (i = 0; i < cert->natoms; i++)
    READ_INT(cert->atom_relation_rank[i]);

  ncol = cert->natoms * cert->maxarity;
  cert->atom_col_class = (int *) palloc(sizeof(int) * (ncol > 0 ? ncol : 1));
  for (i = 0; i < ncol; i++)
    READ_INT(cert->atom_col_class[i]);

#undef READ_INT

  return cert;
}

char *safe_cert_key_serialise(long root, long sec, int factor)
{
  StringInfoData s;
  initStringInfo(&s);
  appendStringInfoChar(&s, SAFE_CERT_EXTRA_PREFIX_KEY);
  appendStringInfo(&s, "%ld %ld %d", root, sec, factor);
  return s.data;
}

bool safe_cert_key_parse(const char *str, SafeCertKey *out)
{
  const char *p;
  char *end;
  SafeCertKey k;

  if (str == NULL || str[0] != SAFE_CERT_EXTRA_PREFIX_KEY)
    return false;

  p = str + 1;
  k.root = strtol(p, &end, 10);
  if (end == p) return false;
  p = end;
  k.sec = strtol(p, &end, 10);
  if (end == p) return false;
  p = end;
  k.factor = (int) strtol(p, &end, 10);
  if (end == p) return false;

  if (out != NULL)
    *out = k;
  return true;
}
