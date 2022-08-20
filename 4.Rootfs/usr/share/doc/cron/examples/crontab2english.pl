#!/usr/bin/perl
#Time-stamp: "2001-07-29 16:07:28 MDT"
my $VERSION = '0.71';
require 5.00404; # I don't think there's (?: ... ) before that.

=head1 NAME

crontab2english -- explain crontab commands in English

=head1 SYNOPSIS

  Usage:
    % crontab2english [-f] files...
  Or:
    % cat files... | crontab2english
    
  If you do just this:
    % crontab2english
  then it's the same as crontab -l | crontab2english

  Example output:
  % crontab2english | less
  Setting env var MAILTO to hulahoops@polygon.int
  
  Command: (line 2)
    Run: /bin/csh -c 'perl ~/thang.pl | mail -s hujambo root'
    At: 8:10am on    the 15th of    every month
  
  Command: (line 5)
    Run: df -k
    At: 5:40am    every day
  
  Command: (line 7)
    Run: ls -l /tmp
    At: 6:50am    every Monday

  Or with the -f ("f" for filter) switch, it just adds comments
  to the input file:
  
  % crontab2english -f | less
  # My happy crontab file
  MAILTO=hulahoops@polygon.int
  10 8 15 * * /bin/csh -c 'perl ~/thang.pl | mail -s hujambo root'
  #>  At: 8:10am on    the 15th of    every month
  
  
  40 5 * * * df -k
  #>  At: 5:40am    every day
  
  50 6 * * 1 ls -l /tmp
  #>  At: 6:50am    every Monday

=head1 DESCRIPTION

It's easy to make mistakes in crontab files.  Running
C<crontab2english> on your crontab files and reading the
resulting English explanations will help you catch errors.

=head1 SWITCHES

C<-f> puts this in "filter mode" -- the output is just
the input plus commentary.

C<-v> describes the current C<crontab2english>
version to STDOUT and exits.

C<-p> forces POSIX-only mode: anything not allowed in
the POSIX crontab spec won't be understood.

C<-e> (usually default) turns off POSIX-only mode:
i.e., it doesn't feign ignorance of things not in
the POSIX spec.

C<--> signals end of switches.

=head1 ENVIRONMENT

If the evironment variables C<POSIXLY_CORRECT> and/or
C<POSIX_ME_HARDER> are true, then this turns on C<-p> (POSIX-only)
mode by default.  That's overrideable with the C<-e> switch.

=head1 CAVEATS

I've tried to make this program understand all the kinds of crontab
lines that are out there.  That probably includes a few kinds of lines
that your particular cron daemon doesn't understand, so just because
crontab2english understands something doesn't mean your cron daemon
will.

Pragmatically, however, there seem to be three kinds of cron daemons
around these days:

=over

=item *

Ones based on old (1993ish) Vixie crontab.  These understand all of
POSIX, and more.  This is what almost almost everyone runs these days.

=item *

Ones that understand I<only> what the POSIX crontab spec allows --
which excludes all sorts of amenities including: stepped ranges
("1-9/2"), "VAR=NAME" lines, English month or day abbreviations ("mon"
or "jan"), day 7 meaning "Sunday", and "*/3" meaning "every third...".

=item *

Even more recent (post-1993ish) Vixies.  These seem relatively rare.
They seem to be just like old Vixies, plus they understand at-words
like "@annually", "@reboot", etc. (altho in some cases, those aren't
mentioned in the docs!).

=back

There I<could> be some ancient or demented pre-Vixie non-POSIX crons
running somewhere.  One hopes that these would all basically
understand anything that POSIX does (and possibly nothing more?), but
you just might find peculiarities including:

=over

=item *

Rejecting 0 for Sunday, and accepting only 7.

=item *

Rejecting 7 for Sunday, and accepting only 0.

=item *

Accepting lists I<or> ranges, but no lists that include ranges.  I.e.,
allowing "7,8,9", allowing "1-3", but I<not> allowing "1-3,7-9".

=item *

Not accepting ranges at all?

=back

Consult your man pages carefully.

