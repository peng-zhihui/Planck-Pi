#
# (C) Copyright 2012 Henrik Nordstrom <henrik@henriknordstrom.net>
#
# Based on some other Makefile
# (C) Copyright 2000-2003
# Wolfgang Denk, DENX Software Engineering, wd@denx.de.
#
# SPDX-License-Identifier:	GPL-2.0+
#
obj-y	+= timer.o
obj-y	+= lowlevel_init.o

ifdef CONFIG_SPL_BUILD
obj-y	+= fel_utils.o
CFLAGS_fel_utils.o := -marm
endif
