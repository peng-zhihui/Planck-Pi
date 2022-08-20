package Text::CharWidth;

use 5.008;
use strict;
use warnings;

require Exporter;

our @ISA = qw(Exporter);
our @EXPORT_OK = qw(mbwidth mbswidth mblen);
our @EXPORT = qw();
our %EXPORT_TAGS = ('all' => [ @EXPORT_OK ]);

our $VERSION = '0.04';

require XSLoader;
XSLoader::load('Text::CharWidth', $VERSION);

# Preloaded methods go here.

1;
__END__
# Below is stub documentation for your module. You'd better edit it!

=head1 NAME

Text::CharWidth - Get number of occupied columns of a string on terminal

=head1 SYNOPSIS

  use Text::CharWidth qw(mbwidth mbswidth mblen);
  mbwidth(string);
  mbswidth(string);
  mblen(string);

=head1 DESCRIPTION

This module supplies features similar as wcwidth(3) and wcswidth(3)
in C language.

Characters have its own width on terminal depending on locale.
For example, ASCII characters occupy one column per character,
east Asian fullwidth characters (like Hiragana or Han Ideograph)
occupy two columns per character, and combining characters (apperaring
in ISO-8859-11 Thai, Unicode, and so on) occupy zero columns per
character.  mbwidth() gives the width of the first character of
the given string and mbswidth() gives the width of the whole given
string.

The names of mbwidth and mbswidth came from "multibyte" versions
of wcwidth and wcswidth which are "wide character" versions.

I<mblen(string)> returns number of bytes of the first character of the
string.  Please note that a character may consist of multiple
bytes in multibyte encodings such as UTF-8, EUC-JP, EUC-KR,
GB2312, or Big5.

I<mbwidth(string)> returns the width of the first character of the
string.  I<mbswidth(string)> returns the width of the whole string.

Parameters are to be given in locale encodings, not always in UTF-8.

=head1 SEE ALSO

locale(5), wcwidth(3), wcswidth(3)

=head1 AUTHOR

Tomohiro KUBOTA, E<lt>kubota@debian.orgE<gt>

=head1 COPYRIGHT AND LICENSE

Copyright 2003 by Tomohiro KUBOTA

This library is free software; you can redistribute it and/or modify
it under the same terms as Perl itself. 

=cut
