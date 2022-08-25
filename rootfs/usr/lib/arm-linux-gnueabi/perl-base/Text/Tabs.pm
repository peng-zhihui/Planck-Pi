package Text::Tabs;

require Exporter;

@ISA = (Exporter);
@EXPORT = qw(expand unexpand $tabstop);

use vars qw($VERSION $SUBVERSION $tabstop $debug);
$VERSION = 2013.0523;
$SUBVERSION = 'modern';

use strict;

use 5.010_000;

BEGIN	{
	$tabstop = 8;
	$debug = 0;
}

my $CHUNK = qr/\X/;

sub _xlen (_) { scalar(() = $_[0] =~ /$CHUNK/g) } 
sub _xpos (_) { _xlen( substr( $_[0], 0, pos($_[0]) ) ) }

sub expand {
	my @l;
	my $pad;
	for ( @_ ) {
		my $s = '';
		for (split(/^/m, $_, -1)) {
			my $offs = 0;
			s{\t}{
			    # this works on both 5.10 and 5.11
				$pad = $tabstop - (_xlen(${^PREMATCH}) + $offs) % $tabstop;
			    # this works on 5.11, but fails on 5.10
				#XXX# $pad = $tabstop - (_xpos() + $offs) % $tabstop;
				$offs += $pad - 1;
				" " x $pad;
			}peg;
			$s .= $_;
		}
		push(@l, $s);
	}
	return @l if wantarray;
	return $l[0];
}

sub unexpand
{
	my (@l) = @_;
	my @e;
	my $x;
	my $line;
	my @lines;
	my $lastbit;
	my $ts_as_space = " " x $tabstop;
	for $x (@l) {
		@lines = split("\n", $x, -1);
		for $line (@lines) {
			$line = expand($line);
			@e = split(/(${CHUNK}{$tabstop})/,$line,-1);
			$lastbit = pop(@e);
			$lastbit = '' 
				unless defined $lastbit;
			$lastbit = "\t"
				if $lastbit eq $ts_as_space;
			for $_ (@e) {
				if ($debug) {
					my $x = $_;
					$x =~ s/\t/^I\t/gs;
					print "sub on '$x'\n";
				}
				s/  +$/\t/;
			}
			$line = join('',@e, $lastbit);
		}
		$x = join("\n", @lines);
	}
	return @l if wantarray;
	return $l[0];
}

1;
__END__

sub expand
{
	my (@l) = @_;
	for $_ (@l) {
		1 while s/(^|\n)([^\t\n]*)(\t+)/
			$1. $2 . (" " x 
				($tabstop * length($3)
				- (length($2) % $tabstop)))
			/sex;
	}
	return @l if wantarray;
	return $l[0];
}

