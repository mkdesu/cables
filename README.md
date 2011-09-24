# Secure Cables Communication

Secure and anonymous communication using email-like addresses, pioneered in [Liberté Linux](http://dee.su/liberte).
Cables communication is Liberté's pivotal component for enabling anyone to communicate safely and covertly in [hostile environments](http://dee.su/liberte-motivation).

See the relevant [peer review](http://dee.su/liberte-peer-review) section for further details.

## Requirements

[Nginx](http://nginx.org/), [spawn-fcgi](http://redmine.lighttpd.net/projects/spawn-fcgi) and [fcgiwrap](http://nginx.localdomain.pl/wiki/FcgiWrap) need to be installed in order for the [CGI service](https://github.com/mkdesu/cables/blob/master/src/service.c) to work. [Tor](https://www.torproject.org/) and [I2P](http://www.i2p2.de/) are needed to utilize the respective communication transports.

## Building

I2P's _i2p.jar_ library is needed to compile [EepPriv.java](https://github.com/mkdesu/cables/blob/master/src/EepPriv.java) — adjust the makefile accordingly. Building _eeppriv.jar_ is not necessary if you don't need the eepSite [keypair generation](https://github.com/mkdesu/cables/blob/master/cable/gen-i2p-hostname) functionality.

## Installation

The init services configuration works as-is in Gentoo, but is easy to adapt for any other Linux distribution.

The [mail sender](https://github.com/mkdesu/cables/blob/master/cable/send), [cables daemon](https://github.com/mkdesu/cables/blob/master/src/daemon.c) and the [CGI service](https://github.com/mkdesu/cables/blob/master/src/service.c) are supposed to be executed under _cable:cable_ user (which needs to be created). The [init service](https://github.com/mkdesu/cables/blob/master/init/cabled) for the cables daemon, [nginx](https://github.com/mkdesu/cables/blob/master/conf/nginx.conf) and [spawn-fcgi](https://github.com/mkdesu/cables/blob/master/conf/spawn-fcgi.cable) configuration should be adapted for the distribution and installed in the appropriate places. Cables and nginx init services should be then added to the relevant runlevels.

Cable pathnames should be adjusted in [suprofile](https://github.com/mkdesu/cables/blob/master/cable/suprofile) (note that _CABLE_MOUNT_ should be a mountpoint — use **/** for root filesystem), in [spawn-fcgi configuration](https://github.com/mkdesu/cables/blob/master/conf/spawn-fcgi.cable) _(CABLE_QUEUES)_ and in [nginx configuration](https://github.com/mkdesu/cables/blob/master/conf/nginx.conf) _(CABLE_PUB's literal value)_. Each occurrence of _CABLE_ in [nginx configuration](https://github.com/mkdesu/cables/blob/master/conf/nginx.conf) should be replaced with the cables username (from _CABLE_CERTS/certs/username_, see certificates propagation below), otherwise nginx will deny all queries.

The _CABLE_PUB/username/{certs,queue,rqueue}_ directories should be writable by _cable_ and readable by _nginx_ (e.g., _cable:nginx_, mode 03310). The _CABLE_INBOX_ and _CABLE_QUEUES/{queue,rqueue}_ directories should be writable by _cable_ (e.g., mode 01770 with _cable_ in user's group).

## User configuration

The user needs to execute [gen-cable-username](https://github.com/mkdesu/cables/blob/master/cable/gen-cable-username), [gen-tor-hostname](https://github.com/mkdesu/cables/blob/master/cable/gen-tor-hostname) and [gen-i2p-hostname](https://github.com/mkdesu/cables/blob/master/cable/gen-i2p-hostname) once, propagate cable certificates from _CABLE_CERTS/certs_ to _CABLE_PUB/username/certs_, and propagate the Tor and I2P keypairs to the respective transports; see how it is done in Liberté Linux's [identity](https://github.com/mkdesu/liberte/blob/master/src/etc/init.d/identity) init service (on each boot, since only _CABLE_MOUNT_ is persistent).

The user also needs to configure the mail client to send emails by piping the message to the [mail sender](https://github.com/mkdesu/cables/blob/master/cable/send), which needs to be executed under _cable:cable_ user (presumably via a _sudo_ wrapper). The email address to configure can be retrieved by running the [cable-info](https://github.com/mkdesu/cables/blob/master/bin/cable-info) applet, also available as a [desktop menu](https://github.com/mkdesu/cables/blob/master/share/cable-info.desktop) entry. Liveness of remote addresses can be checked with [cable-ping](https://github.com/mkdesu/cables/blob/master/cable/cable-ping).
