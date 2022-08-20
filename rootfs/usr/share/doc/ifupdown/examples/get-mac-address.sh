#!/bin/sh

set -e

export LANG=C

/sbin/ip -brief link show dev "$1" | read iface state mac rest
which=""

while read testmac scheme; do
	if [ "$which" ]; then
		continue;
	fi

	if [ "$mac" = "$(echo "$testmac" | tr A-F a-f)" ]; then
		which="$scheme";
	fi
done

if [ "$which" ]; then
	echo "$which";
else
	exit 1;
fi
