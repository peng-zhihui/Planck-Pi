#!/bin/sh
# hwclock.sh	Set and adjust the CMOS clock.
#
# Version:	@(#)hwclock.sh  2.00  14-Dec-1998  miquels@cistron.nl
#
# Patches:
#		2000-01-30 Henrique M. Holschuh <hmh@rcm.org.br>
#		 - Minor cosmetic changes in an attempt to help new
#		   users notice something IS changing their clocks
#		   during startup/shutdown.
#		 - Added comments to alert users of hwclock issues
#		   and discourage tampering without proper doc reading.
#               2012-02-16 Roger Leigh <rleigh@debian.org>
#                - Use the UTC/LOCAL setting in /etc/adjtime rather than
#                  the UTC setting in /etc/default/rcS.  Additionally
#                  source /etc/default/hwclock to permit configuration.

### BEGIN INIT INFO
# Provides:          hwclock
# Required-Start:    mountdevsubfs
# Required-Stop:     mountdevsubfs
# Should-Stop:       umountfs
# Default-Start:     S
# X-Start-Before:    checkroot
# Default-Stop:      0 6
# Short-Description: Sync hardware and system clock time.
### END INIT INFO

# These defaults are user-overridable in /etc/default/hwclock
BADYEAR=no
HWCLOCKACCESS=yes
HWCLOCKPARS=
HCTOSYS_DEVICE=rtc0

# We only want to use the system timezone or else we'll get
# potential inconsistency at startup.
unset TZ

hwclocksh()
{
    [ ! -x /sbin/hwclock ] && return 0
    [ ! -r /etc/default/rcS ] || . /etc/default/rcS
    [ ! -r /etc/default/hwclock ] || . /etc/default/hwclock

    . /lib/lsb/init-functions
    verbose_log_action_msg() { [ "$VERBOSE" = no ] || log_action_msg "$@"; }

    case "$BADYEAR" in
       no|"")	BADYEAR="" ;;
       yes)	BADYEAR="--badyear" ;;
       *)	log_action_msg "unknown BADYEAR setting: \"$BADYEAR\""; return 1 ;;
    esac

    case "$1" in
	start)
	    # If the admin deleted the hwclock config, create a blank
	    # template with the defaults.
	    if [ -w /etc ] && [ ! -f /etc/adjtime ] && [ ! -e /etc/adjtime ]; then
	        printf "0.0 0 0.0\n0\nUTC\n" > /etc/adjtime
	    fi

	    if [ -d /run/udev ] || [ -d /dev/.udev ]; then
		return 0
	    fi

	    if [ "$HWCLOCKACCESS" != no ]; then
		log_action_msg "Setting the system clock"

		# Just for reporting.
		if sed '3!d' /etc/adjtime | grep -q '^UTC$'; then
		    UTC="--utc"
		else
		    UTC=
		fi
		# Copies Hardware Clock time to System Clock using the correct
		# timezone for hardware clocks in local time, and sets kernel
		# timezone. DO NOT REMOVE.
		if /sbin/hwclock --rtc=/dev/$HCTOSYS_DEVICE --hctosys $HWCLOCKPARS $BADYEAR; then
		    #	Announce the local time.
		    verbose_log_action_msg "System Clock set to: `date $UTC`"
		else
		    log_warning_msg "Unable to set System Clock to: `date $UTC`"
		fi
	    else
		verbose_log_action_msg "Not setting System Clock"
	    fi
	    ;;
	stop|restart|reload|force-reload)
	    #
	    # Updates the Hardware Clock with the System Clock time.
	    # This will *override* any changes made to the Hardware Clock.
	    #
	    # WARNING: If you disable this, any changes to the system
	    #          clock will not be carried across reboots.
	    #

	    if [ "$HWCLOCKACCESS" != no ]; then
		log_action_msg "Saving the system clock"
		if /sbin/hwclock --rtc=/dev/$HCTOSYS_DEVICE --systohc $HWCLOCKPARS $BADYEAR; then
		    verbose_log_action_msg "Hardware Clock updated to `date`"
		fi
	    else
		verbose_log_action_msg "Not saving System Clock"
	    fi
	    ;;
	show)
	    if [ "$HWCLOCKACCESS" != no ]; then
		/sbin/hwclock --rtc=/dev/$HCTOSYS_DEVICE --show $HWCLOCKPARS $BADYEAR
	    fi
	    ;;
	*)
	    log_success_msg "Usage: hwclock.sh {start|stop|reload|force-reload|show}"
	    log_success_msg "       start sets kernel (system) clock from hardware (RTC) clock"
	    log_success_msg "       stop and reload set hardware (RTC) clock from kernel (system) clock"
	    return 1
	    ;;
    esac
}

hwclocksh "$@"
