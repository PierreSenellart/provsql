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
