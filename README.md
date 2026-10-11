# ds5bridge

Makes a **Bluetooth DualSense look like a USB DualSense** to the system, so that Proton/Wine games that talk to the pad natively get
**adaptive triggers, HD haptics and the controller speaker over Bluetooth** on Linux.

Games that use Sony's own pad library (libScePad, e.g. Returnal) only use those features over USB. The Linux kernel driver (`hid-playstation`)
supports neither triggers nor haptics over Bluetooth, and Proton only links a game's audio endpoints to a controller when both belong to the same
real USB device. ds5bridge fixes that without touching the game or the kernel:

```
  Game (Proton/Wine)                                          Real pad (Bluetooth)
        │ sees a normal DualSense on USB                             ▲   │ HID reports 0x31 / 0x39
        ▼                                                            │   ▼
  ┌──────────────┐   USB/IP over an in-process socketpair   ┌────────┴────────────┐
  │ vhci_hcd     │◄────────────────────────────────────────►│ ds5bridge (root)    │
  │ virtual USB  │   control, HID, 4-channel audio          │ systemd service     │
  │ DualSense    │                                          └─────────────────────┘
  └──────────────┘
```

* **Input** (sticks, buttons, touchpad, gyro): BT report `0x31` → USB report `0x01`.
* **Output** (rumble, LEDs, **adaptive triggers**): USB report `0x02` → BT report `0x31` with its CRC.
* **Haptics**: audio channels 3-4 → low-pass FIR (385 taps) → 3 kHz int8 stereo → BT report `0x39`.
* **Speaker / headphone**: audio channels 1-2 → Opus (160 kbps) → the same `0x39` report, paced at 46.9 packets/s.
* **Bluetooth QoS**: an HCI QoS Setup raises the pad's report rate from ~70/s to ~300/s on the adapter this was developed on. The daemon logs the measured input rate after the first 10 s of a connection (`pad input: N reports/s`) and again whenever it moves by a quarter.
* **Trigger effects and LEDs survive a Bluetooth drop**: what the game set is remembered (even while the pad is away) and replayed when the pad comes back inside the grace period, a second after it is back so that the kernel's own start-up reports (the order was not measured) should go first.
* The virtual pad exists **only while the real pad is connected** (3 s grace for short Bluetooth drops), so there is no ghost controller.
* **Several pads at once** (up to 4): every Bluetooth DualSense gets its own virtual USB pad on its own vhci port, its own audio, idle timer, QoS and replay state; the log lines carry the end of the pad's address (`[9d:df:27]`). Each virtual pad serves its pad's own address with the "locally administered" bit flipped (never equal to any real pad's address, which `hid-playstation` would refuse), so the host sees them as different controllers.
* **Audio route**: the speaker route and volume the game sets (Wine sends `audio_control` 0x3c and speaker volume 0x64) are remembered and sent again before the speaker starts after every silence and after a Bluetooth drop. `--output speaker|headset|off` and `--speaker-volume N` override them.
* **The WirePlumber rule is shipped as required**: it has been in use on both development machines since the research phase, and the pad was never tried without it (the project owner decided to ship it). The 0.5+ `.conf` was checked to load in a real WirePlumber 0.5.18 (it is what the development machine runs, where the Lua file made WirePlumber warn) and the 0.4 `.lua` in a real 0.4.17 (Ubuntu 24.04), both in a container without a sound card, so only that they are accepted, not that they change the pad's sink; `tests/test_wireplumber.sh` keeps the two files saying the same thing. The 0.4 `.lua` has not run against a real pad.
* **Battery**: logged when a connection starts and when its level or charging state changes (a warning at 15 % or less); `ds5ctl battery` prints it.
* **Idle power-off**: the pad is switched off after 15 minutes without input or haptics (`--idle-off`, 0 = never).

No `usbip` tool is needed: the daemon hands one end of a `socketpair` to `vhci_hcd` through its sysfs `attach` file. The kernel side still speaks the USB/IP wire protocol over that socket (that is how `vhci_hcd` talks to anything); the daemon implements the device end of it, in `src/ds5bridge.c`.

## Status

Developed and tested on one controller (DualSense `054c:0ce6`), CachyOS, kernel 7.2, Intel Bluetooth, GE-Proton.

| Verified on hardware | Not verified |
|---|---|
| Adaptive triggers over BT in a game (Returnal), haptics and speaker via a WASAPI test client (felt, clean; the speaker tone test was dropped once it worked). **A full Returnal session over Bluetooth with the installed service**: triggers, haptics and the controller speaker worked in the game, no warnings in the log, input about 390 reports/s, audio 46.9 packets/s | DualSense Edge (`0df2`), other adapters (the QoS command may be rejected) |
| Service starts when the pad connects; virtual pad attached through vhci_hcd; QoS applied; the real pad's nodes hidden from the user; the sandboxed systemd unit (`ProtectSystem=strict`, `RestrictAddressFamilies`...) runs the daemon end to end (the pad's feature reports are read in 30 ms) | The replay of trigger effects after a real Bluetooth drop (tested with a fake pad only) |
| The virtual pad is removed 3 s after the pad goes away and comes back on reconnect; idle power-off (60 s measured with `--idle-off 1`) | Flashing firmware (`ds5fw --flash`); only the dry run is checked |
| The FIR haptics filter: a 2.5 kHz test tone is felt as a ~500 Hz buzz with the old 16-sample average and not at all with the FIR | **Microphone**: the pipeline exists (`--mic`) but this pad's mic produced no voice signal, over Bluetooth or USB. Off by default |
| The pad accepts at least 333 output reports/s (78-byte state reports, QoS applied) with no write error and input flowing at ~415/s (`ds5ctl stress`) | The output-report limit without QoS (adapters that reject the QoS command) |

