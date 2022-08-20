#
# Makefile
#

LOCALCERTSDIR = /usr/local/share/ca-certificates

all:

clean:

install:
	mkdir -p $(DESTDIR)/$(LOCALCERTSDIR); \
	$(MAKE) -C local install LOCALCERTSDIR=$(DESTDIR)/$(LOCALCERTSDIR)

