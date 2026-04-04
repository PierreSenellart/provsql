\mainpage ProvSQL SQL API

ProvSQL exposes its functionality through SQL functions defined in the
`provsql` schema. All functions are accessible after loading the
extension and setting the search path:
\code{.sql}
SET search_path TO public, provsql;
\endcode

See the **[Topics](topics.html)** page for function groups organized by topic,
or the @ref provsql "provsql schema" for a complete flat listing.
