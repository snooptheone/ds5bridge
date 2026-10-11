#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Anderson S.
# The packages (Arch, Debian/Ubuntu, Fedora) and the Makefile must agree: one version everywhere, the same installed tree whoever builds it, and a license header in every source file.
# With a tag as argument (the release workflow passes $GITHUB_REF_NAME) the tag must be v<version>.
# It builds nothing for the packages and touches nothing outside a temporary directory.
set -eu
cd "$(dirname "$0")/.."
fail=0
bad() { echo "FAIL $*"; fail=1; }

# ---- one version everywhere
v_make=$(sed -n 's/^VERSION ?= \([0-9][0-9.]*\)$/\1/p' Makefile)
v_arch=$(sed -n 's/^pkgver=//p' packaging/arch/PKGBUILD)
v_deb=$(sed -n '1s/^ds5bridge (\([^)-]*\)).*/\1/p' debian/changelog)
v_rpm=$(sed -n 's/^Version: *//p' packaging/rpm/ds5bridge.spec)
v_rpmlog=$(sed -n '/^%changelog/{n;s/.* - \([0-9][0-9.]*\)-[0-9]*$/\1/p;q}' packaging/rpm/ds5bridge.spec)
[ -n "$v_make" ] || bad "no VERSION in the Makefile"
for v in "$v_arch" "$v_deb" "$v_rpm" "$v_rpmlog"; do [ "$v" = "$v_make" ] || bad "version mismatch: Makefile $v_make, PKGBUILD $v_arch, debian/changelog $v_deb, spec $v_rpm (changelog $v_rpmlog)"; done
[ -z "${1:-}" ] || [ "$1" = "v$v_make" ] || bad "the tag is $1 but the version is $v_make"

# ---- the installed tree is the same for every packager
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
make -s install DESTDIR="$tmp" PREFIX=/usr SYSTEMDDIR=/usr/lib/systemd/system UDEVDIR=/usr/lib/udev/rules.d MODULESDIR=/usr/lib/modules-load.d DOCDIR=/usr/share/doc/ds5bridge \
  WPCONFDIR=/usr/share/wireplumber/wireplumber.conf.d WPLUADIR=/usr/share/wireplumber/main.lua.d WPSTYLE=0.5 >/dev/null
got=$(cd "$tmp" && find . -type f | sort)
want=$(printf '%s\n' ./usr/bin/ds5bridge ./usr/bin/ds5ctl ./usr/lib/modules-load.d/ds5bridge.conf ./usr/lib/systemd/system/ds5bridge.service ./usr/lib/udev/rules.d/71-ds5bridge.rules \
       ./usr/lib/udev/rules.d/72-ds5bridge-hide.rules ./usr/share/doc/ds5bridge/PROTOCOL.md ./usr/share/doc/ds5bridge/README.md ./usr/share/doc/ds5bridge/contrib/README.md \
       ./usr/share/doc/ds5bridge/contrib/hid-playstation-mic.patch ./usr/share/wireplumber/wireplumber.conf.d/90-dualsense.conf | sort)
[ "$got" = "$want" ] || { bad "the staged tree differs from the expected one (- expected, + installed):"; printf '%s\n' "$want" > "$tmp/.want"; printf '%s\n' "$got" | diff -u "$tmp/.want" - || true; }
grep -q '@' "$tmp/usr/lib/systemd/system/ds5bridge.service" && bad "the unit still has a placeholder"
grep -q '^ExecStart=/usr/bin/ds5bridge$' "$tmp/usr/lib/systemd/system/ds5bridge.service" || bad "the unit does not start /usr/bin/ds5bridge"
grep -q '^StartLimitIntervalSec=0' "$tmp/usr/lib/systemd/system/ds5bridge.service" || bad "the unit can end up stuck in failed"
"$tmp/usr/bin/ds5bridge" --version | grep -qx "ds5bridge $v_make" || bad "the installed ds5bridge does not report version $v_make"
# each recipe passes the vendor directories to make install (Arch and Debian spell them out, the spec uses macros)
for f in packaging/arch/PKGBUILD debian/rules; do
  for d in /usr/lib/systemd/system /usr/lib/udev/rules.d /usr/lib/modules-load.d /usr/share/wireplumber/wireplumber.conf.d /usr/share/wireplumber/main.lua.d; do grep -qF "$d" "$f" || bad "$f does not install into $d"; done
done
for m in '%{_unitdir}' '%{_udevrulesdir}' '%{_modulesloaddir}' '%{_datadir}/wireplumber/wireplumber.conf.d' 'WPSTYLE=0.5'; do grep -qF "$m" packaging/rpm/ds5bridge.spec || bad "the spec does not use $m"; done
grep -q 'WPSTYLE' debian/rules || bad "debian/rules does not choose the WirePlumber rule"
grep -q 'WPSTYLE=0.5' packaging/arch/PKGBUILD || bad "the PKGBUILD does not install the WirePlumber 0.5 rule"
grep -q 'ds5fw' packaging/rpm/ds5bridge.spec packaging/arch/PKGBUILD debian/rules && bad "a recipe mentions the firmware flasher, which is not installed"

# ---- license
head -2 LICENSE | grep -q 'GNU GENERAL PUBLIC LICENSE' || bad "LICENSE is not the GPL"
grep -q 'GPL-3+' debian/copyright || bad "debian/copyright does not declare GPL-3+"
grep -q '^License: *GPL-3.0-or-later' packaging/rpm/ds5bridge.spec || bad "the spec does not declare GPL-3.0-or-later"
grep -q "GPL-3.0-or-later" packaging/arch/PKGBUILD || bad "the PKGBUILD does not declare GPL-3.0-or-later"
for f in src/*.c src/*.h tools/*.c tools/*.sh tools/wasapi/* tests/*.c tests/*.h tests/*.sh Makefile service/* packaging/arch/PKGBUILD packaging/arch/ds5bridge.install packaging/rpm/ds5bridge.spec \
         debian/rules debian/ds5bridge.postinst debian/ds5bridge.prerm debian/ds5bridge.postrm .github/scripts/*; do
  [ -f "$f" ] || continue
  grep -q 'SPDX-License-Identifier: GPL-3.0-or-later' "$f" || bad "$f has no license header"
done

[ $fail = 0 ] && echo "test_packaging ok: one version ($v_make) in the Makefile, PKGBUILD, debian/changelog and the spec; the same installed tree for every packager; license headers"
exit $fail
