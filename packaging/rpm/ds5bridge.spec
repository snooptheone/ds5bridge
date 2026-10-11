# SPDX-License-Identifier: GPL-3.0-or-later
# Build: tarball from `git archive --prefix=ds5bridge-VERSION/ -o ds5bridge-VERSION.tar.gz vVERSION`, then
#   rpmbuild -ba packaging/rpm/ds5bridge.spec --define "_sourcedir DIR_WITH_THE_TARBALL"
Name:           ds5bridge
Version:        0.1.0
Release:        1%{?dist}
Summary:        Bluetooth DualSense as a virtual USB DualSense
License:        GPL-3.0-or-later
URL:            https://github.com/snooptheone/ds5bridge
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  opus-devel
BuildRequires:  systemd-rpm-macros
Requires:       kmod
Requires:       systemd
Requires:       systemd-udev
Recommends:     bluez
Recommends:     wireplumber

%description
Makes a Bluetooth DualSense look like a USB DualSense to the system, so that
Proton and Wine games that talk to the pad natively get adaptive triggers,
HD haptics and the controller speaker over Bluetooth on Linux.

A root daemon serves a virtual USB pad to the kernel's vhci_hcd and bridges it
to the real pad. It starts by itself when a pad connects (udev), hides the real
Bluetooth pad from the user while it runs, and removes the virtual pad when the
real one goes away. Up to four pads at once. Includes ds5ctl (battery, trigger
effects, power-off).

%prep
%autosetup

%build
%make_build VERSION=%{version}

%check
make VERSION=%{version} test

%install
make install VERSION=%{version} DESTDIR=%{buildroot} PREFIX=%{_prefix} \
    SYSTEMDDIR=%{_unitdir} UDEVDIR=%{_udevrulesdir} MODULESDIR=%{_modulesloaddir} \
    WPCONFDIR=%{_datadir}/wireplumber/wireplumber.conf.d WPLUADIR=%{_datadir}/wireplumber/main.lua.d WPSTYLE=0.5 \
    DOCDIR=%{_docdir}/%{name}

%post
udevadm control --reload >/dev/null 2>&1 || :
modprobe vhci_hcd >/dev/null 2>&1 || echo "ds5bridge: could not load the vhci_hcd kernel module; the virtual pad needs it"
echo "ds5bridge: restart WirePlumber once so that it reads the pad audio rule (as your user: systemctl --user restart wireplumber), then reconnect the DualSense (PS button). Logs: journalctl -u ds5bridge -f"

%preun
if [ $1 -eq 0 ]; then
    systemctl stop ds5bridge.service >/dev/null 2>&1 || :
fi

%postun
if [ $1 -eq 0 ]; then
    # a pad that is connected right now gets its normal permissions back
    udevadm control --reload >/dev/null 2>&1 || :
    udevadm trigger --subsystem-match=hidraw --subsystem-match=input --action=change >/dev/null 2>&1 || :
fi

%files
%license LICENSE
%doc %{_docdir}/%{name}
%{_bindir}/ds5bridge
%{_bindir}/ds5ctl
%{_unitdir}/ds5bridge.service
%{_udevrulesdir}/71-ds5bridge.rules
%{_udevrulesdir}/72-ds5bridge-hide.rules
%{_modulesloaddir}/ds5bridge.conf
%{_datadir}/wireplumber/wireplumber.conf.d/90-dualsense.conf

%changelog
* Sun Oct 11 2026 Anderson S. <4625164+snooptheone@users.noreply.github.com> - 0.1.0-1
- Initial release.
