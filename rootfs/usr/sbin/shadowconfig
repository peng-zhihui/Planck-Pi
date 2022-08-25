#!/bin/sh
# turn shadow passwords on or off on a Debian system

set -e

shadowon () {
    set -e
    pwck -q -r
    grpck -r
    pwconv
    grpconv
    chown root:root /etc/passwd /etc/group
    chmod 644 /etc/passwd /etc/group
    chown root:shadow /etc/shadow /etc/gshadow
    chmod 640 /etc/shadow /etc/gshadow
}

shadowoff () {
    set -e
    pwck -q -r
    grpck -r
    pwunconv
    grpunconv
    # sometimes the passwd perms get munged
    chown root:root /etc/passwd /etc/group
    chmod 644 /etc/passwd /etc/group
}

case "$1" in
    "on")
	if shadowon ; then
	    echo Shadow passwords are now on.
	else
	    echo Please correct the error and rerun \`$0 on\'
	    exit 1
	fi
	;;
    "off")
	if shadowoff ; then
	    echo Shadow passwords are now off.
	else
	    echo Please correct the error and rerun \`$0 off\'
	    exit 1
	fi
	;;
     *)
	echo Usage: $0 on \| off
	;;
esac
