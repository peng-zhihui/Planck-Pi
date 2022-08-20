#########################################################################
#          ~/.bashrc: Personal initialisation script for Bash           #
#########################################################################

# [JNZ] Modified 21-Jun-2013

# This script file is sourced by interactive Bash shells (ie, shells for
# which you are able to provide keyboard input).  It is also sourced by
# ~/.bash_profile for login shells.  It is the best place to put shell
# variables, functions and aliases.
#
# Written by John Zaitseff and released into the public domain.


# Variable settings for your convenience

export EDITOR=emacs				# Everyone's favourite editor

# Run the following only if this shell is interactive

if [ "$PS1" ]; then
    export IGNOREEOF=5				# Disallow accidental Ctrl-D
fi