Packet builders and the `ds5ctl`/`ds5fw` outputs are checked byte for byte against the original Python implementations (`tests/golden*`).

## Install

Packages for Arch Linux, Ubuntu/Debian and Fedora are built by the [release workflow](.github/workflows/release.yml) for every version tag and attached to the
[Releases](https://github.com/snooptheone/ds5bridge/releases) page (with a `SHA256SUMS` file). All of them install the same things: `ds5bridge` and `ds5ctl`
in `/usr/bin`, the systemd unit, the two udev rules, the module list that loads `vhci_hcd` at boot, and the docs. The firmware flasher `ds5fw` is not installed.

**You need** a kernel with the `vhci_hcd` module (`CONFIG_USBIP_VHCI_HCD`) and a working Bluetooth stack. Check with `sudo modprobe vhci_hcd` (no output = fine).
Ubuntu keeps the USB/IP modules in `linux-modules-extra-$(uname -r)`; install that if the command fails (not checked here: the packages were built and installed in clean containers, not run on those kernels).

**The audio rule (WirePlumber).** The packages and `make install` also install a small WirePlumber rule that gives the pad's audio the shape Proton needs: the plain four-channel `analog-surround-40` profile (not the UCM
one, which splits the pad into a hidden parent and a one-channel speaker sink), a sink called "Wireless Controller", S16LE at 48 kHz, channels passed through unmixed. WirePlumber reads its configuration only
when it starts, so **restart it once after installing**, as your user: `systemctl --user restart wireplumber` (or log out and in). It is `90-dualsense.conf` for WirePlumber 0.5 and later and `90-dualsense.lua` for
0.4 (Ubuntu 24.04): only the one for your WirePlumber version is installed (0.5 prints "Old configuration file detected" for every Lua file it finds). `make install` looks at the installed
WirePlumber (`WPSTYLE=0.5|0.4` to choose by hand); the Ubuntu 24.04 package carries the Lua file, the others the `.conf`. Packages put it in `/usr/share/wireplumber/`, `make install` in `/etc/wireplumber/`. If you made your own copy in `~/.config/wireplumber/`, delete it: the
rule now comes with the package. Nothing here applies to PulseAudio without PipeWire.

After installing, **reconnect the pad** (PS button): the udev rule starts `ds5bridge.service` when a Bluetooth DualSense connects. Logs: `journalctl -u ds5bridge -f`; battery: `sudo ds5ctl battery`.
If you use Steam Input, turn it off for the games you want native DualSense features in: Steam Input replaces the native pad.

### Arch Linux

