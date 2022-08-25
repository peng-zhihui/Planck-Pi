#!/bin/bash
#
# Review the cron tasks defined in the system and warn the admin
# if some of the files will not be run
#
# This program is copyright 2011 by Javier Fernandez-Sanguino <jfs@debian.org>
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program; if not, write to the Free Software
#    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
# For more information please see
#  http://www.gnu.org/licenses/licenses.html#GPL


set -e 
# reset locale, just in case
LC_ALL=C
export LC_ALL

PROGNAME=${0##*/}
PROGVERSION=1.0
# Command line options
SHORTOPTS=hvsi
LONGOPTS=help,version,syslog,info
set -- $(getopt -s bash -o $SHORTOPTS -l $LONGOPTS --n $PROGNAME -- "$@")

version () {
  echo "$PROGNAME $PROGVERSION"
  echo "$PROGNAME is copyright © Javier Fernandez-Sanguino <jfs@debian.org>"
  echo "Released under the terms of the GPL version 2 or later"
  echo "This program is part of the cron package"
}

usage () {
    cat <<EOUSE
    Usage: $PROGNAME [-si]

    Reviews the directories used by cron and reports scripts that
    might not be run by cron due to problems in their naming or
    in their setup.
     
    You should run this program as root to prevent false positives.

    Options:
        -s       -- Use syslog to report information
        -i       -- Report also informational messages
EOUSE
}

syslog="no"
send_info="no"
for opt in $@; do
    case $opt in
        -h|--help) usage; exit 0;;
        -v|--version) version; exit 0;;
        -s|--syslog) syslog="yes";;
        -i|--info)   send_info="yes";;
        *)  ;;
    esac
done
    

send_message () {

    level=$1
    msg=$2
    [ "$level" = "info" ] && [ "$send_info" = "no" ] && return

    if [ "$syslog" = "yes" ] ; then
        logger -p cron.$level -t CRON $msg
    else
        case $level in
            "warn")
                echo "WARN: $msg" >&2
                ;;
            "info")
                echo "INFO: $msg" 
                ;;
        esac
    fi
}

warn () {
# Send a warning to the user
    file=$1
    reason=$2

    name=`basename $file`
    # Skip hidden files
    echo $name | grep -q -E '^\.' && return
    # Skip disabled files
    echo $name | grep -q -E '\.disabled' && return

    # TODO: Should we send warnings for '.old' or '.orig'?

    # Do not send a warning if the file is '.dpkg-old' or '.dpkg-dist'
    if ! echo $file | grep -q -E '\.dpkg-(old|dist)$' ; then
        send_message "warn" "The file $file will not be executed by cron: $reason"
    else
        send_message "info" "The file $file is a leftover from the Debian package manager"
    fi
}

check_results () {

    dir=$1
    run_file=$2
    exec_test=$3

    # Now check the files we found and the ones that exist in the directory
    find $dir \( -type f -o -type l \) -printf '%p %l\n'  |
    while read file pointer; do
        if ! grep -q "^$file$" $run_file; then
            [ -L "$file" ] && [ ! -e "$pointer" ] && \
                    warn $file "Points to an nonexistent location ($pointer)" && continue
            [ "$exec_test" = "yes" ]  && [ ! -x "$file" ] &&  \
                    warn $file "Is not executable" && continue
            [ ! -r "$file" ] && [ "`id -u`" != "0" ] && \
                    warn $file "Cannot read the file to determine if it will be run ($PROGNAME is not running as root)" && continue
            [ ! -r "$file" ] && \
                    warn $file "File is unreadable" && continue
             warn $file "Does not conform to the run-parts convention"
        else

# do additional checks for symlinks for files that *are* listed by run-parts 
            if [ -L "$file" ] ; then
# for symlinks: does the file exist?
                if [ ! -e "$pointer" ] ; then
                    warn $file "Points to an nonexistent location ($pointer)"
                fi
# for symlinks: is it owned by root?
                 owner=`ls -l $pointer  | awk '{print $3}'`
                 if [ "$owner" != "root" ]; then
                       warn $file "Is not owned by root"
                 fi
            fi

       fi
    done

}

# Setup for the tests

# First: check if we are using -l
[ -r /etc/default/cron ] &&  . /etc/default/cron
use_lsb="no"
[ "$LSBNAMES" = "-l" ] && use_lsb="yes"
echo $EXTRA_OPTS | grep -q -- '-l' && use_lsb="yes"
# Set the options for run parts
run_opts=""
[ "$use_lsb" = "yes" ] &&  run_opts="--lsbsysinit"

temp=`tempfile` || { echo "ERROR: Cannot create temporary file" >&2 ; exit 1; }
trap "rm -f $temp" 0 1 2 3 13 15

# Now review the scripts, note that cron does not use run-parts to run these
# so they are *not* required to be executables, just to conform with the 

# Step 1: Review /etc/cron.d
run-parts $run_opts --list /etc/cron.d >$temp
check_results /etc/cron.d $temp "no"


# Step 2: Review /etc/cron.{hourly,daily,weekly,monthly}

for interval in hourly daily weekly monthly; do
    testdir=/etc/cron.$interval
    run-parts $run_opts --test $testdir >$temp
    check_results $testdir $temp "yes"
done


exit 0

