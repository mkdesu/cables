# Single-source file programs to build
progs   = cable/daemon cable/service cable/mhdrop bin/hex2base32 \
          cable/eeppriv.jar
cpextra_EepPriv = /opt/i2p/lib/i2p.jar
ldextra_daemon  = -lrt

title  := $(shell grep -o 'LIBERTE CABLE [[:alnum:]._-]\+' src/service.c)


# Default compilers
CC      = gcc
JAVAC   = javac

# Modifications to compiler flags
CFLAGS += -std=c99 -Wall -pedantic
JFLAGS += -target 1.5 -deprecation -Werror -g:none
JLIBS  := $(subst : ,:,$(patsubst %,%:,$(wildcard lib/*.jar)))


# Build rules
.PHONY:     all clean
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