Good general advice: keep your crontab lines simple, and that'll
minimize the chances of disagreement between what you indend, what
crontab2english understands (with or without C<-p>), and what your
cron daemon understands.

=head1 SEE ALSO

C<man 1 crontab>

C<man 5 crontab>

C<man 8 cron>

=head1 BUG REPORTS

If this program explains a crontab line wrong, or can't parse
it, then email me the line, and an explanation of how it you
think it should parse.

=head1 DISCLAIMER

C<crontab2english> is distributed in the hope that it will be useful,
but B<without any warranty>; without even the implied warranty of
B<merchantability> or B<fitness for a particular purpose>.

=head1 COPYRIGHT

Copyright 2001 Sean M. Burke.

This library is free software; you can redistribute it and/or
modify it under the same terms as Perl itself.

=head1 AUTHOR

Sean M. Burke, E<lt>sburke@cpan.orgE<gt>

=head1 README

Translates crontab notation into English, for sanity checking: For
example, "10 8 15 * * foo bar" into: Run: foo bar with input
"baz\x0a" At: 8:10am on the 15th of every month

=head1 SCRIPT CATEGORIES

UNIX/System_administration

=head1 CHANGE LOG

=over

=item v0.71:  2001-07-29

Now supports the weird new Vixie-isms like "C<@annually>".

Features for feigning ignorance of non-POSIX features: C<-p>, C<-e>,
and the evironment variables C<POSIXLY_CORRECT> and/or
C<POSIX_ME_HARDER>.

=item v0.63:  2001-07-17

Fixed a bug spotted by Greg Wimpey, where leading whitespace wasn't
getting duly ignored.

=item v0.62:  2001-07-14

Added special cases for when minutes field is 0.

Added explicit "require" statement to ensure acceptable Perl version.

Changed "Every Tuesday of May" to "Every Tuesday in May"

Changed qr//'s to just plain strings, for all the 500404 dinosaurs.

=item v0.61 2001-01-23

First public release.

=back

=cut

use strict;
use integer;
use constant 'DEBUG' => 0;

my $filter = ''; # ...which is false.

# Lame switch processing:

my $posix; # whether to be just POSIX
while(@ARGV and $ARGV[0] =~ m<^->s) {
  if($ARGV[0] eq '--') { # end of switches.
    shift @ARGV;
    last;
  } elsif($ARGV[0] eq '-f') { # filter mode
    shift @ARGV;
    $filter = '#> '; # ...which is true!
  } elsif($ARGV[0] eq '-p') { # disable extensions
    shift @ARGV;
    $posix = 1;
  } elsif($ARGV[0] eq '-e') { # enable extensions
    shift @ARGV;
    $posix = 0;
  } elsif($ARGV[0] eq '-v') {
    print "crontab2english v$VERSION sburke\@cpan.org\n";
    exit;
  } else {
    die "Usage: crontab2english [-f] [files]\n"
      . "See 'perldoc crontab2english' for more info.\n";
  }
}

$posix = $ENV{'POSIXLY_CORRECT'} || $ENV{'POSIX_ME_HARDER'}
  unless defined $posix;

print $filter, " POSIX-only mode\n" if $posix;

my @lines;
if(@ARGV) {
  @lines = <>;
} elsif(-t *STDIN) {
  @lines = `crontab -l`;
} else {
  @lines = <STDIN>;
}

#--------------------------------------------------------------------------
# Build tables.
my @dows   = qw(Sun Mon Tue Wed Thu Fri Sat);
my @months = qw(Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec);
my($dow, $month, %dow2num, %month2num, %num2dow, %num2month);

my %mil2ampm;
@mil2ampm{0 .. 23}
  = ('midnight', map($_ . 'am', 1 .. 11), 'noon', map($_ . 'pm', 1 .. 11));

@dow2num{map lc($_), @dows} = (0 .. 6);
push @dows, 'Sun' unless $posix;
  # POSIX doesn't know about day 7
