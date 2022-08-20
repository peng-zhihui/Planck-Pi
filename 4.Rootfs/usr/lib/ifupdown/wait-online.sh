#!/bin/sh
set -e

WAIT_ONLINE_METHOD="ifup"
WAIT_ONLINE_IFACE=""
WAIT_ONLINE_ADDRESS=""
WAIT_ONLINE_TIMEOUT=300

[ -f /etc/default/networking ] && . /etc/default/networking

case "$WAIT_ONLINE_METHOD" in
route)
	[ -n "$WAIT_ONLINE_ADDRESS" ] || WAIT_ONLINE_ADDRESS=default
	(/usr/bin/timeout "$WAIT_ONLINE_TIMEOUT" /sbin/ip mon r & /sbin/ip -4 r s; /sbin/ip -6 r s) | /bin/grep -q "^$WAIT_ONLINE_ADDRESS\>"
	;;

ping)
	if [ -z "$WAIT_ONLINE_ADDRESS" ]; then
		echo "No WAIT_ONLINE_ADDRESS specified" >&2
		exit 1
	fi
	/bin/ping -q -c 1 -w "$WAIT_ONLINE_TIMEOUT" "$WAIT_ONLINE_ADDRESS" >/dev/null
	;;

ping6)
	/bin/ping6 -q -c 1 -w "$WAIT_ONLINE_TIMEOUT" "$WAIT_ONLINE_ADDRESS" >/dev/null
	;;

ifup|iface|interface)
	up=false
	if [ -z "$WAIT_ONLINE_IFACE" ]; then
		auto_list="$(/sbin/ifquery -X lo --list)"
		hotplug_list="$(/sbin/ifquery -X lo --allow=hotplug --list)"
		if [ -n "$auto_list" ]; then
			for i in $(seq 1 $WAIT_ONLINE_TIMEOUT); do
				up=true
				for iface in $auto_list; do
					if ! /sbin/ifquery --state $iface >/dev/null; then
						up=false
						break
					fi
				done
				if [ $up = true ]; then
					break
				fi
				sleep 1
			done
		elif [ -n "$hotplug_list" ]; then
			for i in $(seq 1 $WAIT_ONLINE_TIMEOUT); do
				if [ -n "$(/sbin/ifquery --state $hotplug_list)" ]; then
					up=true
					break
				fi
				sleep 1
			done
		else
			exit 0
		fi
	else
		for i in $(seq 1 $WAIT_ONLINE_TIMEOUT); do
			if [ -n "$(/sbin/ifquery --state $WAIT_ONLINE_IFACE)" ]; then
				up=true
				break
			fi
			sleep 1
		done
	fi
	[ $up = true ] || exit 1
	;;

no|none)
	exit 0
	;;

*)
	echo "Unknown wait method $WAIT_ONLINE_METHOD" >&2
	exit 1
	;;
esac
