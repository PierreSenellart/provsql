/**
 * @file c_cpp_compatibility.h
 * @brief Fix macro conflicts between PostgreSQL headers and the C++ STL/Boost.
 *
 * Two families of PostgreSQL macros break C++ headers included after them:
 *
 * - When PostgreSQL is built without @c --enable-nls, its headers redefine
 *   @c gettext, @c ngettext, @c dgettext, and @c dngettext as no-op macros.
 *   Several STL and Boost headers later include @c \<libintl.h\>, which tries
 *   to declare the real function prototypes for those same names.  The
 *   resulting macro/declaration clash causes compilation errors.
 *
 * - @c port.h replaces the printf family (@c snprintf, @c sprintf…) with
 *   @c pg_* macros for output consistency.  Any later-included C++ header
 *   that calls the @c std::-qualified functions then breaks -- e.g.
 *   @c boost/assert/source_location.hpp (Boost >= 1.79) calls
 *   @c std::snprintf, which the macro turns into the non-existent
 *   @c std::pg_snprintf.
 *
 * This header must be included in every translation unit that mixes
 * PostgreSQL C headers with C++ STL or Boost headers, *after* the
 * PostgreSQL headers.  It undefines the offending macros so subsequent
 * STL/Boost headers parse cleanly.  C++ code does not rely on
 * @c pg_snprintf semantics (such as @c %m), so losing the replacement is
 * harmless.  @c provsql_utils.h includes it automatically when compiled
 * as C++, so most translation units are covered transitively.
 *
 * Deliberately *no* include guard: undefining is idempotent, and a guard
 * would make a second inclusion a no-op -- leaving the macros defined when
 * the first inclusion happened before the PostgreSQL headers.
 *
 * The cleanest long-term fix for the gettext clash is to build PostgreSQL
 * with @c --enable-nls, but that is not always under the extension
 * author's control.
 */

/* When compiling PostgreSQL without --enable-nls, gettext, ngettext,
 * dgetttext, dngettext are replaced with macros; but some STL headers (in
 * particular used by Boost) use the definitions of these functions from
 * libintl.h; to avoid conflicts between these declarations, for files
 * that include both C PostgreSQL headers and STL headers, we need to
 * undefine the macros. It is usually better to compile PostgreSQL with
 * --enable-nls, though, but we do not have control over this. */

#undef gettext
#undef ngettext
#undef dgettext
#undef dngettext

/* port.h's printf-family replacements (see file comment). */

#undef snprintf
#undef vsnprintf
#undef sprintf
#undef vsprintf
#undef fprintf
#undef vfprintf
#undef printf
#undef vprintf
