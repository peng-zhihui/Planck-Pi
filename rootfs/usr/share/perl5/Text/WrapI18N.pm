package Text::WrapI18N;

require Exporter;
use strict;
use warnings;

our @ISA = qw(Exporter);
our @EXPORT = qw(wrap);
our @EXPORT_OK = qw($columns $separator);
our %EXPORT_TAGS = ('all' => [ @EXPORT, @EXPORT_OK ]);

our $VERSION = '0.06';

use vars qw($columns $break $tabstop $separator $huge $unexpand $charmap);
use Text::CharWidth qw(mbswidth mblen);

BEGIN {
	$columns = 76;
	# $break, $separator, $huge, and $unexpand are not supported yet.
	$break = '\s';
	$tabstop = 8;
	$separator = "\n";
	$huge = 'wrap';
	$unexpand = 1;
	undef $charmap;
}

sub wrap {
	my $top1=shift;
	my $top2=shift;
	my $text=shift;

	$text = $top1 . $text;

	# $out     already-formatted text for output including current line
	# $len     visible width of the current line without the current word
	# $word    the current word which might be sent to the next line
	# $wlen    visible width of the current word
	# $c       the current character
	# $b       whether to allow line-breaking after the current character
	# $cont_lf true when LF (line feed) characters appear continuously
	# $w       visible width of the current character

	my $out = '';
	my $len = 0;
	my $word = '';
	my $wlen = 0;
	my $cont_lf = 0;
	my ($c, $w, $b);
	$text =~ s/\n+$/\n/;
	while(1) {
		if (length($text) == 0) {
			return $out . $word;
		}
		($c, $text, $w, $b) = _extract($text);
		if ($c eq "\n") {
			$out .= $word . $separator;
			if (length($text) == 0) {return $out;}
			$len = 0;
			$text = $top2 . $text;
			$word = '' ; $wlen = 0;
			next;
		} elsif ($w == -1) {
			# all control characters other than LF are ignored
			next;
		}

		# when the current line have enough room
		# for the curren character

		if ($len + $wlen + $w <= $columns) {
			if ($c eq ' ' || $b) {
				$out .= $word . $c;
				$len += $wlen + $w;
				$word = ''; $wlen = 0;
			} else {
				$word .= $c; $wlen += $w;
			}
			next;
		}

		# when the current line overflows with the
		# current character

		if ($c eq ' ') {
			# the line ends by space
			$out .= $word . $separator;
			$len = 0;
			$text = $top2 . $text;
			$word = ''; $wlen = 0;
		} elsif ($wlen + $w <= $columns - length ($top2)) {
			# the current word is sent to next line
			$out .= $separator;
			$len = 0;
			$text = $top2 . $word . $c . $text;
			$word = ''; $wlen = 0;
		} else {
			# the current word is too long to fit a line
			$out .= $word . $separator;
			$len = 0;
			$text = $top2 . $c . $text;
			$word = ''; $wlen = 0;
		}
	}
}


# Extract one character from the beginning from the given string.
# Supports multibyte encodings such as UTF-8, EUC-JP, EUC-KR,
# GB2312, and Big5.
#
# return value: (character, rest string, width, line breakable)
#   character: a character.  This may consist from multiple bytes.
#   rest string: given string without the extracted character.
#   width: number of columns which the character occupies on screen.
#   line breakable: true if the character allows line break after it.

