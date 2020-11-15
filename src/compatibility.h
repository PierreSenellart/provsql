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
