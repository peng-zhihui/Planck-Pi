#!/usr/bin/perl -w
#
# Generate stats of Cron messages sent to syslog
#
#
# This script can be used to generate per-user and per-task 
# statistics in order to determine how much time do cron tasks take
# and audit cron use. It might be useful to debug issues related
# to cron tasks that take too much time to run, or overlap, or get executed 
# too fast..
#
# Hint: In some systems, cron tasks might not properly use lockfiles to
# prevent themselves from being run twice in a row and some "strange"
# things might happen if the system is overloaded and a cron task takes
# more than expected. And, no, this is not something that Cron should
# prevent (for more information see Debian bug #194805).
#
# In order for this script to work 
#
# together with the logging enhancements included
# in Debian's vixie cron (3.0pl1-95) that make it possible to log 
# when do cron tasks end.
# 
#
# How to use:
# - Modify /etc/init.d/cron so that the calls to cron pass the '-L 2'
#   argument
#   (Hint: change lines 27 and 47 so that they end with '-- -L 2 $LSBNAMES'
#   instead of '-- $LSBNAMES')
#   In Debian you need cron-3.0pl1-95 or later.
#   Note: future versions of cron might use an /etc/default/cron file
#   to provide arguments to cron. 
#
# - Review /etc/syslog.conf to determine where does the output of the cron
#   facility goes to (might be /var/log/syslog together with other messages
#   but you can also configure syslog to store them at /var/log/cron.log)
#
# - Run the script (by default it will read input /var/log/syslog but you
#   can point to other file using the '-f' switch) and review the output
#
# - (Optionally) If you want to analyse log files for a long period
#   concatenate different log files, extract all CRON related entries
#   to a file and process that.
#  
#   You could do, for example, this:
#
#   zcat -f /var/log/syslog* | grep CRON >cron-log ;  \
#              perl stats-cron.pl -f cron-log
#   
#
# This program is copyright 2006 by Javier Fernandez-Sanguino <jfs@debian.org>
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program; if not, write to the Free Software
#    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
# For more information please see
#  http://www.gnu.org/licenses/licenses.html#GPL

# TODO:
# - Print time internal of file analysis (from 'XXX' to 'XXX') based on the
#   first and the last log entry read.
# 
# - Detect overlaped entries for the same task (start of an entry before the
#   previous one finished)
#
# - Make it possible to filter by users
#
# - Consider adapting to other log formats (other cron programs? Solaris?) by
#   separating analysis and extraction in the code and making it possible
#   to switch to different analysis methods.


# Required modules
eval 'use Parse::Syslog';
if ($@) {
	print STDERR "ERROR: Parse::Syslog not installed, please install this Perl library\n";
	print STDERR "ERROR: (in Debian it is provided by the 'libparse-syslog-perl' package)\n";
	exit 1;
}
use Parse::Syslog;
use Getopt::Std;
# Parse command line
getopts('dhvf:');
$opt_d = 0 if ! defined ($opt_d) ;

if ( $opt_h ) {
	print "Usage: $ARGV[0] [-dhv] [-f syslog file]";
	exit 1;
}

# Note: in Debian's default syslog configuration log messages are 
# sent to /var/log/syslog but you might have edited it to use
# /var/log/cron.log instead. Which, BTW, is more efficient because
# we do not need to parse the syslog to extract only cron messages.
my $LOGFILE = $opt_f || "/var/log/syslog";

# Sanity checks
if ( ! -r $LOGFILE ) {
	print STDERR "ERROR: Cannot read logfile $LOGFILE";
}

