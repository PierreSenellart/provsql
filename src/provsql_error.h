/**
 * @file provsql_error.h
 * @brief Uniform error-reporting macros for ProvSQL.
 *
 * Defines four convenience macros that wrap PostgreSQL's @c elog() and
 * always prefix the user-visible message with @c "ProvSQL: ", giving every
 * diagnostic a consistent origin tag regardless of which source file emits
 * it.
 *
 * The prefix is inserted by compile-time string-literal concatenation, so
 * @p fmt **must** be a string literal (not a runtime @c char* variable).
 *
 * ### Availability of @c elog()
 * This header intentionally contains no @c \#include directives.  The
 * caller is responsible for making @c elog() visible before including this
 * header:
 * - In normal PostgreSQL extension code, @c elog() comes from
 *   @c \<utils/elog.h\>, pulled in transitively through @c postgres.h or
 *   @c provsql_utils.h (which already includes this file at its end).
 * - In the standalone @c tdkc binary, @c BooleanCircuit.cpp defines a
 *   lightweight @c \#define @c elog stub that writes to @c stderr and calls
 *   @c exit() on @c ERROR; @c provsql_error.h is included after that stub.
 */

#ifndef PROVSQL_ERROR_H
#define PROVSQL_ERROR_H

/**
 * @brief Report a fatal ProvSQL error and abort the current transaction.
 *
 * Expands to @c elog(ERROR, "ProvSQL: " fmt, ...).  In PostgreSQL, @c ERROR
 * performs a non-local exit via @c longjmp; the call never returns.  In the
 * standalone @c tdkc build the @c elog stub calls @c exit(EXIT_FAILURE).
 *
 * @param fmt  A string literal format string (printf-style).
 * @param ...  Optional format arguments.
 */
#define provsql_error(fmt, ...)   elog(ERROR,   "ProvSQL: " fmt, ##__VA_ARGS__)

/**
 * @brief What kind of limit a refusal or a freezing is.
 *
 * @c PROVSQL_DELIBERATE: the shape has no provenance to give, so refusing it is
 * the answer and not a shortcoming -- @c EXCEPT @c ALL and @c INTERSECT @c ALL,
 * whose kept copies have no provenance of their own; @c IN read as a value,
 * whose unknown truth no count of matches tells from false; two subquery
 * conditions in one Boolean combination, whose two counts do not meet on one
 * row.  No rewriting will remove these.
 *
 * @c PROVSQL_GAP: the query has a provenance and the rewriting does not reach
 * it yet.  Every one of these is a candidate for work.
 *
 * @c PROVSQL_OUT_OF_SCOPE: the feature lies outside what the provenance of the
 * supported query fragment covers -- random variables and continuous
 * distributions, where-provenance, conditioning, and ProvSQL's own surfaces
 * (a @c provenance() call in an expression, an @c INSERT into an untracked
 * table) -- so neither of the two above applies to it.
 *
 * The kind reaches tooling on the @c DETAIL line, next to the tag, so that a
 * survey of what is covered can tell a deliberate refusal from a gap without
 * keeping a table of its own.
 */
#define PROVSQL_DELIBERATE   "deliberate"
#define PROVSQL_GAP          "gap"
#define PROVSQL_OUT_OF_SCOPE "out-of-scope"

/**
 * @brief Refuse a query ProvSQL cannot track, and abort the transaction.
 *
 * Like @c provsql_error, with SQLSTATE @c 0A000 (@c feature_not_supported)
 * rather than @c XX000 (@c internal_error), so that clients can tell a
 * deliberate refusal from a bug.
 *
 * Every refusal carries a stable short tag, reported on the @c DETAIL line as
 * @c "provsql-reason: @c <tag>".  The message is what a user reads and may be
 * reworded freely; the tag is what tooling keys on -- the differential-testing
 * harness groups its refusals by it, and the study of what the fragment covers
 * joins on it -- so a tag changes only with a reason.  It is the first
 * argument, so that no refusal can be added without one.
 *
 * @param scope  @c PROVSQL_DELIBERATE, @c PROVSQL_GAP or
 *               @c PROVSQL_OUT_OF_SCOPE.
 * @param tag    Stable kebab-case identifier of the cause.
 * @param fmt    A string literal format string (printf-style).
 * @param ...    Optional format arguments.
 */
#ifdef TDKC
#define provsql_unsupported(scope, tag, fmt, ...)                             \
  provsql_error(fmt, ##__VA_ARGS__)
#else
#define provsql_unsupported(scope, tag, fmt, ...)                             \
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),                     \
                  errmsg("ProvSQL: " fmt, ##__VA_ARGS__),                     \
                  errdetail("provsql-reason: %s; scope: %s", tag, scope)))
#endif

/**
 * @brief Emit a ProvSQL warning message (execution continues).
 *
 * Expands to @c elog(WARNING, "ProvSQL: " fmt, ...).  The message is sent
 * to the client and server log according to the PostgreSQL
 * @c log_min_messages / @c client_min_messages settings.
 *
 * @param fmt  A string literal format string (printf-style).
 * @param ...  Optional format arguments.
 */
#define provsql_warning(fmt, ...) elog(WARNING,  "ProvSQL: " fmt, ##__VA_ARGS__)

/**
 * @brief Emit a ProvSQL warning that names its cause by a stable tag.
 *
 * The warning counterpart of @c provsql_unsupported: a freezing is reported as
 * a warning or, under @c provsql.implicit_freeze @c = @c 'error', as a
 * refusal, and both carry the same tag on their @c DETAIL line so that
 * tooling reads one key whichever the setting.
 *
 * @param scope  @c PROVSQL_DELIBERATE, @c PROVSQL_GAP or
 *               @c PROVSQL_OUT_OF_SCOPE.
 * @param tag    Stable kebab-case identifier of the cause.
 * @param fmt    A string literal format string (printf-style).
 * @param ...    Optional format arguments.
 */
#ifdef TDKC
#define provsql_warning_tagged(scope, tag, fmt, ...)                          \
  provsql_warning(fmt, ##__VA_ARGS__)
#else
#define provsql_warning_tagged(scope, tag, fmt, ...)                          \
  ereport(WARNING, (errmsg("ProvSQL: " fmt, ##__VA_ARGS__),                   \
                    errdetail("provsql-reason: %s; scope: %s", tag, scope)))
#endif

/**
 * @brief Emit a ProvSQL informational notice (execution continues).
 *
 * Expands to @c elog(NOTICE, "ProvSQL: " fmt, ...).  Typically used for
 * progress messages gated on @c provsql.verbose_level.
 *
 * @param fmt  A string literal format string (printf-style).
 * @param ...  Optional format arguments.
 */
#define provsql_notice(fmt, ...)  elog(NOTICE,   "ProvSQL: " fmt, ##__VA_ARGS__)

/**
 * @brief Write a ProvSQL message to the server log only.
 *
 * Expands to @c elog(LOG, "ProvSQL: " fmt, ...).  @c LOG messages go to the
 * PostgreSQL server log and are not forwarded to the client.  Suitable for
 * background-worker lifecycle events (e.g. worker startup).
 *
 * @param fmt  A string literal format string (printf-style).
 * @param ...  Optional format arguments.
 */
#define provsql_log(fmt, ...)     elog(LOG,      "ProvSQL: " fmt, ##__VA_ARGS__)

#endif /* PROVSQL_ERROR_H */
