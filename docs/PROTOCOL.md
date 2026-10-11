# What was learned (DualSense over Bluetooth and USB/IP)

Everything here was either read from the projects listed in the README or measured on one DualSense. "Unverified" marks claims without a test on hardware.

## Bluetooth (hidraw of the pad, report ids include the first byte)

CRC: every output report ends with `crc32` little endian over `seed || report[0..n)`. Seeds: `0xA1` input, `0xA2` output, `0xA3` feature (`hid-playstation`),
`0x53` for feature `0x08` (Bluetooth control, used by `dualsensectl power-off`).

| Report | Size | Layout |
|---|---|---|
| input `0x31` | 78 | byte 1 bit 0 = carries pad state (payload at byte 2, same layout as USB `0x01` from its byte 1), bit 1 = carries microphone audio (71 B Opus at byte 3) |
| output `0x31` | 78 | `[0x31, seq<<4, 0x10, 47 B "common" block, zero padding to 74, crc]` (the USB `0x02` body is the same common block) |
| output `0x32` | 142 | `[0x32, seq<<4, 0x91, 1, flags]`: `flags` 0b11 switches the pad microphone stream on, 0b10 off |
| audio `0x39` | 547 | `[0x39, seq<<4, 0x91, 6, flags, buflen x4, counter, 0xD2, 64, 2 x 64 B haptics, (0xD3 or 0xD6), 200, 2 x 200 B Opus, crc]` |

The common block (offsets inside it): `0` valid_flag0, `1` valid_flag1, `4` headphone volume, `5` speaker volume, `6` mic volume (0..0x40), `7` audio control, `8` mute LED,
`9` power save control, `10..20` right trigger effect, `21..31` left trigger effect, lightbar around `44..46`. Flag bits are in `hid-playstation` and `dualsensectl`.

* **Adaptive triggers**: effect modes `0x05` off, `0x21` feedback, `0x25` weapon, `0x26` vibration (zone bit masks plus 3-bit strengths); set `valid_flag0` bit 2 (right) / 3 (left).
  Worked over Bluetooth on this pad, including in a game. Mode and zone encoding come from community documentation.
* **Haptics** (`0xD2` sub-packet): 2 blocks of 64 bytes = 32 stereo frames of **int8 at 3 kHz** each, one `0x39` packet per 21.33 ms. The format is fixed by the pad.
  ds5bridge builds it from the game's 48 kHz channels 3-4 with a **385-tap Kaiser-windowed-sinc low-pass FIR** (beta 5.65, cutoff 1250 Hz) and decimation by 16:
  flat within 0.01 dB up to 1 kHz, -0.4 dB at 1.1 kHz, at least 59 dB down from 1.5 kHz (the new Nyquist), 4 ms of delay, unity gain at DC. DS5Dongle uses a
  linear-interpolation resampler without a filter and truncates; a plain 16-sample average (the first version of this project) is 1.6 dB down at 1 kHz and leaves
  tones above 1.5 kHz only 14-22 dB down, so they alias into the haptic band. The golden packets (`tests/golden.txt`) do not include the haptics path because of that change; the
  unit test checks the response instead (passband amplitude and four tones that would alias).
  Felt on hardware, side by side with the old 16-sample average (160 Hz / 2.5 kHz / silence cycles, the `highband` mode of `tools/wasapi`): the 2.5 kHz part left a ~500 Hz buzz
  with the average and nothing with the FIR. The average was then removed; the unit test keeps the FIR's passband and stopband honest.
