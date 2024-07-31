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
