# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Anderson S.
CC      ?= cc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-format-truncation -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fPIE
LDFLAGS ?= -pie -Wl,-z,relro,-z,now   # the daemon runs as root
# Keep VERSION in step with packaging/arch/PKGBUILD, debian/changelog and packaging/rpm/ds5bridge.spec (tests/test_packaging.sh checks). No comment after the value: make would keep the blanks.
VERSION ?= 0.1.0
VERDEF   = -DDS5_VERSION='"$(VERSION)"'
PREFIX  ?= /usr/local
# Where `make install` puts the system files. The defaults suit a manual install (the admin's /etc); packages override them with the vendor directories:
#   make install PREFIX=/usr SYSTEMDDIR=/usr/lib/systemd/system UDEVDIR=/usr/lib/udev/rules.d MODULESDIR=/usr/lib/modules-load.d \
#        WPCONFDIR=/usr/share/wireplumber/wireplumber.conf.d WPLUADIR=/usr/share/wireplumber/main.lua.d
SYSTEMDDIR ?= /etc/systemd/system
UDEVDIR    ?= /etc/udev/rules.d
MODULESDIR ?= /etc/modules-load.d
# WirePlumber: the rule that gives the pad's audio the shape Proton needs: service/90-dualsense.conf for WirePlumber 0.5 and later, 90-dualsense.lua for 0.4. Only the file for
# the installed version is installed (0.5 warns about every Lua file it finds: "Old configuration file detected"). WPSTYLE is 0.5 or 0.4; the default, auto, looks at the
# installed WirePlumber when installing for real (0.5 when there is none) and means 0.5 when staging into a DESTDIR (a packager says which: the Debian package picks by the
# distribution's WirePlumber version). Packages use /usr/share/wireplumber/wireplumber.conf.d and /usr/share/wireplumber/main.lua.d.
WPCONFDIR  ?= /etc/wireplumber/wireplumber.conf.d
WPLUADIR   ?= /etc/wireplumber/main.lua.d
WPSTYLE    ?= auto
ifeq ($(WPSTYLE),auto)
ifeq ($(DESTDIR),)
WPSTYLE := $(if $(filter 0.4,$(shell wireplumber --version 2>/dev/null | sed -n 's/.*libwireplumber \([0-9]*\.[0-9]*\).*/\1/p')),0.4,0.5)
else
WPSTYLE := 0.5
endif
endif
DOCDIR     ?= $(PREFIX)/share/doc/ds5bridge
BIN      = bin/ds5bridge bin/ds5ctl bin/ds5fw

all: $(BIN)
bin/ds5bridge: src/ds5bridge.c src/ds5proto.h src/descriptors.h Makefile
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VERDEF) src/ds5bridge.c -o $@ $(LDFLAGS) -lopus -lm -pthread
bin/ds5ctl: tools/ds5ctl.c src/ds5proto.h Makefile
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VERDEF) -Isrc tools/ds5ctl.c -o $@ $(LDFLAGS)
bin/ds5fw: tools/ds5fw.c src/ds5proto.h
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc tools/ds5fw.c -o $@ $(LDFLAGS)
clean:
	rm -rf bin

bin/test_unit: tests/test_unit.c src/ds5bridge.c src/ds5proto.h src/descriptors.h
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_unit.c -o $@ $(LDFLAGS) -lopus -lm -pthread
bin/test_proto: tests/test_proto.c src/descriptors.h
	@mkdir -p bin
	$(CC) $(CFLAGS) tests/test_proto.c -o $@ $(LDFLAGS)
bin/test_lifecycle: tests/test_lifecycle.c tests/padtest.h tests/fakepad.h src/descriptors.h
	@mkdir -p bin
	$(CC) $(CFLAGS) tests/test_lifecycle.c -o $@ $(LDFLAGS)
bin/test_multipad: tests/test_multipad.c tests/padtest.h tests/fakepad.h src/descriptors.h
	@mkdir -p bin
	$(CC) $(CFLAGS) tests/test_multipad.c -o $@ $(LDFLAGS)
bin/test_route: tests/test_route.c tests/padtest.h tests/fakepad.h src/descriptors.h
	@mkdir -p bin
	$(CC) $(CFLAGS) tests/test_route.c -o $@ $(LDFLAGS)
bin/test_ctl_pad: tests/test_ctl_pad.c tests/fakepad.h
	@mkdir -p bin
	$(CC) $(CFLAGS) tests/test_ctl_pad.c -o $@ $(LDFLAGS)
test: all bin/test_unit bin/test_proto bin/test_lifecycle bin/test_multipad bin/test_route bin/test_ctl_pad
	./bin/test_unit
	./bin/test_unit --dump-packets | cmp - tests/golden.txt && echo "golden packets ok: identical to the Python reference"
	./bin/test_proto
	./bin/test_lifecycle
	./bin/test_multipad
	./bin/test_route
	./bin/test_ctl_pad
	tests/test_tools.sh
	tests/test_options.sh
	tests/test_install.sh
	tests/test_packaging.sh
	tests/test_wireplumber.sh

