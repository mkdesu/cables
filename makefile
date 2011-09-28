# Single-source file programs to build
progs   = cable/daemon cable/service cable/mhdrop bin/hex2base32 \
          cable/eeppriv.jar
cpextra_EepPriv = /opt/i2p/lib/i2p.jar
ldextra_daemon  = -lrt

title  := $(shell grep -o 'LIBERTE CABLE [[:alnum:]._-]\+' src/service.c)


# Installation directories (override DESTDIR and/or PREFIX)
# (DESTDIR is temporary, PREFIX is hard-coded into scripts)
DESTDIR=/
PREFIX=/usr
instdir=$(subst //,/,$(DESTDIR)$(PREFIX))


# Default compilers
CC      = gcc
JAVAC   = javac

# Modifications to compiler flags
CFLAGS += -std=c99 -Wall -pedantic
JFLAGS += -target 1.5 -deprecation -Werror -g:none
JLIBS  := $(subst : ,:,$(patsubst %,%:,$(wildcard lib/*.jar)))


# Build rules
.PHONY:     all clean install
.SUFFIXES:  .o
.SECONDARY:

all: $(progs)

clean:
	$(RM) -r $(progs) obj/*

bin/% cable/%: obj/%.o
	$(CC) -o $@ $(CFLAGS) $< $(LDFLAGS) $(ldextra_$*) 

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
	install -d $(instdir)/bin $(instdir)/libexec/cable $(instdir)/share/applications $(instdir)/share/cable
	install -m 644 -t $(instdir)/share/applications $(wildcard share/*.desktop)
	install        -t $(instdir)/share/cable   $(wildcard init/*)
	install -m 644 -t $(instdir)/share/cable   $(wildcard conf/*)
	install        -t $(instdir)/bin           bin/*
	install        -t $(instdir)/libexec/cable cable/*
	-chmod a-x $(patsubst %,$(instdir)/libexec/cable/%,suprofile extensions.cnf eeppriv.jar)
	sed -i     's&/usr/libexec/cable/&$(PREFIX)/libexec/cable/&g' \
	           $(patsubst %,$(instdir)/share/cable/%,cabled nginx-cable.conf) \
	           $(patsubst %,$(instdir)/bin/%,cable-id cable-ping gen-cable-username gen-tor-hostname gen-i2p-hostname)
