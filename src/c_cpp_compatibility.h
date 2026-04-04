/**
 * @file c_cpp_compatibility.h
 * @brief Fix gettext macro conflicts between PostgreSQL and the C++ STL.
 *
 * When PostgreSQL is built without @c --enable-nls, its headers redefine
 * @c gettext, @c ngettext, @c dgettext, and @c dngettext as no-op macros.
 * Several STL and Boost headers later include @c \<libintl.h\>, which tries
 * to declare the real function prototypes for those same names.  The
 * resulting macro/declaration clash causes compilation errors.
 *
 * This header must be included in every translation unit that mixes
 * PostgreSQL C headers with C++ STL or Boost headers.  It undefines the
 * four offending macros after the PostgreSQL headers have been processed,
 * allowing @c libintl.h to declare the functions correctly.
 *
 * The cleanest long-term fix is to build PostgreSQL with @c --enable-nls,
 * but that is not always under the extension author's control.
 */
#ifndef C_CPP_COMPATIBILITY_H
#define C_CPP_COMPATIBILITY_H

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

#endif /* C_CPP_COMPATIBILITY_H */
