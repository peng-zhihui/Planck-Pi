#!/usr/bin/perl -w
# Generate an /etc/network/interfaces script based on the
# current interface and network status.
# Useful to migrate the configuration of old Debian versions (i.e. pre-woody)
# or non-Debian systems to the ifup/down scheme used in Debian.
#
# (c) 2000 Anthony Towns 
# slight improvements (route parsing and direct command
# execution) done by Javier Fernandez-Sanguino 
# 
# TODO:
# [aj] It'd be good if it also grabbed the default gateway from route,
# and worked out the network based on the netmask and the address
# (the network is needed for ifup under 2.0.x kernels).
#
# [jfs] Some (optional) information is not parsed, like: route metrics
# and hw addresses of interfaces


use strict;

my %iface = ();  # name -> iface info hash
my $ciface;  # current iface name

# First, read interfaces from ifconfig
#
open (IFC,"ifconfig -a | ") || die ("Could not execute ifconfig: $!\n");

while(my $line = <IFC>) {
    chomp $line;
    if ($line =~ m/^(\S+)\s+(\S.*)$/) {
        $ciface = $1;
	$iface{$ciface} = { };
        $line = $2;
    } elsif ($line =~ m/^\s+(\S.*)$/) {
        $line = $1;
    } else {
        $ciface = undef;
        next;
    }
    next unless(defined $ciface);

    if ($line =~ s/Link encap:(.*)$//) {
        $iface{$ciface}->{"type"} = $1;
    }
    if ($line =~ s/^inet //) {
        $iface{$ciface}->{"ipv4"} = "yes";
        if ($line =~ s/addr:(\S*)//) {
            $iface{$ciface}->{"ipv4_addr"} = $1;
        }
        if ($line =~ s/Bcast:(\S*)//) {
            $iface{$ciface}->{"ipv4_bcast"} = $1;
        }
        if ($line =~ s/Mask:(\S*)//) {
            $iface{$ciface}->{"ipv4_mask"} = $1;
        }
    }
}

close IFC;

# Now, read route information from netstat
#
open (ROU,"route -n | ") || die ("Could not execute route: $!\n");
while(my $line = <ROU>) {
    chomp $line;
    if ( $line =~ m/^([\d\.]+)\s+([\d\.]+)\s+([\d\.]+)\s+(\S+)\s+(\d+).*?\s+(\S+)$/) {
    	my $dest = $1; my $gw = $2; my $mask = $3; my $flags = $4;
	my $metric = $5; my $if = $6;
	if ( defined ( $iface{$if} ) ) {
		if ( $dest eq "0.0.0.0" && $mask eq "0.0.0.0" ) {
		# Default gateway
			$iface{$if}->{"gateway"} = $gw;
	        } elsif ( $gw ne "0.0.0.0" )  {
		# Specific (static) route
			push @{$iface{$if}->{"up"}} , "route add -net $dest netmask $mask gw $gw dev $if";
			push @{$iface{$if}->{"down"}} , "route del -net $dest netmask $mask gw $gw dev $if";
		}
	}
    }

}

close ROU;

foreach my $if (keys %iface) {
    if ($iface{$if}->{"type"} =~ m/loopback/i) {
        if ($iface{$if}->{"ipv4"} eq "yes") {
            print "iface $if inet loopback\n";
        }
    }

    if ($iface{$if}->{"type"} =~ m/ethernet/i) {
        if ($iface{$if}->{"ipv4"}) {
            print "iface $if inet static\n";
            if (defined $iface{$if}->{"ipv4_addr"}) {
                print "    address " . $iface{$if}->{"ipv4_addr"} . "\n";
            }
            if (defined $iface{$if}->{"ipv4_mask"}) {
                print "    netmask " . $iface{$if}->{"ipv4_mask"} . "\n";
            }
            if (defined $iface{$if}->{"ipv4_addr"}) {
                print "    broadcast " . $iface{$if}->{"ipv4_bcast"} . "\n";
            }
            if (defined $iface{$if}->{"gateway"}) {
                print "    gateway " . $iface{$if}->{"gateway"} . "\n";
            }
            if (defined $iface{$if}->{"pre-up"}) {
	        while ( my $upcmd = pop @{$iface{$if}->{"pre-up"}} ) {
                	print "    pre-up " . $upcmd . "\n";
		}
            }
            if (defined $iface{$if}->{"up"}) {
	        while ( my $upcmd = pop @{$iface{$if}->{"up"}} ) {
                	print "    up " . $upcmd . "\n";
		}
            }
            if (defined $iface{$if}->{"down"}) {
	        while ( my $downcmd = pop @{$iface{$if}->{"down"}} ) {
                	print "    down " . $downcmd . "\n";
		}
            }
            if (defined $iface{$if}->{"post-down"}) {
	        while ( my $downcmd = pop @{$iface{$if}->{"pre-down"}} ) {
                	print "    pre-down " . $downcmd . "\n";
		}
            }
        }
    }
    print "\n";
}

exit 0;