sub _extract {
	my $string=shift;
	my ($l, $c, $r, $w, $b, $u);

	if (length($string) == 0) {
		return ('', '', 0, 0);
	}
	$l = mblen($string);
	if ($l == 0 || $l == -1) {
		return ('?', substr($string,1), 1, 0);
	}
	$c = substr($string, 0, $l);
	$r = substr($string, $l);
	$w = mbswidth($c);

	if (!defined($charmap)) {
		$charmap = `/usr/bin/locale charmap`;
	}

	if ($charmap =~ /UTF.8/i) {
		# UTF-8
		if ($l == 3) {
			# U+0800 - U+FFFF
			$u = (ord(substr($c,0,1))&0x0f) * 0x1000 
			    + (ord(substr($c,1,1))&0x3f) * 0x40
			    + (ord(substr($c,2,1))&0x3f);
			$b = _isCJ($u);
		} elsif ($l == 4) {
			# U+10000 - U+10FFFF
			$u = (ord(substr($c,0,1))&7) * 0x40000 
			    + (ord(substr($c,1,1))&0x3f) * 0x1000
			    + (ord(substr($c,2,1))&0x3f) * 0x40
			    + (ord(substr($c,3,1))&0x3f);
			$b = _isCJ($u);
		} else {
			$b = 0;
		}
	} elsif ($charmap =~ /(^EUC)|(^GB)|(^BIG)/i) {
		# East Asian legacy encodings
		# (EUC-JP, EUC-KR, GB2312, Big5, Big5HKSCS, and so on)

		if (ord(substr($c,0,1)) >= 0x80) {$b = 1;} else {$b = 0;}
	} else {
		$b = 0;
	}
	return ($c, $r, $w, $b);
}

# Returns 1 for Chinese and Japanese characters.  This means that
# these characters allow line wrapping after this character even
# without whitespaces because these languages don't use whitespaces
# between words.
#
# Character must be given in UCS-4 codepoint value.

sub _isCJ {
	my $u=shift;

	if ($u >= 0x3000 && $u <= 0x312f) {
		if ($u == 0x300a || $u == 0x300c || $u == 0x300e ||
		    $u == 0x3010 || $u == 0x3014 || $u == 0x3016 ||
		    $u == 0x3018 || $u == 0x301a) {return 0;}
		return 1;
	}  # CJK punctuations, Hiragana, Katakana, Bopomofo
	if ($u >= 0x31a0 && $u <= 0x31bf) {return 1;}  # Bopomofo
	if ($u >= 0x31f0 && $u <= 0x31ff) {return 1;}  # Katakana extension
	if ($u >= 0x3400 && $u <= 0x9fff) {return 1;}  # Han Ideogram
	if ($u >= 0xf900 && $u <= 0xfaff) {return 1;}  # Han Ideogram
	if ($u >= 0x20000 && $u <= 0x2ffff) {return 1;}  # Han Ideogram

	return 0;
}

1;
__END__

=head1 NAME

Text::WrapI18N - Line wrapping module with support for multibyte, fullwidth,
and combining characters and languages without whitespaces between words

=head1 SYNOPSIS

  use Text::WrapI18N qw(wrap $columns);
  wrap(firstheader, nextheader, texts);

=head1 DESCRIPTION

This module intends to be a better Text::Wrap module.  
This module is needed to support multibyte character encodings such
as UTF-8, EUC-JP, EUC-KR, GB2312, and Big5.  This module also supports
characters with irregular widths, such as combining characters (which
occupy zero columns on terminal, like diacritical marks in UTF-8) and
fullwidth characters (which occupy two columns on terminal, like most
of east Asian characters).  Also, minimal handling of languages which
doesn't use whitespaces between words (like Chinese and Japanese) is
supported.

Like Text::Wrap, hyphenation and "kinsoku" processing are not supported,
to keep simplicity.

I<wrap(firstheader, nextheader, texts)> is the main subroutine of
Text::WrapI18N module to execute the line wrapping.  Input parameters
and output data emulate Text::Wrap.  The texts have to be written in
locale encoding.

=head1 SEE ALSO

locale(5), utf-8(7), charsets(7)

=head1 AUTHOR

Tomohiro KUBOTA, E<lt>kubota@debian.orgE<gt>

=head1 COPYRIGHT AND LICENSE

Copyright 2003 by Tomohiro KUBOTA

This library is free software; you can redistribute it and/or modify
it under the same terms as Perl itself. 

=cut