@num2dow{0 .. $#dows} = @dows;
DEBUG and print "DOWS: @dows\n";


@month2num{map lc($_), @months} = (1 .. 12);
@num2month{1 .. 12} = @months;
unshift @months, '';

{
  my $x = join '|', map quotemeta($_), @dows;
  $dow = "^($x)\$";    # regexp
  $x = join '|', map quotemeta($_), @months;
  $month = "^($x)\$";  # regexp
}

my(%num2month_long, %num2dow_long);
@num2month_long{1 .. 12} = qw(
  January February March April May June July August September October
  November December
);
@num2dow_long{0 .. 6} = qw(
 Sunday Monday Tuesday Wednesday Thursday Friday Saturday
);
$num2dow_long{7} = 'Sunday' unless $posix;

my $atom;
if($posix) {
  $atom = '\d+|(?:\d+-\d+)';           # will be a RE
  # Yes, POSIX allows no stepped ranges.
} else {
  $atom = '\d+|(?:\d+-\d+(?:/\d+)?)';  # will be a RE
}
my $atoms   = "^(?:$atom)(?:,$atom)*\$";    # well be a RE

print "Atoms RE: $atoms\n" if DEBUG;

my %atword = (  # for latter-day Vixie-isms
  'reboot'   => 'At reboot',
  'yearly'   => 'Yearly (midnight on January 1st)',
  'annually' => 'Yearly (midnight on January 1st)',
  'monthly'  => 'Monthly (midnight on the first of every month)',
  'weekly'   => 'Weekly (midnight every Sunday)',
  'daily'    => 'Daily, at midnight',
  'midnight' => 'Daily, at midnight',
  'hourly'   => 'At the top of every hour',
 # These are no longer documented in Vixie cron 3.0.  Why not?
);

#--------------------------------------------------------------------------

my $line_number = 0;

{
  my(@bits,$k,$v);
  foreach (@lines) {
    print $_ if $filter;
    chomp;
    DEBUG > 1 and print "Line: <$_>\n";
    ++$line_number;
    next if m/^[ \t]*#/s or m/^[ \t]*$/s;
    s/^[ \t]+//s; # "leading spaces and tabs are ignored"

    if(DEBUG > 1) {
      @bits = split m/[ \t]+/,$_,6;
      print "Bit count: ", scalar(@bits), ".\n";
    }

    # The POSIX cron spec doesn't seem to mention
    #  environment-setting lines at all!
    
    if(!$posix and m/^([^= \t]+)[ \t]*=[ \t]*\"(.*)\"[ \t]*$/s ) {
      # NAME = "VALUE"
      $k =~ s/[ \t]+$//;
      $filter or print "Setting env var $k to \"$v\"\n";
    } elsif(!$posix and m/^([^= \t]+)[ \t]*=[ \t]*\'(.*)\'[ \t]*$/s ) {
      # NAME = 'VALUE'
      ($k,$v) = ($1,$2);
      #$k =~ s/[ \t]+$//;
      $filter or print "Setting env var $k to \'$v\'\n";
    } elsif(!$posix and m/^([^= \t]+)[ \t]*=(.*)/s ) {
      # NAME = VALUE
      ($k,$v) = ($1,$2);
      #$k =~ s/[ \t]+$//;
      $v =~ s/^[ \t]+//;
      $filter or print "Setting env var $k to $v\n\n";
    } elsif(!$posix and m/^\@(\w+)[ \t]+(.*)/s and exists $atword{lc $1}) {
      process_command($_, $atword{lc $1}, $2);
    } elsif( (@bits = split m/[ \t]+/, $_, 6) and @bits == 6 ) {
      DEBUG and print "Bits: ", map("<$_> ", @bits), "\n";
      process_command($_, @bits);
    } else {
      if($filter) {
        print $filter, "UNPARSEABLE LINE?!\n";
      } else {
        print "Unparseable line (#$line_number): \"", esc($_), "\"\n";
      }
    }
  }
  exit;
}

#--------------------------------------------------------------------------
sub process_command {
  # 0 m,   1 h,   2 day-of-month,  3 month,  4 dow
  my $line = shift;

  my(@time_lines, $command_string);
  if(@_ == 2) { # hack for funky vixieism
    $command_string = $_[1];
    @time_lines = ($_[0]);
  } else {
    # a normal line -- expand and Englishify it
    my(@bits) = expand_time_bits(@_);

    if(@bits == 1) { # signals error condition
      my $x = $bits[0];
      if($filter) {
        print $filter,
          "Unparseable ", @$x == 1 ? 'bit' : 'bits',
         ": ", join(' ', map "\"$_\"", @$x), "\n"
        ;
      } else {
        print
          "Unparseable ", @$x == 1 ? 'bit' : 'bits',
          " in parsing of command ${$x}[-1] at line number $line_number:\n",
          map("  $_\n", @$x), "\n"
        ;
      }
      return;
    }
  
    @time_lines = bits_to_english(@bits);
    $time_lines[0] = ucfirst($time_lines[0]);
    if(length(join '    ', @time_lines) <= 75) {
      @time_lines = (join '    ', @time_lines);
    }
    for(@time_lines) { $_ = ' ' . $_ }; # indent over
    $time_lines[0] = "At:" . $time_lines[0];
    $command_string = pop @bits;
  }
 
  my @command = split( "\n", percent_proc($command_string), -1 );
  
  if(@command) {
    pop @command if @command == 2 and $command[1] eq '';
     # Eliminate mention of basically null input
  } else {
    push @command, '';
  }
  
  if(@command > 1) {
    my $x = join "\n", splice @command, 1;
    push @command, " with input \"" . esc($x) . "\"";
  }
  if($command[0] =~ m<^\*>s) {
    push @command, " (Do you really mean the command to start with \"*\"?)";
  } elsif($command[0] eq '') {
    push @command, " (Do you really mean to run a null command?)";
  }
  $command[0] = "Run: $command[0]";
  
  if($filter) {
    print
      map("$filter $_\n",
          (@command == 1) ? () : (@command), # be concise for simple cases
          @time_lines
         ),
    ;
  } else {
    print
      #was: "Command: (line $line_number) $line\n",
      # but that's awful verbose
      "Command: (line $line_number)\n",
      map("  $_\n", @command, @time_lines ), "\n";
  }
  
  return;
}

#--------------------------------------------------------------------------
sub expand_time_bits {
  my @bits = @_;
  my @unparseable;

  # 0 m,   1 h,   2 day-of-month,  3 month,  4 dow
  
  unless($posix) {
    if($bits[3] =~ m/($month)/oi) { $bits[3] = $month2num{lc $1} }
    if($bits[4] =~ m/($dow)/oi  ) { $bits[4] =   $dow2num{lc $1} }
  }

  for(my $i = 0; $i < 5 ; ++$i) {
    my @segments;
    if($bits[$i] eq '*') {
      push @segments, ['*'];
    } elsif(!$posix and $bits[$i] =~ m<^\*/(\d+)$>s) {
      # a hack for "*/3" etc
      push @segments, ['*', 0 + $1];
    } elsif($bits[$i] =~ m/$atoms/ois) {
      foreach my $thang (split ',', $bits[$i]) {
        if($thang =~ m<^(?:(\d+)|(?:(\d+)-(\d+)(?:/(\d+))?))$>s) {
          if(defined $1) {
            push @segments, [0 + $1]; # "7"
          } elsif(defined $4) {
            push @segments, [0 + $2, 0 + $3, 0 + $4];  # "3-20/4"
          } else {
            push @segments, [0 + $2, 0 + $3]; # "3-20"
          }
        } else {
          warn "GWAH? thang \"$thang\"";
        }
      }
    } else {
      push @unparseable, sprintf "field %s: \"%s\"", $i + 1, esc($bits[$i]);
      next;
    }
    
    $bits[$i] = \@segments;
  }
  return \@unparseable if @unparseable;
  return @bits;
}

#--------------------------------------------------------------------------

sub bits_to_english {
  # This is the deep ugly scary guts of this program.
  # The older and eldritch among you might recognize this as sort of a
  # parody of bad old Lisp style of data-structure handling.

  my @bits = @_;
  my @time_lines;
  
  { # gratuitous block.
    
    # Render the minutes and hours ########################################
    if(@{$bits[0]}    == 1   and @{$bits[1]}    == 1 and 
       @{$bits[0][0]} == 1   and @{$bits[1][0]} == 1 and 
       $bits[0][0][0] ne '*' and $bits[1][0][0] ne '*'
       # It's a highly simplifiable time expression!
       #  This is a very common case.  Like "46 13" -> 1:46pm
       #  Formally: when minute and hour are each a single number.
    ) {
      my $h = $bits[1][0][0];
      if($bits[0][0][0] == 0) {
	# Simply at the top of the hour, so just call it by the hour name.
	push @time_lines, $mil2ampm{$h};
      } else {
	# Can't say "noon:02", so use an always-numeric time format:
	push @time_lines, sprintf '%s:%02d%s',
	    ($h > 12) ? ($h - 12) : $h,
	    $bits[0][0][0],
	    ($h >= 12) ? 'pm' : 'am';
      }
      $time_lines[-1] .= ' on';

    } else {    # It's not a highly simplifiable time expression
      
      # First, minutes:
      if($bits[0][0][0] eq '*') {
        if(1 == @{$bits[0][0]} or $bits[0][0][1] == 1) {
          push @time_lines, 'every minute of';
        } else {
          push @time_lines, 'every ' . freq($bits[0][0][1]) . ' minute of';
        }
        
      } elsif( @{$bits[0]} == 1 and $bits[0][0][0] == 0 ) {
        # It's just a '0'.  Ignore it -- instead of bothering
	# to add a "0 minutes past"
      } elsif( !grep @$_ > 1, @{$bits[0]} ) {
        # it's all like 7,10,15.  conjoinable
        push @time_lines, conj_and(map $_->[0], @{$bits[0]}) . (
          $bits[0][-1][0] == 1 ? ' minute past' : ' minutes past' );
      } else { # it's just gonna be long.
        my @hunks;
        foreach my $bit (@{$bits[0]}) {
          if(@$bit == 1) {   #"7"
            push @hunks, $bit->[0] == 1 ? '1 minute' : "$bit->[0] minutes";
          } elsif(@$bit == 2) { #"7-9"
            push @hunks, sprintf "from %d to %d %s", @$bit,
              $bit->[1] == 1 ? 'minute' : 'minutes';
          } elsif(@$bit == 3) { # "7-20/2"
            push @hunks, sprintf "every %d %s from %d to %d",
              $bit->[2],
              $bit->[2] == 1 ? 'minute' : 'minutes',
              $bit->[0], $bit->[1],
            ;
          }
        }
        push @time_lines, conj_and(@hunks) . ' past';
      }
      
      # Now hours
      if($bits[1][0][0] eq '*') {
        if(1 == @{$bits[1][0]} or $bits[1][0][1] == 1) {
          push @time_lines, 'every hour of';
        } else {
          push @time_lines, 'every ' . freq($bits[1][0][1]) . ' hour of';
        }
      } else {
        my @hunks;
        foreach my $bit (@{$bits[1]}) {
          if(@$bit == 1) {   # "7"
            push @hunks, $mil2ampm{$bit->[0]} || "HOUR_$bit->[0]??";
          } elsif(@$bit == 2) { # "7-9"
            push @hunks, sprintf "from %s to %s",
              $mil2ampm{$bit->[0]} || "HOUR_$bit->[0]??",
              $mil2ampm{$bit->[1]} || "HOUR_$bit->[1]??",
          } elsif(@$bit == 3) { # "7-20/2"
            push @hunks, sprintf "every %d %s from %s to %s",
              $bit->[2],
              $bit->[2] == 1 ? 'hour' : 'hours',
              $mil2ampm{$bit->[0]} || "HOUR_$bit->[0]??",
              $mil2ampm{$bit->[1]} || "HOUR_$bit->[1]??",
          }
        }
        push @time_lines, conj_and(@hunks) . ' of';
      }
      # End of hours and minutes
    }

    # Day-of-month ########################################################
    if($bits[2][0][0] eq '*') {
      $time_lines[-1] =~ s/ on$//s;
      if(1 == @{$bits[2][0]} or $bits[2][0][1] == 1) {
        push @time_lines, 'every day of';
      } else {
        push @time_lines, 'every ' . freq($bits[2][0][1]) . ' day of';
      }
    } else {
      my @hunks;
      foreach my $bit (@{$bits[2]}) {
        if(@$bit == 1) {   # "7"
          push @hunks, 'the ' . ordinate($bit->[0]);
        } elsif(@$bit == 2) { # "7-9"
          push @hunks, sprintf "from the %s to the %s",
            ordinate($bit->[0]), ordinate($bit->[1]),
        } elsif(@$bit == 3) { # "7-20/2"
          push @hunks, sprintf "every %d %s from the %s to the %s",
            $bit->[2],
            $bit->[2] == 1 ? 'day' : 'days',
            ordinate($bit->[0]), ordinate($bit->[1]),
        }
      }
      
      # collapse the "the"s, if all the elements have one
      if(@hunks > 1 and !grep !m/^the /s, @hunks) {
        for (@hunks) { s/^the //s; }
        $hunks[0] = 'the '. $hunks[0];
      }
      
      push @time_lines, conj_and(@hunks) . ' of';
    }

    # Month ###############################################################
    if($bits[3][0][0] eq '*') {
      if(1 == @{$bits[3][0]} or $bits[3][0][1] == 1) {
        push @time_lines, 'every month';
      } else {
        push @time_lines, 'every ' . freq($bits[3][0][1]) . ' month';
      }
    } else {
      my @hunks;
      foreach my $bit (@{$bits[3]}) {
        if(@$bit == 1) {   # "7"
          push @hunks, $num2month_long{$bit->[0]} || "MONTH_$bit->[0]??"
        } elsif(@$bit == 2) { # "7-9"
          push @hunks, sprintf "from %s to %s",
            $num2month_long{$bit->[0]} || "MONTH_$bit->[0]??",
            $num2month_long{$bit->[1]} || "MONTH_$bit->[1]??",
        } elsif(@$bit == 3) { # "7-20/2"
          push @hunks, sprintf "every %d %s from %s to %s",
            $bit->[2],
            $bit->[2] == 1 ? 'month' : 'months',
            $num2month_long{$bit->[0]} || "MONTH_$bit->[0]??",
            $num2month_long{$bit->[1]} || "MONTH_$bit->[1]??",
        }
      }
      push @time_lines, conj_and(@hunks);
      
      # put in semicolons in the case of complex constituency
      #if($time_lines[-1] =~ m/every|from/) {
      #  $time_lines[-1] =~ tr/,/;/;
      #  s/ (and|or)\b/\; $1/g;
      #}
    }
    
    
    # Weekday #############################################################
   #
  #
 #
#
# From man 5 crontab:
#   Note: The day of a command's execution can be specified by two fields
#   -- day of month, and day of week.  If both fields are restricted
#   (ie, aren't *), the command will be run when either field matches the
#   current time.  For example, "30 4 1,15 * 5" would cause a command to
#   be run at 4:30 am on the 1st and 15th of each month, plus every Friday.
#
# [But if both fields ARE *, then it just means "every day".
#  and if one but not both are *, then ignore the *'d one --
#  so   "1 2 3 4 *" means just 2:01, April 3rd
#  and  "1 2 * 4 5" means just 2:01, on every Friday in April
#  But  "1 2 3 4 5" means 2:01 of every 3rd or Friday in April. ]
#
 #
  #
   #
    # And that's a bit tricky.
    
    if($bits[4][0][0] eq '*' and (
      @{$bits[4][0]} == 1 or $bits[4][0][1] == 1
     )
    ) {
      # Most common case -- any weekday.  Do nothing really.
      #
      #   Hm, does "*/1" really mean "*" here, given the above note?
      #
      
      # Tidy things up while we're here:
      if($time_lines[-2] eq "every day of" and
         $time_lines[-1] eq 'every month'
      ) {
        $time_lines[-2] = "every day";
        pop @time_lines;
      }
      
    } else {
      # Ugh, there's some restriction on weekdays.
      
      # Translate the DOW-expression
      my $expression;
      my @hunks;
      foreach my $bit (@{$bits[4]}) {
        if(@$bit == 1) {
          push @hunks, $num2dow_long{$bit->[0]} || "DOW_$bit->[0]??";
        } elsif(@$bit == 2) {
          if($bit->[0] eq '*') { # it's like */3
            #push @hunks, sprintf "every %s day of the week", freq($bit->[1]);
            #  the above was ambiguous -- "every third day of the week"
            #  sounds synonymous with just "3"
            if($bit->[1] eq 2) {
              # common and unambiguous case.
              push @hunks, "every other day of the week";
            } else {
              # rare cases: N > 2
              push @hunks, "every $bit->[1] days of the week";
               # sounds clunky, but it's a clunky concept
            }
          } else {
            # it's like "7-9"
            push @hunks, sprintf "%s through %s",
              $num2dow_long{$bit->[0]} || "DOW_$bit->[0]??",
              $num2dow_long{$bit->[1]} || "DOW_$bit->[1]??",
          }
        } elsif(@$bit == 3) { # "7-20/2"
          push @hunks, sprintf "every %s %s from %s through %s",
            ordinate_soft($bit->[2]), #$bit->[2],
            'day',               #$bit->[2] == 1 ? 'days' : 'days',
            $num2dow_long{$bit->[0]} || "DOW_$bit->[0]??",
            $num2dow_long{$bit->[1]} || "DOW_$bit->[1]??",
        }
      }
      $expression = conj_or(@hunks);
      
      # Now figure where to put it...
      #
      if($time_lines[-2] eq "every day of") {
        # Unrestricted day-of-month, hooray.
        #
        if($time_lines[-1] eq 'every month') {
          # change it to "every Tuesday", killing the "of every month".
          $time_lines[-2] = "every $expression";
          $time_lines[-2] =~ s/every every /every /g;
          pop @time_lines;
        } else {
          # change it to "every Tuesday in"
          $time_lines[-2] = "every $expression in";
          $time_lines[-2] =~ s/every every /every /g;
        }
      } else {
        # This is the messy case where there's a DOM and DOW
        #  restriction
        
        # Was, wrongly:
        #  $time_lines[-1] .= ',';
        #  push @time_lines, "if it's also " . $expression;
        
        $time_lines[-2] .= " -- or every $expression in --";
         # Yes, dashes look very strange, but then this is a very
         # rare case.
        $time_lines[-2] =~ s/every every /every /g;
      }
    }
    #######################################################################
  }
    # TODO: change "3pm" -> "the 3pm hour" or something?
  $time_lines[-1] =~ s/ of$//s;
  
  return @time_lines;
}

###########################################################################
# Just utility routines below here.

my %pretty_form;
BEGIN {
  %pretty_form = ( '"' => '\"',  '\\' => '\\\\', );
}

sub esc {
  my $x = $_[0];
  $x =~
   #s<([^\x20\x21\x23\x27-\x3F\x41-\x5B\x5D-\x7E])>
   s<([\x00-\x1F"\\])>
    <$pretty_form{$1} || '\\x'.(unpack("H2",$1))>eg;
  return $x;
}

#--------------------------------------------------------------------------

#      if($time_lines[-1] =~ m/every|from/) {
#        $time_lines[-1] =~ tr/,/;/;
#        s/ (and|or)\b/\; $1/g;
#      }

sub conj_and {
  if(grep m/every|from/, @_) {
    # put in semicolons in the case of complex constituency
    return join('; and ', @_) if @_ < 2;
    my $last = pop @_;
    return join('; ', @_) . '; and ' . $last;
  }
  
  return join(' and ', @_) if @_ < 3;
  my $last = pop @_;
  return join(', ', @_) . ', and ' . $last;
}

sub conj_or {
  if(grep m/every|from/, @_) {
    # put in semicolons in the case of complex constituency
    return join('; or ', @_) if @_ < 2;
    my $last = pop @_;
    return join('; ', @_) . '; or ' . $last;
  }
  
  return join(' or ', @_) if @_ < 3;
  my $last = pop @_;
  return join(', ', @_) . ', or ' . $last;
}

#--------------------------------------------------------------------------

my %ordinations;
BEGIN {
  # English-language overrides for common ordinals
  %ordinations = qw(
    1 first 2 second 3 third 4 fourth 5 fifth 6 sixth
    7 seventh 8 eighth 9 ninth 10 tenth
  );
  #  11 eleventh 12 twelfth
  #  13 thirteenth 14 fourteen 15 fifteenth 16 sixteenth
  #  17 seventeenth 18 eighteenth 19 nineteenth 20 twentieth
}

sub ordsuf  {
  return 'th' if not(defined($_[0])) or not( 0 + $_[0] );
   # 'th' for undef, 0, or anything non-number.
  my $n = abs($_[0]);  # Throw away the sign.
  return 'th' unless $n == int($n); # Best possible, I guess.
  $n %= 100;
  return 'th' if $n == 11 or $n == 12 or $n == 13;
  $n %= 10;
  return 'st' if $n == 1; 
  return 'nd' if $n == 2;
  return 'rd' if $n == 3;
  return 'th';
}

sub ordinate  {
  my $i = $_[0] || 0;
  $ordinations{$i} || ($i . ordsuf($i));
}

sub freq {
  # frequentive form.  Like ordinal, except that 2 -> 'other'
  #  (as in every other)
  my $i = $_[0] || 0;
  return 'other' if $i == 2;  # special case
  $ordinations{$i} || ($i . ordsuf($i));
}

sub ordinate_soft  {
  my $i = $_[0] || 0;
  $i . ordsuf($i);
}

#--------------------------------------------------------------------------

sub percent_proc {
  # Translated literally from the C, cron/do_command.c.
  my($esc,$need_newline);
  my $out = '';
  my $c;
  for(my $i = 0; $i < length($_[0]); $i++) {
    $c = substr($_[0],$i,1);
    if($esc) {
      $out .= "\\" unless $c eq '%';
    } else {
      $c = "\n" if $c eq '%';
    }
    unless($esc = ($c eq "\\")) {
      # For unescaped characters,
      $out .= $c;
      $need_newline = ($c ne "\n");
    }
  } 
  $out .= "\\" if $esc;
  $out .= "\n" if $need_newline;
  return $out;
  
  # I think this would do the same thing:
  #  $x =~ s/((?:\\\\)+)  |  (\\\%)  |  (\%)  /$3 ? "\n" : $2 ? '%'   : $1/xeg;
  # But I don't want to think about it, and I need it to work just
  #  as the original does.
}

#--------------------------------------------------------------------------

__END__

# Test data for crontab parsing:

  MAILTO=hulahoops@polygon.int
10 8 15 * * /bin/csh -c 'perl ~/thang.pl | mail -s hujambo root'

40 5 * * * df -k

  50 6 * * 1 ls -l /tmp

1 2 * apr mOn foo


foo=bar
  foo=bar 
 foo = "bar"
 foo="bar"
	foo = 'bar'  
 foo='bar'

  1 2 3 4 7 Foo
1-20/3 * * * * Foo
 1,2,3 * * * * Foo
1-9,15-30 * * * * Foo
   1-9/3,15-30/4 * * * * Foo

  1 2 3 jan mon stuff
  1 2 3 4 mON stuff
  1 2 3 jan 5 stuff

              @reboot        xxstartuply foo
              @yearly        xxx yearlijk%thig%hooboy
              @annually      xxannuallijk heehoo
              @monthly       xxx monthlijk
              @weekly        xXxX weeklijk
              @daily         xxxdaylijk
              @midnight      xxxmidnightlijk
              @hourly        xXxXhourlijk


*/3 * * * * stuff


#End, really.
