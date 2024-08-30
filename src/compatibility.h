#ifndef COMPATIBILITY_H
#define COMPATIBILITY_H

#include "postgres.h"
#include "nodes/pg_list.h"

static inline List *my_list_delete_cell(List *list, ListCell *cell, ListCell *prev) {
#if PG_VERSION_NUM >= 130000
  return list_delete_cell(list, cell);
#else
  return list_delete_cell(list, cell, prev);
#endif
}

static inline ListCell *my_lnext(const List *l, const ListCell *c)
{
#if PG_VERSION_NUM >= 130000
  return lnext(l, c);
#else
  return lnext(c);
#endif
}

#if PG_VERSION_NUM < 130000
// Before PostgreSQL 13, lists were implemented as linked lists; after
// that, they became arrays. The insertion functions were added to
// PostgreSQL 13.
List *list_insert_nth(List *list, int pos, void *datum);
#endif

#if PG_VERSION_NUM < 140000
// Macros for the OID of predefined functions were not defined until
// PostgreSQL 14. The OIDs have remained the same throughout, though.
#define F_COUNT_ANY 2147
#define F_COUNT_ 2803
#endif

#endif /* COMPATIBILITY_H */
