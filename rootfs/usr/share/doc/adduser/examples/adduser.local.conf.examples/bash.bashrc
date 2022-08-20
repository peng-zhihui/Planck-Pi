#########################################################################
#      /etc/bash.bashrc: System-wide initialisation file for Bash       #
#########################################################################

# [JNZ] Modified 21-Jun-2013

# This script file is sourced by bash(1) for interactive shells.  It is
# also sourced by /etc/profile for (possibly non-interactive) login
# shells.
#
# Written by John Zaitseff and released into the public domain.


# Useful shell settings

shopt -s checkwinsize expand_aliases
set -P

# Useful variable settings

export LANG=en_AU.UTF-8				# We are in Australia
export LC_ALL=en_AU.UTF-8
export TIME_STYLE=$'+%b %e  %Y\n%b %e %H:%M'	# As used by ls(1)

# Useful aliases, defined whether or not this shell is interactive

alias cls=clear
alias ls="ls -v"
alias ll="ls -l"
alias l.="ls -A"
alias dir="ls -laF"
alias e="emacs -nw"
alias lo=libreoffice

# Set a variable identifying any Debian Chroot Compilation Environment

if [ -z "$debian_chroot" -a -r /etc/debian_chroot ]; then
    export debian_chroot=$(cat /etc/debian_chroot)
fi

# Run the following only if this shell is interactive

if [ "$PS1" ]; then

    export HISTIGNORE="&: +.*"		# Forget commands starting with space
    unset HISTFILE			# Don't save commands to history file
    export LESSHISTFILE=-		# Don't save history for less(1)
    export PROMPT_DIRTRIM=2		# Trailing directory components to keep

    # Make less(1) more friendly for non-text input files
    if [ -x /usr/bin/lesspipe ]; then
	eval $(/usr/bin/lesspipe)
    fi

    # Allow the Debian Chroot Compilation Environment to modify the prompt
    if [ -z "$debian_chroot" ]; then
	PS1h="\h"
    else
	PS1h="($debian_chroot)"
    fi

    # Set options depending on terminal type
    if [ -x /usr/bin/tput ] && tput setaf 1 >&/dev/null; then
	# The terminal supports colour: assume it complies with ECMA-48
	# (ISO/IEC-6429).  This is almost always the case...

	# Make ls(1) use colour in its listings
	if [ -x /usr/bin/dircolors ]; then
	    alias ls="ls -v --color=auto"
	    eval $(/usr/bin/dircolors --sh)
	fi

        # Set the terminal prompt
        if [ $(id -u) -ne 0 ]; then
            PS1="\[\e[42;30m\]\u@$PS1h\[\e[37m\]:\[\e[30m\]\w\[\e[0m\] \\\$ "
        else
            # Root user gets a nice RED prompt!
            PS1="\[\e[41;37;1m\]\u@$PS1h\[\e[30m\]:\[\e[37m\]\w\[\e[0m\] \\\$ "
        fi
    else
	# The terminal does not support colour
	PS1="\u@$PS1h:\w \\\$ "
    fi

    # Allow bash(1) completion in interactive shells

    if ! shopt -oq posix; then
	if [ -f /usr/share/bash-completion/bash_completion ]; then
	    . /usr/share/bash-completion/bash_completion
	elif [ -f /etc/bash_completion ]; then
	    . /etc/bash_completion
	fi
    fi

    unset PS1h
fi
