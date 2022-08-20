#!/bin/sh

# 6 seconds maximum wait time
attempts=${IF_DAD_ATTEMPTS:-60}
delay=${IF_DAD_INTERVAL:-0.1}

[ $attempts -eq 0 ] && exit 0

echo -n "Waiting for DAD... "
for attempt in $(seq 1 $attempts); do
	tentative=$(ip -o -6 address list dev "$IFACE" to "${IF_ADDRESS}/${IF_NETMASK}" tentative | wc -l)
	if [ $tentative -eq 0 ]; then
		attempt=0 # This might have been our last attempt, but successful
		break
	fi
	sleep $delay
done

if [ $attempt -eq $attempts ]; then
	echo "Timed out"
	exit 1
fi

dadfailed=$(ip -o -6 address list dev "$IFACE" to "${IF_ADDRESS}/${IF_NETMASK}" dadfailed | wc -l)

if [ $dadfailed -ge 1 ]; then
	echo "Failed"
	exit 1
fi

echo Done
