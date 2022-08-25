#!/bin/sh

if [ `id -u` -ne 0 ] || [ "$1" = "" ]; then exit 1; fi

if [ -x /usr/bin/fping ]; then
	PING="/usr/bin/fping"
else
	PING="/bin/ping -c 2"
fi

iface="$1"
which=""

while read addr pingme scheme; do
	if [ "$which" ]; then continue; fi

	#echo "  Trying $addr & $pingme ($scheme)" >&2

	ip addr add $addr dev $iface  >/dev/null 2>&1
	ip link set $iface up         >/dev/null 2>&1

	if $PING $pingme >/dev/null 2>&1; then
		which="$scheme"	
	fi
	ip link set $iface down       >/dev/null 2>&1
	ip addr del $addr dev $iface  >/dev/null 2>&1
done

if [ "$which" ]; then echo $which; exit 0; fi
exit 1
