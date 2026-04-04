#!/usr/bin/env perl

use strict;
use warnings;

my $input_file = $ARGV[0] // '';
$_ = join '', <>;

# Pre-pass: record the original line number for each named SQL entity.
# Used later to emit #line directives so Doxygen links back to the real source.
my %line_of;
{
    my $lno = 0;
    for my $ln (split /\n/, $_) {
        $lno++;
        if ($ln =~ /CREATE\s+(?:OR\s+REPLACE\s+)?(?:FUNCTION|AGGREGATE)\s+(\w+)\s*\(/i) {
            $line_of{lc $1} //= $lno;
        }
        if ($ln =~ /CREATE\s+(?:OR\s+REPLACE\s+)?TYPE\s+(\w+)/i) {
            $line_of{lc $1} //= $lno;
        }
        if ($ln =~ /CREATE\s+TABLE\s+(\w+)\s*\(/i) {
            $line_of{lc $1} //= $lno;
        }
        if ($ln =~ /CREATE\s+CAST\s*\(\s*(\w+)\s+AS\s+(\w+)/i) {
            $line_of{lc "cast_${1}_to_${2}"} //= $lno;
        }
        # Operators: look ahead handled by two-line context below.
    }
    # Two-pass for operators: find LEFTARG/RIGHTARG within each CREATE OPERATOR block.
    my $text = join "\n", split /\n/, $_;
    while ($text =~ /CREATE\s+OPERATOR\s+([^\s(]+)\s*\(([^;]*?)\)/gsi) {
        my ($op, $body, $start) = ($1, $2, pos($text) - length($&));
        my $lno_op = 1 + (() = substr($text, 0, $start) =~ /\n/g);
        my ($left)  = ($body =~ /LEFTARG\s*=\s*(\w+)/i);
        my ($right) = ($body =~ /RIGHTARG\s*=\s*(\w+)/i);
        my %omap = ('<'=>'lt','<='=>'le','='=>'eq','<>'=>'ne','>='=>'ge','>'=>'gt');
        if ($left && $right) {
            my $oname = lc "${left}_" . ($omap{$op}//'op') . "_${right}";
            $line_of{$oname} //= $lno_op;
        }
    }
}

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

# Re-enter namespace after a close (e.g., provsql.14.sql reopens the schema)
s{
  \}\n+SET\s+search_path\s+TO\s+provsql;
}{
  \}\nnamespace provsql \{
}sigx;

# Ignore other search_path changes (redundant SET within open namespace)
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

# Convert CREATE TABLE to a C struct (stripping DEFAULT/NOT NULL/etc.)
s{
  CREATE\s+TABLE\s+(\w+)\s*
  \(((?:[^()]+|\((?:[^()]+|\([^()]*\))*\))*)\)
  \s*;
}{
  my ($name, $body) = ($1, $2);
  my @parts;
  my ($depth, $cur) = (0, '');
  for my $ch (split //, $body) {
    if    ($ch eq '(')                { $depth++; $cur .= $ch }
    elsif ($ch eq ')')                { $depth--; $cur .= $ch }
    elsif ($ch eq ',' && $depth == 0) { push @parts, $cur; $cur = '' }
    else                              { $cur .= $ch }
  }
  push @parts, $cur if $cur =~ /\S/;
  my @cols;
  for my $col (@parts) {
    $col =~ s/^\s+|\s+$//g;
    next if $col =~ /^\s*(?:CONSTRAINT|PRIMARY\s+KEY|FOREIGN\s+KEY|UNIQUE\s*\(|CHECK\s*\()/i;
    if ($col =~ /^(\w+)\s+(\w+)/) {
      push @cols, "$2 $1";
    }
  }
  "struct $name \{ " . join('; ', @cols) . "; \};"
}sigxe;

# Convert CREATE CAST to a C-style function declaration
s{
  CREATE\s+CAST\s*\(\s*(\w+)\s+AS\s+(\w+)\s*\)
  \s*WITH\s+FUNCTION\s+\w+\s*\([^)]*\)
  (?:\s+AS\s+IMPLICIT)?
  \s*;
}{
  my ($from, $to) = ($1, $2);
  "${to} CAST_${from}_TO_${to}(${from});"
}sigxe;

# Convert CREATE OPERATOR to a C-style boolean function declaration
my %op_map = ('<' => 'lt', '<=' => 'le', '=' => 'eq', '<>' => 'ne', '>=' => 'ge', '>' => 'gt');
s{
  CREATE\s+OPERATOR\s+([^\s(]+)\s*\(\s*((?:[^()]*|\([^()]*\))*)\)\s*;
}{
  my ($op, $body) = ($1, $2);
  my ($left)  = ($body =~ /LEFTARG\s*=\s*(\w+)/i);
  my ($right) = ($body =~ /RIGHTARG\s*=\s*(\w+)/i);
  my $op_name = $op_map{$op} // 'op';
  ($left && $right)
    ? "boolean ${left}_${op_name}_${right}(${left} left, ${right} right);"
    : ""
}sigxe;

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
$_ = "/** \@cond INTERNAL */\n${typedefs}/** \@endcond */\n" . $_ if $typedefs;

# Normalize multi-word SQL types to use underscores (matching the typedefs above)
s/\bDOUBLE PRECISION\b/DOUBLE_PRECISION/gi;
s/\bCHARACTER VARYING\b/CHARACTER_VARYING/gi;

# Convert SQL array notation (type[]) to C-compatible names
s/(\w+)\[\]/${1}_array/g;

# Inject source links into doc comments, pointing to Doxygen's source browser.
# The source browser (FILTER_SOURCE_FILES=NO) shows the original SQL at the
# correct line numbers recorded by the pre-pass above.
if ($input_file) {
    # Derive Doxygen's source-browser HTML filename from the input filename.
    # e.g. "provsql.sql" -> "provsql_8sql_source.html"
    (my $source_html = ($input_file =~ m{([^/]+)$})[0]) =~ s/\./_8/;
    $source_html .= '_source.html';

    # Functions, aggregates, operators, casts (declarations with parentheses)
    s{
        (/\*\*(?:[^*]|\*(?!/))*\*/)   # doc comment block
        (\s*\n)                         # separator
        ([A-Za-z_]\w*(?:\s+\w+)*\s+\w+\s*\([^\n]*\)\s*;)  # declaration
    }{
        my ($cmt, $sep, $decl) = ($1, $2, $3);
        my ($name) = lc(($decl =~ /(\w+)\s*\(/)[0] // '');
        if (my $lno = $line_of{$name}) {
            my $anchor = sprintf("l%05d", $lno);
            $cmt =~ s|\s*\*/|\n * \@par Source code\n * <a href="${source_html}#${anchor}">provsql.sql line $lno</a>\n */|;
        }
        "$cmt$sep$decl"
    }msxge;

    # Structs and enums (type and table declarations)
    s{
        (/\*\*(?:[^*]|\*(?!/))*\*/)   # doc comment block
        (\s*\n)                         # separator
        ((struct|enum)\s+(\w+)\b[^\n]*;)  # declaration
    }{
        my ($cmt, $sep, $decl, $kind, $name) = ($1, $2, $3, $4, lc $5);
        if (my $lno = $line_of{$name}) {
            my $anchor = sprintf("l%05d", $lno);
            $cmt =~ s|\s*\*/|\n * \@par Source code\n * <a href="${source_html}#${anchor}">provsql.sql line $lno</a>\n */|;
        }
        "$cmt$sep$decl"
    }msxge;
}

print;
