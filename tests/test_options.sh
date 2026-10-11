#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Anderson S.
# every option must be parsed once: a value is consumed exactly once and clamped (a macro that evaluated its argument twice once ate the next argument)
set -eu
cd "$(dirname "$0")/.."
got=$(bin/ds5bridge --idle-off 7 --grace 2.5 --audio-buffer 80 --qos-latency 0 --haptics-gain 1.5 --mic --mic-volume 30 --mic-gain 2 --port 4000 --no-speaker --print-config)
want="attach=1 port=4000 mic=1 no_speaker=1 no_haptics=0 audio_buffer=80 mic_volume=30 mic_gain=2.00 qos_us=0 idle_off_min=7 grace_s=2.5 haptics_gain=1.50 output=0 speaker_volume=-1"
[ "$got" = "$want" ] || { echo "FAIL options"; echo "  want: $want"; echo "  got:  $got"; exit 1; }
got=$(bin/ds5bridge --audio-buffer 999 --mic-volume 99 --haptics-gain 9 --idle-off -5 --print-config)   # clamped; value then the next flag still parsed
case "$got" in *"audio_buffer=127 mic_volume=64"*"idle_off_min=0"*"haptics_gain=2.00"*) ;; *) echo "FAIL clamping: $got"; exit 1;; esac
got=$(bin/ds5bridge --output headset --speaker-volume 999 --print-config)   # words and numbers; the volume is clamped to a byte
case "$got" in *"output=2 speaker_volume=255") ;; *) echo "FAIL --output/--speaker-volume: $got"; exit 1;; esac
got=$(bin/ds5bridge --output speaker --speaker-volume 80 --print-config)
case "$got" in *"output=1 speaker_volume=80") ;; *) echo "FAIL --output speaker: $got"; exit 1;; esac
got=$(bin/ds5bridge --output off --print-config)                            # off is the old --no-speaker
case "$got" in *"no_speaker=1"*"output=0 speaker_volume=-1") ;; *) echo "FAIL --output off: $got"; exit 1;; esac
if bin/ds5bridge --output loud --print-config >/dev/null 2>&1; then echo "FAIL --output accepted a bad word"; exit 1; fi
[ "$(bin/ds5bridge --version)" = "ds5bridge $(sed -n 's/^VERSION ?= \([0-9.]*\).*/\1/p' Makefile)" ] || { echo "FAIL --version"; exit 1; }   # the Makefile's VERSION reaches the binary
[ "$(bin/ds5ctl --version)" = "ds5ctl $(sed -n 's/^VERSION ?= \([0-9.]*\).*/\1/p' Makefile)" ] || { echo "FAIL ds5ctl --version"; exit 1; }
echo "test_options ok: --version, numeric options parsed once and clamped, --output and --speaker-volume"
