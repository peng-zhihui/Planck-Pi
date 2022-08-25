#!/bin/sh

if [ ! -e /etc/pcmcia/shared ]; then exit 1; fi

pcmcia_shared () {
	. /etc/pcmcia/shared
}

iface="$1"

# /etc/pcmcia/shared sucks
pcmcia_shared "start" $iface
usage () {
	exit 1
}

get_info $iface
HWADDR=`/sbin/ifconfig $DEVICE | sed -n -e 's/.*addr \([^ ]*\) */\1/p'`

which=""
while read glob scheme; do
	if [ "$which" ]; then continue; fi
	case "$SCHEME,$SOCKET,$INSTANCE,$HWADDR" in
		$glob) which=$scheme ;;
	esac
done

if [ "$which" ]; then echo $which; exit 0; fi
exit 1
