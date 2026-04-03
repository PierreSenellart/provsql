#!/usr/bin/env perl

use strict;
use warnings;

$_ = join '', <>;

s{
  CREATE\s+
  (?:OR\s+REPLACE\s+)?
  FUNCTION\s+
  (\w+)\s*
  \((.*?)\)\s+
  (?:RETURNS\s+(.*?)\s+)?
  AS
  ([^;]*LANGUAGE\s+C[^;]*;)?
}{
    my ( $function_name, $args, $return ) = ( $1, $2, $3 );

    $args =~ s/\bOUT\s+/&/g;
    $args =~ s/\bIN\s+//g;
    $args =~ s/\bVARIADIC\s+//g;
    $args =~ s/^\s+|\s+$//g;
    $args =~ s/\s+/ /gs;

    my @args = split /, */, $args;

    foreach my $a (@args) {
        # SQL parameter format is "name type [DEFAULT val]".
        # Extract optional DEFAULT first, then rearrange to C-style "type name".
        my $default = ($a =~ s/\s+DEFAULT\s+(.+)$//) ? " = $1"
                : ($a =~ s/\s*=\s*(.+)$//)     ? " = $1"
                : "";
        $a =~ s/^(\S+)\s+(.*\S)\s*$/$2 $1/;
        $a .= $default;
    }

    $return //= '';
    # Strip LANGUAGE clause that leaks in for "LANGUAGE lang AS $$" style functions
    $return =~ s/\s*\bLANGUAGE\s+\w+.*$//s;
    $return =~ s/\s+$//;
    $return =~ s/TABLE\(.*?\)/TABLE/g;
    # Use void when there is no explicit return type (OUT-parameter-only functions)
    $return ||= 'void';

    "$return $function_name(" . join( ', ', @args ) . ");"
}sigxe;

# Start namespace
s{
  CREATE\s+SCHEMA\s+(.*?);
}{
  namespace $1 \{
}sigx;

# End namespace
s{
  SET\s+search_path\s+TO\s+public;
}{
  \}
}sigx;

# Ignore other search_path changes
s{
  \nSET\s+search_path\s+.*?;
}{
}sigx;

# Strip function bodies: use (?:(?!\$\$).)* to prevent matching across
# multiple $$ delimiters (which would eat comments between functions)
s{
  \$\$(?:(?!\$\$).)*\$\$\s+LANGUAGE.*?;+
}{
}sigxg;

s{
  \$\$(?:(?!\$\$).)*\$\$;+
}{
}sigxg;

s{
  CREATE\s+TYPE\s+([^\s]+)\s+AS\s+ENUM\s*\((.*?)\);
}{
  enum $1 { $2 };
}sigx;

# Strip pure-dash separator lines (-- followed only by dashes/spaces)
s{^[ \t]*--[-\s]*$}{}mg;
# Convert remaining full-line SQL comments, stripping trailing decorative dashes
s{^(\s*)--\s?(.*?)\s*-*\s*$}{$1///$2}mg;
# Convert remaining inline SQL comments (-- after code on the same line)
s{([^\n])--}{$1///<}sigx;

s{
  CREATE\s+TYPE\s+([^\s]+)\s+AS\s+\((.*?)\);
}{
    my ( $type_name, $fields ) = ( $1, $2 );

    $fields =~ s/^\s+|\s+$//g;
    $fields =~ s/\s+/ /gs;

    my @fields = split /, */, $fields;

    foreach my $a (@fields) {
        $a =~ /(.+?) (.+)/;
        my ($name, $type) = ($1, $2);
        $type =~ s/ /_/g;
        if($type =~ /\[\]/) {
          $name.="[]";
          $type=~s/\[\]//;
        }
        $a = "$type $name";
    }

    "struct $type_name \{ " . join( '; ', @fields ) . "; \};"
}sigxe;

s{
  CREATE\s+TYPE\s+([^\s]+)\s*;
}{
  struct $1;
}sigx;

# Strip base type definitions (internal PostgreSQL storage attributes, not useful for docs)
s{
  CREATE\s+TYPE\s+([^\s]+)\s*
  \(([^)]*)\);
}{}sigx;

# AGGREGATE: body uses (?:[^)']|'[^']*')* to handle ')' inside string literals
s{
  CREATE\s+(?:OR\s+REPLACE\s+)?
  AGGREGATE\s+
  (\w+)\s*\(([^)]*)\)\s*
  \((?:[^)']|'[^']*')*\);
}{
  my ($name, $args) = ($1, $2);
  "void $name($args);"
}sigxe;

s{
  \bGRANT\s+[^;]*;
}{}sigx;

# Strip CREATE CAST, CREATE OPERATOR, CREATE TABLE, and standalone SELECT statements
s{
  \bCREATE\s+CAST\b[^;]*;
}{}sigxg;

s{
  \bCREATE\s+OPERATOR\b.*?\);
}{}sigxg;

s{
  \bCREATE\s+TABLE\b.*?;
}{}sigxg;

s{
  \bSELECT\b[^;]*;
}{}sigxg;

s{
  SETOF\s+([^\s]+)
}{
  SETOF_$1
}sigx;

# Prepend hidden typedefs so Doxygen's C parser accepts SQL type names.
# These have no Doxygen comments so they won't appear in the documentation.
# DOUBLE PRECISION and type[] are not valid C, so we define them as typedefs.
my $typedefs = "";
$typedefs .= "typedef double DOUBLE_PRECISION;\n"   if /\bDOUBLE PRECISION\b/i;
$typedefs .= "typedef char* CHARACTER_VARYING;\n"    if /\bCHARACTER VARYING\b/i;
while (/\b(\w+)\[\]/g) {
    $typedefs .= "typedef void* ${1}_array;\n";
}
# Deduplicate typedefs
my %seen;
$typedefs = join("", grep { !$seen{$_}++ } split(/^/m, $typedefs));
$_ = $typedefs . $_ if $typedefs;

# Normalize multi-word SQL types to use underscores (matching the typedefs above)
s/\bDOUBLE PRECISION\b/DOUBLE_PRECISION/gi;
s/\bCHARACTER VARYING\b/CHARACTER_VARYING/gi;

# Convert SQL array notation (type[]) to C-compatible names
s/(\w+)\[\]/${1}_array/g;

print;
