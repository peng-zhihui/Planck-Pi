package Text::Iconv;
# @(#) $Id: Iconv.pm,v 1.10 2007/10/17 14:14:22 mxp Exp $
# Copyright (c) 2007 Michael Piotrowski

use strict;
use vars qw($VERSION @ISA @EXPORT @EXPORT_OK);

require Exporter;
require DynaLoader;
require AutoLoader;

@ISA = qw(Exporter AutoLoader DynaLoader);
# Items to export into callers namespace by default. Note: do not export
# names by default without a very good reason. Use EXPORT_OK instead.
# Do not simply export all your public functions/methods/constants.
@EXPORT_OK = qw(
	convert
);
$VERSION = '1.7';

bootstrap Text::Iconv $VERSION;

# Preloaded methods go here.

# Autoload methods go after =cut, and are processed by the autosplit program.

1;
__END__
# Below is the documentation for the module.

=head1 NAME

Text::Iconv - Perl interface to iconv() codeset conversion function

=head1 SYNOPSIS

  use Text::Iconv;
  $converter = Text::Iconv->new("fromcode", "tocode");
  $converted = $converter->convert("Text to convert");

=head1 DESCRIPTION

The B<Text::Iconv> module provides a Perl interface to the iconv()
function as defined by the Single UNIX Specification.

The convert() method converts the encoding of characters in the input
string from the I<fromcode> codeset to the I<tocode> codeset, and
returns the result.

Settings of I<fromcode> and I<tocode> and their permitted combinations
are implementation-dependent.  Valid values are specified in the
system documentation; the iconv(1) utility should also provide a B<-l>
option that lists all supported codesets.

=head2 Utility methods

B<Text::Iconv> objects also provide the following methods:

retval() returns the return value of the underlying iconv() function
for the last conversion; according to the Single UNIX Specification,
this value indicates "the number of non-identical conversions
performed."  Note, however, that iconv implementations vary widely in
the interpretation of this specification.

This method can be called after calling convert(), e.g.:

  $result = $converter->convert("lorem ipsum dolor sit amet");
  $retval = $converter->retval;

When called before the first call to convert(), or if an error occured
during the conversion, retval() returns B<undef>.

get_attr(): This method is only available with GNU libiconv, otherwise
it throws an exception.  The get_attr() method allows you to query
various attributes which influence the behavior of convert().  The
currently supported attributes are I<trivialp>, I<transliterate>, and
I<discard_ilseq>, e.g.:

  $state = $converter->get_attr("transliterate");

See iconvctl(3) for details.  To ensure portability to other iconv
implementations you should first check for the availability of this
method using B<eval {}>, e.g.:

    eval { $conv->get_attr("trivialp") };
    if ($@)
    {
      # get_attr() is not available
    }
    else
    {
      # get_attr() is available
    }

This method should be considered experimental.

set_attr(): This method is only available with GNU libiconv, otherwise
it throws an exception.  The set_attr() method allows you to set
various attributes which influence the behavior of convert().  The
currently supported attributes are I<transliterate> and
I<discard_ilseq>, e.g.:

  $state = $converter->set_attr("transliterate");

See iconvctl(3) for details.  To ensure portability to other iconv
implementations you should first check for the availability of this
method using B<eval {}>, cf. the description of set_attr() above.

This method should be considered experimental.

=head1 ERRORS

If the conversion can't be initialized an exception is raised (using
croak()).

=head2 Handling of conversion errors

I<Text::Iconv> provides a class attribute B<raise_error> and a
corresponding class method for setting and getting its value.  The
handling of errors during conversion depends on the setting of this
attribute.  If B<raise_error> is set to a true value, an exception is
raised; otherwise, the convert() method only returns B<undef>.  By
default B<raise_error> is false.  Example usage:

  Text::Iconv->raise_error(1);     # Conversion errors raise exceptions
  Text::Iconv->raise_error(0);     # Conversion errors return undef
  $a = Text::Iconv->raise_error(); # Get current setting

=head2 Per-object handling of conversion errors

As an experimental feature, I<Text::Iconv> also provides an instance
attribute B<raise_error> and a corresponding method for setting and
getting its value.  If B<raise_error> is B<undef>, the class-wide
settings apply.  If B<raise_error> is 1 or 0 (true or false), the
object settings override the class-wide settings.

Consult L<iconv(3)> for details on errors that might occur.

=head2 Conversion of B<undef>

Converting B<undef>, e.g.,

  $converted = $converter->convert(undef);

always returns B<undef>.  This is not considered an error.

=head1 NOTES

The supported codesets, their names, the supported conversions, and
the quality of the conversions are all system-dependent.

=head1 AUTHOR

Michael Piotrowski <mxp@dynalabs.de>

=head1 SEE ALSO

iconv(1), iconv(3)

=cut
