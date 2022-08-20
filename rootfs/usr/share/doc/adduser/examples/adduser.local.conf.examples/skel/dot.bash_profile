#########################################################################
#       ~/.bash_profile: Personal initialisation script for Bash        #
#########################################################################

# [JNZ] Modified 21-Jun-2013

# This script file is sourced by bash(1) for login shells.
#
# When a login shell starts, the following script files are sourced, in
# this order:
#
#   /etc/profile          - run by bash(1)
#   /etc/profile.d/*.sh   - additional profile scripts
#   /etc/bash.bashrc      - sourced by /etc/profile file (only for bash(1))
#   $HOME/.bash_profile   - this file
#   $HOME/.bashrc         - sourced by this file (if unchanged)
#
# When a normal (non-login) bash(1) shell starts, the following files are
# sourced:
#
#   /etc/bash.bashrc      - run by bash(1)
#   $HOME/.bashrc         - run by bash(1)
#
# Written by John Zaitseff and released into the public domain.


if [ -f $HOME/.bashrc ]; then
    . $HOME/.bashrc
fi

# Display a verse from the Bible

if [ ! -f $HOME/.hushlogin ] && [ ! -f $HOME/.hushverse ]; then
    if [ $(type -p verse) ]; then
	echo
	verse
	echo
    fi
fi

# Turn on talk(1) messages, unless the user does not want this

if [ ! -f $HOME/.hushlogin ] && [ ! -f $HOME/.hushtalk ]; then
    mesg y 2>/dev/null
fi