* **Speaker / headphone** (`0xD3` speaker, `0xD6` headphone): 48 kHz stereo, 10 ms frames, **CBR 160 kbps Opus = exactly 200 bytes**; each packet carries two frames.
  The dongle feeds 512 samples as 480 by linear interpolation (a ~6.7 % speed-up that keeps the pad's buffer from underrunning); ds5bridge does the same.
  Sending each packet as soon as audio arrives is choppy because the host delivers audio in 8 ms bursts: a pacer sends at a steady 21.33 ms with a 2-packet prebuffer.
  The `buflen` field (`--audio-buffer`, default 100) mattered: the dongle's 48 was choppy on this setup.
* The sequence nibble is shared by all `0x31/0x32/0x39` output reports.
* **Report rate**: by default the pad sent only ~70 input reports/s in bursts every ~25 ms, regardless of what the host sent. An HCI *QoS Setup* (latency 5000 us) on the
  ACL link raised it to ~300/s on this adapter (the command's status was 0). It is lost at every reconnect, so ds5bridge reapplies it. Needs `CAP_NET_RAW` (root).
* **Feature reads**: the pad switches from the short `0x01` input report to the full `0x31` after a feature read of `0x05` (calibration). `0x05` (41 B), `0x09`
  (pairing info with the MAC, 20 B) and `0x20` (firmware info, 64 B) are what `hid-playstation` reads at probe; ds5bridge reads them from the real pad and serves them
  on the virtual one (with the MAC's locally administered bit flipped, bit 1 of its first octet = byte 6 of the little-endian address in the report: `hid-playstation` refuses a device whose MAC any registered pad already has, so the virtual address must differ from every real one; flipping the lowest bit would collide with a second pad whose address differs only there).
* **Power-off**: feature report `0x08` with byte 1 = 2 and the `0x53` CRC. Verified by ds5bridge on a real pad: after `--idle-off 1` (60 s without activity) the report was sent and the pad dropped the link 13 ms later.
* Flooding the pad with high-rate writes (e.g. 250 Hz of `0x32`) dropped its HID channel (`Errno 5`); that run was with the 142-byte mic report and no QoS. `ds5ctl stress` (78-byte state reports, QoS applied) found no write error from 20 up to 333 reports/s, the highest rate tried, with input reports at ~415/s and gaps of at most 50 ms, so ds5bridge does not rate-limit the game's output reports. The limit without QoS was not measured.

## USB/IP and the virtual device

* `vhci_hcd`'s sysfs `attach` takes `"port sockfd devid speed"` and only requires a `SOCK_STREAM` socket (checked in the kernel source), so an `AF_UNIX` `socketpair`
  works and no TCP, handshake or `usbip` tool is needed. `devid` = `(busnum << 16) | devnum`; speed 3 = high. Needs root (the file is 0200). Detach: write the port to `detach`.
* The server answers `CMD_SUBMIT` / `CMD_UNLINK` for the control endpoint, a HID interrupt IN (a parked URB answered at ~250 Hz), an interrupt OUT, and isochronous
  OUT (4-channel speaker, 16-bit 48 kHz) and IN (2-channel microphone). Isochronous URBs are completed after the time their packets would take on the wire,
  so the host audio clock runs at real time. `dummy_hcd` (configfs gadget) has no isochronous endpoints, which is why `vhci_hcd` is used.
* Descriptors are copied from a real pad over USB (UAC1 4-channel speaker `wChannelConfig 0x33`, 2-channel mic, HID report map of 289 bytes). Interface 2 is the mic stream,
  the HID interface is number 3.
* Proton/Wine only links a device's audio endpoints to the HID device through the USB container (`ContainerId`); a virtual USB device satisfies that, a uhid device
  (which wine accepts only with bus 6) does not carry audio.

## Idle power-off

Activity = a stick moving more than 12/255 from where it was at the last activity, a trigger above 10, any d-pad/face/shoulder/stick/options/PS/touchpad/mute button,
a touchpad contact, or host audio above 200/32768 on any channel (so haptics during a cutscene count). Gyro and accelerometer are ignored (they never rest).

## Firmware updater (`ds5fw`)

Reverse engineered from Sony's FWupdater and cross-checked with `dualsensectl`'s updater: feature `0xF4` `[F4, stage, len, data[57], pad[4]]`, status in `0xF5`
`[F5, stage, status]`; stage 0 header (first 0x100 bytes), 1 image (0x8000-byte blocks of 57-byte chunks, poll after each), 2 verify, 3 finalize (reboot). Image size
950272 bytes; PID at offset `0x62`, version at `0x78` (`dualsensectl`'s compatibility check, which `ds5fw` repeats: it refuses an image for another product id before sending anything). The controller validates a CMAC and the version itself.

## Factory/test commands (never forwarded)

The PS5 Developer wiki lists `0x80`/`0x81` (63 B command/response) commands: device info read/erase, Spider DSP and Venom firmware, Bluetooth patches, DFU modes (`0xA0`),
NVS lock, BT address. ds5bridge drops every feature SET from the host. A read-only `GET_FEATURE` of `0x81` without a prior command returns zeros, so there is no status to read.

## Microphone (unresolved)

The pipeline works: `0x32` enable, gain/internal-mic select via the common block, Opus 71-byte frames decoded and delivered to the host. With QoS the pad delivers ~100 frames/s
(without it ~35/s, because the mic frames share the ~70 reports/s budget). On the tested pad the decoded signal was a flat noise floor that scaled with the gain and never
reacted to voice, clapping or scraping, over Bluetooth and over USB (6 combinations of internal mic, noise cancel, beam forming and input path were tried over USB). This points to the
pad's mic hardware rather than to the protocol; it was not tested in another system. Side effect worth knowing: with the mic on, stock `hid-playstation` parses the mic reports as
pad state (see `contrib/`), and Steam does too (ghost clicks).

## What a real game does over USB (usbmon captures of Returnal under Proton, one real USB pad)

* The game never sets the rumble motors: bytes 3-4 of every output report `0x02` stay 0. "Rumble" is the haptic audio on channels 3-4 of the 4-channel isochronous OUT stream (16 bit, 48 kHz, 384 B per ms).
* The output reports are only three kinds: `02 80 40 ... [8]=0x3c` (audio control: speaker path + echo/noise bits), `02 70 40 ... [6]=0x64` (headphone volume 0, speaker volume 0x64, microphone volume 0) and trigger effects (`0x25` weapon, `0x21` feedback, `0x05` off). The audio ones come from Wine's DualSense code, not from the game.
* Speaker sound is in channels 1-2 of the *second* four-channel stream; when the speaker works, channels 1-2 are non-zero in about half of the seconds of play.
* The pad's input status bytes (USB byte 53 battery, 54 headphone/mic/USB flags) are the same on both machines: `0x18 0x08` (charging, level 8; USB data).
* hid-playstation and Steam also send reports in the first seconds (`02 0d 17 ...`, audio control `0x0c` = headphone path); the game's `0x3c` replaces it within 0.2 s.
* The GE-Proton patches (`proton-ds5-haptic`, 0132/0133/0149/0150) say the pad can drop its speaker route when the PCM is opened, and fix it by sending the route (output path, speaker volume, pre-gain) again after the PCM opens. ds5bridge does the same over Bluetooth before the first speaker packet after each silence (not measured whether the pad needs it).

## Battery

Status byte of the input report (USB 0x01 byte 53, Bluetooth 0x31 byte 54): low nibble = charge in 10 % steps (0-10), high nibble = state. hid-playstation: `0` discharging and `1` charging both give `min(level * 10 + 5, 100)`, `2` full gives 100, `0xA`/`0xB` out of range give 0, anything else is an error. (DualSenseClient uses `(level - 1) * 10` for charging.)

## Not done / ideas

Edge pad; headphone-jack handling beyond the 0xD6 header; audio-derived haptics for games without native haptics; whether the output state (triggers, LEDs, rumble) is
ignored while the 0x39 audio lane is open, as DualSenseClient claims (then it would have to be embedded in the audio packets, with the 0x90 0x3F + 63 B state sub-packet and a
synthesised 60/180 Hz rumble); a low-battery desktop notification.
