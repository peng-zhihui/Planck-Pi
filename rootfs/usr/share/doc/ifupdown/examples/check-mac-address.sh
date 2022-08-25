#!/bin/sh
#
# Checks if the given interface matches the given ethernet MAC.
# If it does it exits with 0 (success) status;
# if it doesn't then it exists with 1 (error) status.

set -e

export LANG=C

if [ ! "$1" -o ! "$2" ]; then
	echo "Usage: $0 IFACE targetMAC"
	exit 1
fi

/sbin/ip -brief link show dev "$1" | read iface state mac rest
targetmac=`echo "$2" | tr A-F a-f`

test "$targetmac" = "$mac"
