# Single-source file programs to build
progs   = cable/daemon cable/mhdrop cable/hex2base32 \
          cable/eeppriv.jar
objextra_daemon = obj/server.o obj/service.o obj/util.o
ldextra_daemon  = -lrt -lmicrohttpd
cpextra_EepPriv = /opt/i2p/lib/i2p.jar

title  := $(shell grep -o 'LIBERTE CABLE [[:alnum:]._-]\+' src/daemon.h)


# Installation directories (override DESTDIR and/or PREFIX)
# (DESTDIR is temporary, (ETC)PREFIX is hard-coded into scripts)
DESTDIR=
PREFIX=/usr
ETCPREFIX=$(patsubst %/usr/etc,%/etc,$(PREFIX)/etc)

instdir=$(DESTDIR)$(PREFIX)
etcdir=$(DESTDIR)$(ETCPREFIX)


# Default compilers
CC      = gcc
JAVAC   = javac

# Modifications to compiler flags
CFLAGS := -std=c99 -Wall -pedantic -MMD -D_FILE_OFFSET_BITS=64 -D_POSIX_C_SOURCE=200809L -D_BSD_SOURCE -DNDEBUG $(CFLAGS)
JFLAGS := -target 1.5 -deprecation -Werror -g:none $(JFLAGS)
JLIBS   = $(subst : ,:,$(addsuffix :,$(wildcard lib/*.jar)))


# Build rules
.PHONY:     all clean install
.SUFFIXES:  .o
.SECONDARY:

all: $(progs)

clean:
	$(RM) -r $(progs) obj/*

cable/%: obj/%.o
	$(CC) -o $@ $(CFLAGS) $< $(objextra_$*) $(LDFLAGS) $(ldextra_$*)

obj/%.o: src/%.c
	$(CC) -c -o $@ $(CFLAGS) $<

cable/eeppriv.jar: obj/su/dee/i2p/EepPriv.class
	echo "Manifest-Version: 1.0"                                   > $(<D)/manifest.mf
	echo "Created-By: $(title)"                                   >> $(<D)/manifest.mf
	echo "Main-Class: $(subst /,.,$(subst obj/,,$(basename $<)))" >> $(<D)/manifest.mf
	echo "Class-Path: $(cpextra_$(basename $(<F)))"               >> $(<D)/manifest.mf
	jar c0mf $(<D)/manifest.mf $@ -C obj $(patsubst obj/%,%,$^)

obj/%.class: src/%.java
	$(JAVAC) -d obj $(JFLAGS) $< -classpath obj:$(JLIBS)$(cpextra_$(basename $(*F)))

install: all
	install -d $(etcdir)/cable $(instdir)/bin $(instdir)/libexec/cable $(instdir)/share/applications
	install -m 644 -t $(etcdir)/cable               $(wildcard conf/*)
	install        -t $(instdir)/bin                bin/*
	install        -t $(instdir)/libexec/cable      cable/*
	install -m 644 -t $(instdir)/share/applications $(wildcard share/*.desktop)
	-chmod a-x $(instdir)/libexec/cable/eeppriv.jar
	sed -i     's&/usr/libexec/cable\>&$(PREFIX)/libexec/cable&g' \
	           $(addprefix $(etcdir)/cable/,profile cabled)       \
	           $(instdir)/bin/cable-send
	sed -i     's&/etc/cable\>&$(ETCPREFIX)/cable&g'              \
	           $(etcdir)/cable/profile                            \
	           $(addprefix $(instdir)/libexec/cable/,cabled send) \
	           $(addprefix $(instdir)/bin/,cable-id cable-ping cable-send gen-cable-username gen-tor-hostname gen-i2p-hostname)


# File-specific dependencies
cable/daemon:  $(objextra_daemon)
-include $(wildcard obj/*.d)
