#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Anderson S.
# Runs wasapi_test.exe (WASAPI haptic test signal on channels 3-4 of the DualSense endpoint, like a game does) under GE-Proton's wine, in its own
# prefix (~/.cache/ds5test-prefix), so it exercises the same path a Proton game uses without touching any game prefix. Build it first: make wasapi
#   tools/wasapi/run.sh             haptics test signal on channels 3-4, the speaker stays silent
#   tools/wasapi/run.sh speaker     also a 440 Hz tone on channels 1-2 (the controller speaker)
#   tools/wasapi/run.sh highband    160 Hz / 2.5 kHz / silence cycles: the 2.5 kHz part must not be felt (a hardware check of the haptics filter)
HERE=$(cd "$(dirname "$0")" && pwd)
GE=$(ls -d "$HOME"/.local/share/Steam/compatibilitytools.d/GE-Proton*-x86_64/files 2>/dev/null | tail -1)
[ -x "$GE/bin/wine" ] || { echo "GE-Proton not found under compatibilitytools.d"; exit 1; }
[ -f "$HERE/../../bin/wasapi_test.exe" ] || { echo "build it first: make wasapi"; exit 1; }
export WINEPREFIX="$HOME/.cache/ds5test-prefix" WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all
[ -d "$WINEPREFIX" ] || { echo "creating wine prefix (first run, ~20 s)"; "$GE/bin/wine" wineboot -i >/dev/null 2>&1; }
exec "$GE/bin/wine" "$HERE/../../bin/wasapi_test.exe" "$@" 2>&1 | grep -v -i -E "mesa|pci id|dri2|fixme|ntsync"
