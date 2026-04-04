/**
 * @file compatibility.h
 * @brief PostgreSQL cross-version compatibility shims for ProvSQL.
 *
 * ProvSQL supports a range of PostgreSQL major versions.  This header
 * centralises the small API differences between those versions so that
 * the rest of the codebase can call a single, stable interface.
 *
 * Currently handled differences:
 * - **List API (13+)**: @c list_delete_cell() and @c lnext() gained or
 *   lost a @p prev argument between PostgreSQL 12 and 13.  The
 *   @c my_list_delete_cell() and @c my_lnext() wrappers hide this.
 * - **list_insert_nth() (< 13)**: In PostgreSQL 12 and earlier the
 *   list implementation was a linked list and this helper did not exist;
 *   @c compatibility.c provides a backport.
 * - **Predefined function OIDs (< 14)**: The @c F_COUNT_ANY,
 *   @c F_COUNT_, and @c F_SUM_INT4 macros were introduced in PostgreSQL
 *   14.  Stable OID values for older releases are defined here.
 */
#ifndef COMPATIBILITY_H
#define COMPATIBILITY_H

#include "postgres.h"
#include "nodes/pg_list.h"

/**
 * @brief Version-agnostic wrapper around @c list_delete_cell().
 *
 * PostgreSQL 13 changed @c list_delete_cell() to no longer require the
 * previous cell pointer (because lists became arrays).  This inline
 * helper selects the correct call form at compile time.
 *
 * @param list  The list to modify.
 * @param cell  The cell to delete.
 * @param prev  The cell immediately before @p cell (ignored on PG ≥ 13).
 * @return      The modified list.
 */
static inline List *my_list_delete_cell(List *list, ListCell *cell, ListCell *prev) {
#if PG_VERSION_NUM >= 130000
  return list_delete_cell(list, cell);
#else
  return list_delete_cell(list, cell, prev);
#endif
}

/**
 * @brief Version-agnostic wrapper around @c lnext().
 *
 * PostgreSQL 13 added the list pointer parameter to @c lnext() to
 * support the array-based list implementation.  This inline helper
 * selects the correct call form at compile time.
 *
 * @param l  The list (ignored on PG < 13).
 * @param c  The current cell.
 * @return   The next cell, or @c NULL if @p c is the last element.
 */
static inline ListCell *my_lnext(const List *l, const ListCell *c)
{
#if PG_VERSION_NUM >= 130000
  return lnext(l, c);
#else
  return lnext(c);
#endif
}

#if PG_VERSION_NUM < 130000
/**
 * @brief Insert @p datum at position @p pos in @p list (PG < 13 backport).
 *
 * PostgreSQL 13 introduced @c list_insert_nth() when lists were
 * reimplemented as arrays.  This declaration provides the same function
 * for older PostgreSQL versions; the implementation lives in
 * @c compatibility.c.
 *
 * @param list   The list to insert into (may be @c NIL).
 * @param pos    Zero-based index at which to insert the new element.
 * @param datum  The value to insert.
 * @return       The (possibly reallocated) list.
 */
List *list_insert_nth(List *list, int pos, void *datum);
#endif

#if PG_VERSION_NUM < 140000
/** @brief OID of @c count(*) / @c count(any) aggregate function (pre-PG 14). */
#define F_COUNT_ANY 2147
/** @brief OID of @c count() aggregate function (pre-PG 14). */
#define F_COUNT_ 2803
/** @brief OID of @c sum(int4) aggregate function (pre-PG 14). */
#define F_SUM_INT4 2108
#endif

#endif /* COMPATIBILITY_H */
