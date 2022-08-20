#! /bin/sh
### BEGIN INIT INFO
# Provides:          haveged
# Required-Start:    $remote_fs
# Required-Stop:     $remote_fs
# Should-Start:      $syslog
# Should-Stop:       $syslog
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Entropy daemon using the HAVEGE algorithm
# Description:       haveged uses HAVEGE (HArdware Volatile Entropy Gathering
#                    and Expansion) to maintain a pool of random bytes used
#                    to fill /dev/random whenever necessary.
### END INIT INFO

# Do NOT "set -e"

PATH=/sbin:/usr/sbin:/bin:/usr/bin
DESC="entropy daemon"
NAME=haveged
DAEMON=/usr/sbin/$NAME
DAEMON_ARGS=""
PIDFILE=/var/run/$NAME.pid
SCRIPTNAME=/etc/init.d/$NAME

# Exit if the package is not installed
[ -x "$DAEMON" ] || exit 0

# Read configuration variable file if it is present
[ -r /etc/default/$NAME ] && . /etc/default/$NAME

# Load the VERBOSE setting and other rcS variables
. /lib/init/vars.sh

# Define LSB log_* functions.
. /lib/lsb/init-functions

do_start()
{
	start-stop-daemon --start --quiet --pidfile $PIDFILE --exec $DAEMON --test > /dev/null \
		|| return 1
	start-stop-daemon --start --quiet --pidfile $PIDFILE --exec $DAEMON -- \
		$DAEMON_ARGS \
		|| return 2
}

do_stop()
{
	start-stop-daemon --stop --quiet --retry=TERM/30/KILL/5 --pidfile $PIDFILE --name $NAME
	RETVAL="$?"
	[ "$RETVAL" = 2 ] && return 2
	rm -f $PIDFILE
	return "$RETVAL"
}

case "$1" in
    start)
	[ "$VERBOSE" != no ] && log_daemon_msg "Starting $DESC" "$NAME"
	do_start
	case "$?" in
	    0|1) [ "$VERBOSE" != no ] && log_end_msg 0 ;;
	    2) [ "$VERBOSE" != no ] && log_end_msg 1 ;;
	esac
	;;
    stop)
	[ "$VERBOSE" != no ] && log_daemon_msg "Stopping $DESC" "$NAME"
	do_stop
	case "$?" in
	    0|1) [ "$VERBOSE" != no ] && log_end_msg 0 ;;
	    2) [ "$VERBOSE" != no ] && log_end_msg 1 ;;
	esac
	;;
    status)
       status_of_proc "$DAEMON" "$NAME" && exit 0 || exit $?
       ;;
    restart|force-reload)
	log_daemon_msg "Restarting $DESC" "$NAME"
	do_stop
	case "$?" in
	    0|1)
		do_start
		case "$?" in
		    0) log_end_msg 0 ;;
		    1) log_end_msg 1 ;; # Old process is still running
		    *) log_end_msg 1 ;; # Failed to start
		esac
		;;
	    *)
		# Failed to stop
		log_end_msg 1
		;;
	esac
	;;
    *)
	echo "Usage: $SCRIPTNAME {start|stop|status|restart|force-reload}" >&2
	exit 3
	;;
esac

:
