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
    $args =~ s/^\s+|\s+$//g;
    $args =~ s/\s+/ /gs;

    my @args = split /, */, $args;

    foreach my $a (@args) {
        # SQL parameter format is "name type [DEFAULT val]".
        # Extract optional DEFAULT first, then rearrange to C-style "type name".
        my $default = ($a =~ s/\s+DEFAULT\s+(.+)$//) ? " = $1" : "";
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

s{
  \$\$.*?\$\$\s+LANGUAGE.*?;
}{
}sigxg;

s{
  \$\$.*?\$\$;
}{
}sigxg;

s{
  CREATE\s+TYPE\s+([^\s]+)\s+AS\s+ENUM\((.*?)\);
}{
  enum $1 { $2 };
}sigx;

s{
  ([^\n])--
}{$1///<}sigx;
s{
  --
}{///}sigx;

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

s{
  CREATE\s+TYPE\s+([^\s]+)\s*
  \(([^)]*)\);
}{
  my ($type, $members) = ($1, $2);
  $members =~ s/,/;/g;
  "struct $type \{ $members; \};";
}sigxe;

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

# Strip CREATE CAST, CREATE TABLE, and standalone SELECT statements
s{
  \bCREATE\s+CAST\s*\([^;]*\);
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
  SETOF<$1>
}sigx;

print;
