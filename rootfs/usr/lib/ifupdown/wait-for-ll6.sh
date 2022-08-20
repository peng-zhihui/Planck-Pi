#!/bin/sh

attempts=${IF_LL_ATTEMPTS:-60}
delay=${IF_LL_INTERVAL:-0.1}

for attempt in $(seq 1 $attempts); do
	lladdress=$(ip -6 -o a s dev "$IFACE" scope link -tentative)
	if [ -n "$lladdress" ]; then
		attempt=0
		break
	fi
	sleep $delay
done

if [ $attempt -eq $attempts ]; then
	echo "Could not get a link-local address"
	exit 1
fi
