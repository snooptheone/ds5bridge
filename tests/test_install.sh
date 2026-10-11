#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Anderson S.
# `make install` must put every file in place: the binaries, the systemd unit (with its path filled in), the module list and both udev rules. A forgotten file would
# leave the real pad hidden without the safety net (the rules only hide it while vhci_hcd is loaded, which modules-load.d guarantees at boot).
#
# And a test must NEVER touch the running system: staging into a DESTDIR may not call systemctl, udevadm, modprobe or sudo. Stand-ins for them sit first in the PATH and
# record every call; any call fails the test. (An unguarded `systemctl disable --now` in `make uninstall` once made this test ask for a password and, if
# it had been given, stop the real service.)
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
root="$tmp/root"
mkdir "$tmp/bin" "$root"
for c in systemctl udevadm modprobe sudo; do
    printf '#!/bin/sh\necho "%s $*" >> "%s/calls"\nexit 0\n' "$c" "$tmp" > "$tmp/bin/$c"
    chmod +x "$tmp/bin/$c"
done
PATH="$tmp/bin:$PATH" make install DESTDIR="$root" >/dev/null
fail=0
for f in usr/local/bin/ds5bridge usr/local/bin/ds5ctl etc/systemd/system/ds5bridge.service etc/modules-load.d/ds5bridge.conf \
         etc/udev/rules.d/71-ds5bridge.rules etc/udev/rules.d/72-ds5bridge-hide.rules usr/local/share/doc/ds5bridge/README.md etc/wireplumber/wireplumber.conf.d/90-dualsense.conf; do
    [ -e "$root/$f" ] || { echo "FAIL make install did not install $f"; fail=1; }
done
[ -e "$root/usr/local/bin/ds5fw" ] && { echo "FAIL make install put the firmware flasher in the PATH"; fail=1; }
[ -x "$root/usr/local/bin/ds5bridge" ] || { echo "FAIL ds5bridge is not executable"; fail=1; }
if grep -q '@' "$root/etc/systemd/system/ds5bridge.service"; then echo "FAIL the unit still has an unfilled @placeholder@"; fail=1; fi
grep -q "^StartLimitIntervalSec=0" "$root/etc/systemd/system/ds5bridge.service" || { echo "FAIL the unit can still end up stuck in failed (no StartLimitIntervalSec=0)"; fail=1; }
grep -q "^CapabilityBoundingSet=CAP_NET_RAW" "$root/etc/systemd/system/ds5bridge.service" || { echo "FAIL the unit does not limit the capabilities"; fail=1; }
grep -q "^ExecStart=/usr/local/bin/ds5bridge" "$root/etc/systemd/system/ds5bridge.service" || { echo "FAIL the unit does not start the installed binary"; fail=1; }
# the WirePlumber rule follows the version: 0.5+ gets the .conf only (0.5 warns about every Lua file), 0.4 the .lua only; a file of the other kind left by an earlier version is removed
[ -e "$root/etc/wireplumber/main.lua.d/90-dualsense.lua" ] && { echo "FAIL the default (0.5) install put a Lua file where WirePlumber 0.5 would warn about it"; fail=1; }
w=$tmp/wp; mkdir "$w"
PATH="$tmp/bin:$PATH" make install DESTDIR="$w" WPSTYLE=0.4 >/dev/null
{ [ -e "$w/etc/wireplumber/main.lua.d/90-dualsense.lua" ] && [ ! -e "$w/etc/wireplumber/wireplumber.conf.d/90-dualsense.conf" ]; } || { echo "FAIL WPSTYLE=0.4 must install the Lua file only"; fail=1; }
mkdir -p "$w/etc/wireplumber/main.lua.d"; echo stale > "$w/etc/wireplumber/main.lua.d/90-dualsense.lua"
PATH="$tmp/bin:$PATH" make install DESTDIR="$w" WPSTYLE=0.5 >/dev/null
{ [ -e "$w/etc/wireplumber/wireplumber.conf.d/90-dualsense.conf" ] && [ ! -e "$w/etc/wireplumber/main.lua.d/90-dualsense.lua" ]; } || { echo "FAIL WPSTYLE=0.5 must install the .conf only and remove an old Lua file"; fail=1; }
# uninstall removes what install put (only the DESTDIR copy is touched)
PATH="$tmp/bin:$PATH" make uninstall DESTDIR="$root" >/dev/null 2>&1 || true
left=$(find "$root" -type f | wc -l)
[ "$left" = 0 ] || { echo "FAIL make uninstall left $left file(s)"; find "$root" -type f; fail=1; }
if [ -s "$tmp/calls" ]; then echo "FAIL staging into a DESTDIR touched the running system:"; cat "$tmp/calls"; fail=1; fi
[ $fail = 0 ] && echo "test_install ok: make install puts the 6 files, the WirePlumber rule of the right kind and the docs in place (not the firmware flasher), the unit is filled in, uninstall removes them, and neither touches the running system"
exit $fail