my $parser = Parse::Syslog->new( $LOGFILE);
print STDERR "Starting parse of file $LOGFILE\n" if $opt_v;
while(my $log = $parser->next) {
	if ( $log->{program} =~ /CRON/ ) {
		print STDERR "DEBUG: Found syslog entry: $log->{pid}\n" if $opt_d;
# We extract the entries with the same text but with 'CMD' (start)
# and 'END' (end) to check if it's the same we use the PID reported
# by cron.
		if ($log->{text} =~ /CMD \(/) {
			my $pid;
			if  ($log->{text} =~ /\(\[(\d+)\]/ ) {
			# Debian - with loglevel 8, the child process id is
			# reported
				$pid = $1;
			} else {
			# All the other cases, the process ID of the
			# child process is the same one as the log entry
				$pid = $log->{pid};
			}
			add_entry($log->{timestamp},$log->{text},$pid, $log->{pid})
		}
		if ($log->{text} =~ /END \(/) {
			my $pid;
			if  ($log->{text} =~ /\(\[(\d+)\]/ ) {
			# Debian - with loglevel 8
				$pid = $1;
			} else {
			# Other cases
				$pid = "UNKNOWN";
			}
			clear_entry($log->{timestamp},$log->{text},$pid,$log->{pid});
		}
	}
}
print STDERR "Finished parse of file $LOGFILE\n" if $opt_v;


# Analysis, we find all entries with a start and stop timestamp 
# and print them
generate_analysis();

# Stats:
# - average time of execution of cron jobs
# - cronjob which executed slowest
# - cronjob which executed fastes
# - cronjobs which start before they finish
print_stats();

exit 0;

sub find_user {
# Extract a user from a cron entry
	my ($text) = @_;
	my $user = "";
	if ( $text =~ /^\((.*?)\)/ ) {
		$user = $1;
	} 
	return $user;
}

sub find_prg {
# Extract a program from a cron entry
	my ($text) = @_;
	my $prg = "";
	if ( $text =~ /(CM|EN)D\s*\((.*)\s*\)\s*$/ ) {
		$prg = $2;
		$prg =~ s/^\[\d+\]\s+//; # Strip off child process numbers
					# (Debian loglevel 8)
	}
	return $prg;
}

sub add_entry {
# Get a syslog entry and add it to the table of entries for analysis
	my ($time, $text, $pid, $ppid) = @_;
	my ($user, $prg);
	print STDERR "DEBUG: Entering add_entry with @_\n" if $opt_d; 
	$user = find_user($text);
	if ( ! defined ($user) ) {
		print STDERR "WARN: Cannot find user for cron job (pid $pid)\n";
		return;
	}
	$prg  = find_prg($text);
	if ( ! defined ($prg) ) {
		print STDERR "WARN: Cannot find program for cron job (pid $pid)\n";
		return;
	}
	my $value = $pid."-".$time;
	print STDERR "DEBUG: Adding cronjob of user $user to list: $prg\n" if $opt_d;
	add_cron_entry($user, $prg, $value, $ppid);
	$cron_entries{$user}{$prg}{'count'}++;
	return;
}

sub clear_entry {
# Get a syslog entry, find the start entry and add the timestamp difference
# to the list of timestamps for that user
#
# There's two ways to match entries:
#  - (in Debian, using loglevel 8): By matching the child process ID
#    between the entries
#  - in all the other cases: By matching the process ID of the 
#    END message (which belongs to the parent) with the process IF
#    of the CMD message (which belongs to the child)
	my ($timestamp, $text, $pid, $ppid) = @_;
	my ($user, $prg, $entry, $count);
	my @array; my @stack;
	print STDERR "DEBUG: Entering clear_entry with @_\n" if $opt_d; 
	$user = find_user($text);
	if ( $user eq "" ) {
		print STDERR "WARN: Cannot find user for cron job (pid $pid)\n";
		return;
	}
	$prg  = find_prg($text);
	if ( $prg eq "" ) {
		print STDERR "WARN: Cannot find program for cron job (pid $pid)\n";
		return;
	}
	if ( ! defined ( $cron_entries{$user}{$prg}{'item'} ) ) {
		print STDERR "WARN: End entry without start entry (pid $pid)\n";
		return;
	}
	@array = split(':', $cron_entries{$user}{$prg}{'item'});
	$count = $#array + 1 ;
	print STDERR "DEBUG: $count cron entries for $user, task '$prg'\n" if $opt_d;
	my $finish = 0;
	while ( $finish == 0 ) {
		$entry = pop @array;
		last if ! defined ($entry) ;
		# Pop all entries and look for one whose pid matches 
		my ($spid, $stimestamp) =  split ("-", $entry);
		print  STDERR "DEBUG: Comparing entry $spid to $pid\n" if $opt_d;
		if  ( ( $pid ne "UNKNOWN" && $pid == $spid ) ||  $ppid-$spid == 1 ) {
			my $timediff =  $timestamp-$stimestamp;
			$timediff = 0 if $timediff < 0;
			print STDERR "DEBUG: Entries match, time difference of $timediff\n" if $opt_d ;
			$cron_entries{$user}{$prg}{'finished'}++;
			if (defined ( $cron_times{$user}{$prg} ) ) { 
				$cron_times{$user}{$prg} = join(":", $cron_times{$user}{$prg}, $timediff);
			} else {
				$cron_times{$user}{$prg} = $timediff;
			}
			$finish = 1;
		} else {
			print STDERR "DEBUG: Pushing entry $spid into stack\n" if $opt_d;
			push @stack, $entry;
		}
	}
	# Push all remaining entries to the stack
	$cron_entries{$user}{$prg}{'item'}="";
	$count = $#array + 1 ;
	if ($opt_d) {
		print STDERR "DEBUG: Restoring all entries (size: array $count";
		$count = $#stack + 1 ;
		print STDERR ", stack $count)\n";
	}
	print STDERR "DEBUG: Total finished tasks: $cron_entries{$user}{$prg}{'finished'} out of $cron_entries{$user}{$prg}{'count'}\n" if defined $cron_entries{$user}{$prg}{'finished'} && $opt_d;
	print STDERR "DEBUG: Unmatched now: $cron_entries{$user}{$prg}{'item'}\n" if $opt_d;
	while ( $entry = pop @array ) {
		add_cron_entry($user, $prg, $entry);
	}
	while ( $entry = pop @stack ) {
		add_cron_entry($user, $prg, $entry);
	}
	print STDERR "DEBUG: Finish execution of clear_entry\n" if $opt_d;
	return;
}

sub add_cron_entry {
	my ($user, $prg, $entry) = @_;
	if ( defined ($cron_entries{$user}{$prg}) &&  $cron_entries{$user}{$prg} ne "" ) { 
		$cron_entries{$user}{$prg}{'item'} = join(":", $cron_entries{$user}{$prg}{'item'}, $entry);
	} else {
		$cron_entries{$user}{$prg}{'item'} = $entry;
		$cron_entries{$user}{$prg}{'count'} = 0;
		$cron_entries{$user}{$prg}{'finished'} = 0;
		$cron_entries{$user}{$prg}{'unmatched'} = 0 ;
		$cron_entries{$user}{$prg}{'average'} = 0 ;
		$cron_entries{$user}{$prg}{'minimum'} = 0 ;
		$cron_entries{$user}{$prg}{'maximum'} = 0;
	}
}

sub count_unmatched {
	my ($user, $prg) = @_;
	my ($count, @array);
	return 0 if ! defined ( $cron_entries{$user}{$prg}{'item'} ); 
	@array = split(':', $cron_entries{$user}{$prg}{'item'});
	$count = $#array + 1 ;
	return $count;
}

sub find_average {
	my ($user, $prg) = @_;
	my ($average, $count, $total, @array, $entry);
	$total = 0 ;
	return -1 if ! defined ( $cron_times{$user}{$prg} ); 
	@array = split(':', $cron_times{$user}{$prg});
	$count = $#array + 1 ;
	while ( defined ( $entry = pop @array ) ) {
		$total += $entry;
	}
	$average = $total / $count;
	return $average;
}

sub find_minimum {
	my ($user, $prg) = @_;
	my ($minimum, @array, $entry);
	$minimum = -1;
	return -1 if ! defined ( $cron_times{$user}{$prg} ); 
	@array = split(':', $cron_times{$user}{$prg});
	while ( defined ( $entry = pop @array ) ) {
		if ( $minimum == -1 ) {
			$minimum = $entry;
		} else {
			$minimum = $entry if $entry < $minimum;
		}
	}
	return $minimum;
}

sub find_maximum {
	my ($user, $prg) = @_;
	my ($maximum, @array);
	$maximum = -1;
	return -1 if ! defined ( $cron_times{$user}{$prg} ); 
	@array = split(':', $cron_times{$user}{$prg});
	while ( defined ( $entry = pop @array ) ) {
		if ( $maximum == -1 ) {
			$maximum = $entry ;
		} else { 
			$maximum = $entry if $entry > $maximum;
		}
	}
	return $maximum;
}



sub generate_analysis {
# For each user and program calculate the average time for the task
	my ($user, $prg);
	foreach $user (keys %cron_entries) {
		print STDERR "DEBUG: Calculating data for user '$user'\n" if $opt_d;
		foreach my $prg ( keys %{$cron_entries{$user}} ) {
			print STDERR "DEBUG: Calculating data for task '$prg'\n" if $opt_d;
			my $unmatched = count_unmatched($user, $prg);
			my $average = find_average($user, $prg);
			my $minimum = find_minimum($user, $prg);
			my $maximum = find_maximum($user, $prg);
			$cron_entries{$user}{$prg}{'unmatched'} = $unmatched;
			$cron_entries{$user}{$prg}{'average'} = $average;
			$cron_entries{$user}{$prg}{'minimum'} = $minimum;
			$cron_entries{$user}{$prg}{'maximum'} = $maximum;
		}
	}
}

sub print_stats {
# Print information of cron statistics
	my ($user, $prg);
	foreach  $user (keys %cron_entries) {
		print "Cron statistics for user '$user'\n";
		foreach $prg ( keys %{$cron_entries{$user}} ) {
			print "\tTask: '$prg'\n";
			print "\t\tDEBUG: $cron_times{$user}{$prg}\n" if $opt_d;
			print <<EOF
\t\tTotal: $cron_entries{$user}{$prg}{'count'}
\t\tFinished: $cron_entries{$user}{$prg}{'finished'}
\t\tUnmatched: $cron_entries{$user}{$prg}{'unmatched'}
\t\tAvg time: $cron_entries{$user}{$prg}{'average'}
\t\tMin time: $cron_entries{$user}{$prg}{'minimum'}
\t\tMax time: $cron_entries{$user}{$prg}{'maximum'}
EOF
		}
	}
}