# the same tests with AddressSanitizer + UndefinedBehaviorSanitizer (the tests start the binaries from bin/, so everything is built into bin/asan/bin)
SAN = -g -O1 -std=gnu11 -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-format-truncation
asan:
	@mkdir -p bin/asan/bin
	$(CC) $(SAN) src/ds5bridge.c -o bin/asan/bin/ds5bridge -lopus -lm -pthread
	$(CC) $(SAN) -Isrc tools/ds5ctl.c -o bin/asan/bin/ds5ctl
	$(CC) $(SAN) tests/test_unit.c -o bin/asan/test_unit -lopus -lm -pthread
	$(CC) $(SAN) tests/test_proto.c -o bin/asan/test_proto
	$(CC) $(SAN) tests/test_lifecycle.c -o bin/asan/test_lifecycle
	$(CC) $(SAN) tests/test_multipad.c -o bin/asan/test_multipad
	$(CC) $(SAN) tests/test_route.c -o bin/asan/test_route
	$(CC) $(SAN) tests/test_ctl_pad.c -o bin/asan/test_ctl_pad
	cd bin/asan && export ASAN_OPTIONS=detect_leaks=0 && ./test_unit && ./test_proto && ./test_lifecycle && ./test_multipad && ./test_route && ./test_ctl_pad

# ---- install (sudo make install after make): ds5bridge and ds5ctl, systemd unit, udev rules. The firmware flasher bin/ds5fw is not installed: it has never run on hardware, run it from here.
DESTDIR ?=
install: all
	install -Dm755 bin/ds5bridge $(DESTDIR)$(PREFIX)/bin/ds5bridge
	install -Dm755 bin/ds5ctl    $(DESTDIR)$(PREFIX)/bin/ds5ctl
	install -d $(DESTDIR)$(SYSTEMDDIR)
	sed 's|@BINDIR@|$(PREFIX)/bin|' service/ds5bridge.service.in > $(DESTDIR)$(SYSTEMDDIR)/ds5bridge.service
	install -Dm644 service/ds5bridge-modules.conf  $(DESTDIR)$(MODULESDIR)/ds5bridge.conf
	install -Dm644 service/71-ds5bridge.rules      $(DESTDIR)$(UDEVDIR)/71-ds5bridge.rules
	install -Dm644 service/72-ds5bridge-hide.rules $(DESTDIR)$(UDEVDIR)/72-ds5bridge-hide.rules
	@case "$(WPSTYLE)" in 0.5) install -Dm644 service/90-dualsense.conf $(DESTDIR)$(WPCONFDIR)/90-dualsense.conf ;; 0.4) rm -f $(DESTDIR)$(WPCONFDIR)/90-dualsense.conf ;; esac
	@case "$(WPSTYLE)" in 0.4) install -Dm644 service/90-dualsense.lua  $(DESTDIR)$(WPLUADIR)/90-dualsense.lua ;; 0.5) rm -f $(DESTDIR)$(WPLUADIR)/90-dualsense.lua; rmdir $(DESTDIR)$(WPLUADIR) 2>/dev/null || true ;; esac
	@echo "WirePlumber rule for version $(WPSTYLE) installed"
	install -Dm644 README.md                       $(DESTDIR)$(DOCDIR)/README.md
	install -Dm644 docs/PROTOCOL.md                $(DESTDIR)$(DOCDIR)/PROTOCOL.md
	install -Dm644 contrib/README.md               $(DESTDIR)$(DOCDIR)/contrib/README.md
	install -Dm644 contrib/hid-playstation-mic.patch $(DESTDIR)$(DOCDIR)/contrib/hid-playstation-mic.patch
	@if [ -z "$(DESTDIR)" ]; then modprobe vhci_hcd; systemctl daemon-reload; udevadm control --reload; fi
	@echo "installed. Restart WirePlumber once so that it reads the pad audio rule (as your user: systemctl --user restart wireplumber), then reconnect the pad (PS button); logs: journalctl -u ds5bridge -f"
uninstall:
	@if [ -z "$(DESTDIR)" ]; then systemctl disable --now ds5bridge.service 2>/dev/null || true; fi   # never touch the running system when staging into a DESTDIR
	rm -f $(DESTDIR)$(PREFIX)/bin/ds5bridge $(DESTDIR)$(PREFIX)/bin/ds5ctl $(DESTDIR)$(PREFIX)/bin/ds5fw \
	      $(DESTDIR)$(SYSTEMDDIR)/ds5bridge.service $(DESTDIR)$(MODULESDIR)/ds5bridge.conf $(DESTDIR)$(UDEVDIR)/71-ds5bridge.rules $(DESTDIR)$(UDEVDIR)/72-ds5bridge-hide.rules \
	      $(DESTDIR)$(WPCONFDIR)/90-dualsense.conf $(DESTDIR)$(WPLUADIR)/90-dualsense.lua
	rm -f $(DESTDIR)$(DOCDIR)/README.md $(DESTDIR)$(DOCDIR)/PROTOCOL.md $(DESTDIR)$(DOCDIR)/contrib/README.md $(DESTDIR)$(DOCDIR)/contrib/hid-playstation-mic.patch
	-rmdir $(DESTDIR)$(DOCDIR)/contrib $(DESTDIR)$(DOCDIR) 2>/dev/null
	@if [ -z "$(DESTDIR)" ]; then systemctl daemon-reload; udevadm control --reload; udevadm trigger --subsystem-match=hidraw --subsystem-match=input --action=change; fi   # a pad that is connected right now gets its normal permissions back

# ---- optional: the Windows WASAPI test client (needs clang + lld and wine's import libs), run with tools/wasapi/run.sh
wasapi:
	@mkdir -p bin
	clang --target=x86_64-windows-gnu -nostdlib -ffreestanding -c tools/wasapi/wasapi_test.c -o bin/wasapi_test.o
	lld-link /nodefaultlib /entry:main /subsystem:console /out:bin/wasapi_test.exe bin/wasapi_test.o \
	    /usr/lib/wine/x86_64-windows/libkernel32.a /usr/lib/wine/x86_64-windows/libmsvcrt.a /usr/lib/wine/x86_64-windows/libole32.a

.PHONY: all clean test asan install uninstall wasapi