**From the pacman repository** (updates with `pacman -Syu`), signed with a dedicated key. The repository is kept in the [`repo` release](https://github.com/snooptheone/ds5bridge/releases/tag/repo) and is created by the first
tagged release once the signing key is set up (`tools/setup-signing-key.sh`, see [docs/RELEASING.md](docs/RELEASING.md)); the fingerprint will be published here, and you must check it before trusting the key.

1. Download the key and check that the fingerprint printed matches the one published in this README:

```bash
curl -fsSLO https://github.com/snooptheone/ds5bridge/releases/download/repo/ds5bridge.gpg
```
```bash
gpg --show-keys --fingerprint ds5bridge.gpg
```

2. Trust it (replace `FINGERPRINT` with the 40 characters printed above, no spaces):

```bash
sudo pacman-key --add ds5bridge.gpg
```
```bash
sudo pacman-key --lsign-key FINGERPRINT
```

3. Add the repository at the end of `/etc/pacman.conf`:

```ini
[ds5bridge]
SigLevel = Required DatabaseRequired
Server = https://github.com/snooptheone/ds5bridge/releases/download/repo
```

4. Install:

```bash
sudo pacman -Syu ds5bridge
```

**Or build it yourself** (clones the release tag):

```bash
git clone https://github.com/snooptheone/ds5bridge.git
cd ds5bridge/packaging/arch
makepkg -si
```

**Or install the package from a release** (single versions are not signed; compare the file with `SHA256SUMS`): `sudo pacman -U ds5bridge-0.1.0-1-x86_64.pkg.tar.zst`.

### Ubuntu and Debian

Download the `.deb` for your release from the Releases page (`ubuntu24.04` or `ubuntu26.04` in the name; the same version is built on each. Those two are the tested releases; other Debian-based systems are not tested) and install it with apt, which also pulls the dependencies (`libopus0`, `kmod`, `udev`):

```bash
sudo apt install ./ds5bridge_0.1.0_amd64_ubuntu24.04.deb
```

Remove it with `sudo apt remove ds5bridge` (the real pad gets its normal permissions back).

### Fedora and other RPM distributions

```bash
sudo dnf install ./ds5bridge-0.1.0-1.fc44.x86_64.rpm
```

### Any other distribution: install by hand

You need a C compiler, `make` and the libopus development files (`libopus-dev` on Debian and Ubuntu, `opus-devel` on Fedora, `opus` on Arch). Then:

```bash
git clone https://github.com/snooptheone/ds5bridge.git
cd ds5bridge
make
make test          # optional: no hardware, no root
sudo make install  # /usr/local/bin, /etc/systemd/system, /etc/udev/rules.d, /etc/modules-load.d
```

`sudo make uninstall` removes it again. To install into the vendor directories, as the packages do, pass them to make:

```bash
sudo make install PREFIX=/usr SYSTEMDDIR=/usr/lib/systemd/system UDEVDIR=/usr/lib/udev/rules.d MODULESDIR=/usr/lib/modules-load.d
```

(`DESTDIR=` stages the files into a directory without touching the running system, which is what package builds use.)

### What gets installed, and what it does to the system

The udev rules start `ds5bridge.service` when a Bluetooth DualSense connects and make the **real pad's `hidraw`/`event`/`js` nodes root-only**
(ignored by libinput), so Steam and games see only the virtual USB pad. A side effect: tools that talk to the real Bluetooth pad (`ds5ctl`) need `sudo`. The rules only
hide the pad while `vhci_hcd` is loaded, so a system without the module keeps a working pad. The unit has no `[Install]` section: nothing is enabled, udev starts it.

## Build and test

```sh
make              # bin/ds5bridge  bin/ds5ctl  bin/ds5fw
make test         # unit tests, golden packets, kernel-side protocol test, lifecycle, multi-pad and audio-route tests and ds5ctl test with fake pads, option, tool, install and packaging tests (no hardware, no root)
make asan         # the C tests (unit, protocol, lifecycle, multi-pad, route, ds5ctl) under AddressSanitizer + UBSan
```

Packages from source (what the release workflow runs): Arch `cd packaging/arch && makepkg -f`; Debian and Ubuntu `dpkg-buildpackage -us -uc -b`; Fedora `rpmbuild -ba packaging/rpm/ds5bridge.spec` (with the source tarball
from `git archive`, see the header of the spec). `tests/test_packaging.sh` checks that the versions and the installed tree agree. Optional: `clang` + `lld` + wine import libs build the Windows test client (`make wasapi`).

## Tools

```sh
ds5bridge --help                          # the daemon (normally run by systemd)
sudo ds5ctl trigger right feedback 3 6    # adaptive trigger effects: off | feedback | weapon | vibration; --dry-run prints the report
sudo ds5ctl battery                       # battery level and charging state
sudo ds5ctl power-off                     # power the Bluetooth pad off
sudo ds5ctl rate 3                        # input reports/s of the Bluetooth pad
sudo ds5ctl stress                        # how many output reports/s the Bluetooth pad takes (stop ds5bridge first; the pad may drop at its limit)
bin/ds5fw                                 # firmware flasher, not installed (run it from the build folder): lists USB pads; bin/ds5fw FILE.bin checks an image; --dry-run / --flash (see the header of tools/ds5fw.c)
make wasapi && tools/wasapi/run.sh        # Windows test client under GE-Proton: a haptic test signal on channels 3-4 (arguments: speaker, highband)
```

## Safety

The pad has factory/test commands reachable by HID feature reports (erase device info, DFU modes, firmware and BT patches...). ds5bridge **never
forwards a feature SET from the host to the real pad**: the virtual pad acknowledges and drops it (checked by the tests). The only feature report the
daemon itself sends is the Bluetooth-control power-off, and only when the idle timer fires. `ds5fw --flash` is the one tool that writes firmware, and
it refuses an image whose product id (offset `0x62`) is not the connected controller's, and asks you to type `FLASH`; its protocol was reverse engineered from Sony's updater and has not been exercised on hardware by this project.

## Layout

`src/ds5bridge.c` the daemon · `src/ds5proto.h` shared protocol helpers · `src/descriptors.h` the (device independent) USB/HID descriptors ·
`tools/` ds5ctl, ds5fw, the WASAPI client · `service/` systemd unit and udev rules · `tests/` (the unit tests include the daemon's source, so the shipped binary has no test code) · `contrib/` an optional kernel patch (needed only with `--mic`) ·
`docs/PROTOCOL.md` what was learned about the protocol.

## Known limits and roadmap

* **Up to 4 pads**, tested with fake pads only (`tests/test_multipad.c`: slow pad, drop and return, a fifth pad refused, a freed slot reused): the second real pad has not been tried on hardware. A fifth pad is ignored with a warning until a slot frees up. A new connection reads the pad's feature reports in a thread, so a pad that answers slowly does not stall the others.
* **Battery**: only logged and printed. A desktop notification for a low battery would need a bridge from the root service to the user session (not done). The table is hid-playstation's: DualSenseClient reads "charging" as `(level - 1) * 10`, the kernel as `level * 10 + 5`.
* **Not done**: embedding the game's output state in the 0x39 audio packets and synthesising rumble (DualSenseClient says the pad ignores a standalone 0x31 output report while the audio lane is open; not seen here, see `docs/PROTOCOL.md`), haptics derived from the voice channels for games without native haptics, a combined 0x36 report.
* Output reports from the game are written to the pad as they come, with no limiter: `ds5ctl stress` found no problem up to 333 reports/s with QoS applied, far above what a game sends. Not measured without QoS.
* Only a Bluetooth DualSense is bridged; a pad on USB is used natively and does not need ds5bridge.
* The pad's microphone is off by default and unproven (see Status). The kernel patch in `contrib/` is only for `--mic`.
* Hardened build (PIE, RELRO, stack protector, `_FORTIFY_SOURCE=2`): the daemon runs as root. A debug USB/IP port (`--port`) drops clients that send oversized or too many requests.

## Troubleshooting: Proton and the controller speaker

The game opens the controller's audio as **two** four-channel streams (one for the haptic actuators, one for the speaker). If the speaker is silent while triggers and haptics work, look at `pactl list sink-inputs`: you should see two `float32le 4ch` streams going to the DualSense sink, and the USB capture (`usbmon`) shows silence in channels 1-2.

On the development machine this was caused by **a Wine prefix that earlier tests had filled with stale DualSense audio endpoints** (177 endpoint device nodes, 19 different container ids, endpoint names wrapped six times). A fresh prefix fixed it. GE-Proton 11-1 and Proton Experimental (11.0-100) both work with a clean prefix; a prefix that ran a test with a different Proton, Bluetooth pads, the virtual pad and a hot-plugged pad is not clean. While testing, restore a clean prefix before every run and compare the registry afterwards (`system.reg`: the number of `Enum\SWD\MMDEVAPI` nodes and of `MMDevices\Audio\Render` endpoints for the pad; a healthy prefix has none and one).

Wine's container id of a USB device is built from the vendor/product id, `BUSNUM`, `DEVNUM` and `USEC_INITIALIZED` of the USB device (`bus_udev.c`), so every new virtual pad (every Bluetooth reconnect after the grace period) is a new container, exactly like replugging a cable. With GE-Proton 11-1 this did not pile up: a second machine that attached a virtual pad (usbip) eleven times in one day still had a single speaker endpoint. `usbip bind/unbind` on the machine that owns the pad is only a device *reset* (same device number), so it does not change the container there.

## Credits

The Bluetooth audio/haptics framing comes from reading [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) (and its fork
[artzox/DS5Dongle-Studio](https://github.com/artzox/DS5Dongle-Studio)); output report fields and power-off from [nowrep/dualsensectl](https://github.com/nowrep/dualsensectl),
[WujekFoliarz/duaLib](https://github.com/WujekFoliarz/duaLib) and the kernel's `hid-playstation`; command names from the PS5 Developer wiki.
No code was copied; the project reimplements the protocol facts.

## License

ds5bridge is free software under the [GNU General Public License v3.0 or later](LICENSE) (`GPL-3.0-or-later`). `contrib/hid-playstation-mic.patch` is a patch to a Linux kernel file and stays under that file's license (GPL-2.0).
