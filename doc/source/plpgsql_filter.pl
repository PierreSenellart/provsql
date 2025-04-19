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
  RETURNS\s+(.*?)\s+AS
}{
    my ( $function_name, $args, $return ) = ( $1, $2, $3 );

    $args =~ s/^\s+|\s+$//g;
    $args =~ s/\s+/ /gs;

    my @args = split /, */, $args;

    foreach my $a (@args) {
        $a =~ s/(.+?) ([^\s]*)((?: .*)?)/$2 $1$3/;
        $a =~ s/DEFAULT /= /;
    }

    "$return $function_name(" . join( ', ', @args ) . ")"
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
  SET\s+search_path\s+.*?;
}{
}sigx;

s{
  \$\$.*?\$\$
}{
}sigx;

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

    "struct $type_name \{ " . join( '; ', @fields ) . " \};"
}sigxe;

print;
