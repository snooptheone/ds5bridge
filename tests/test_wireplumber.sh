#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Anderson S.
# The pad's audio rule exists twice: service/90-dualsense.conf (WirePlumber 0.5+) and service/90-dualsense.lua (WirePlumber 0.4, Ubuntu 24.04). They must say the same thing: the same match patterns and the same
# property names and values. If a Lua interpreter is installed, the Lua file is also run against a stand-in for WirePlumber's rule table.
set -eu
cd "$(dirname "$0")/.."
fail=0
bad() { echo "FAIL $*"; fail=1; }
conf=service/90-dualsense.conf lua=service/90-dualsense.lua

# what each file matches and sets, in one normalised form: kind.name=pattern, key=value (the .conf's regex ".*" is the Lua glob "*")
from_conf() {
  sed -E 's/,[[:space:]]*$//' "$conf" | sed -nE \
    -e 's/^[[:space:]]*(device|node)\.name = "~(.*)"$/\1.name=\2/p' \
    -e 's/^[[:space:]]*"([^"]+)" = (.*)$/\1=\2/p' | sed -E 's/\.\*/*/g'
}
from_lua() {
  sed -E 's/,[[:space:]]*$//' "$lua" | sed -nE \
    -e 's/.*"(device|node)\.name", "matches", "([^"]*)".*/\1.name=\2/p' \
    -e 's/^[[:space:]]*\["([^"]+)"\] = (.*)$/\1=\2/p'
}
a=$(from_conf | sort); b=$(from_lua | sort)
[ -n "$a" ] || bad "found nothing in $conf"
[ "$a" = "$b" ] || { bad "the .conf and the .lua rules differ:"; diff -u /dev/fd/3 /dev/fd/4 3<<EOF1 4<<EOF2 || true
$a
EOF1
$b
EOF2
}
n=$(printf '%s\n' "$a" | grep -c '=')
[ "$n" -ge 10 ] || bad "expected at least 10 matches and properties, found $n"

# the Lua file against a stand-in for WirePlumber's alsa_monitor.rules
lua_bin=$(command -v lua lua5.4 lua5.3 luajit 2>/dev/null | head -1 || true)
if [ -n "$lua_bin" ]; then
  "$lua_bin" - "$lua" <<'LUA' || bad "the Lua rules do not behave as expected"
alsa_monitor = { rules = {} }
dofile(arg[1])
assert(#alsa_monitor.rules == 2, "two rules")
local card, node = alsa_monitor.rules[1], alsa_monitor.rules[2]
assert(card.matches[1][1][1] == "device.name" and card.apply_properties["api.alsa.use-ucm"] == false, "card rule")
assert(card.apply_properties["device.profile-set"] == "analog-surround-40.conf", "profile set")
assert(node.matches[1][1][1] == "node.name" and node.apply_properties["node.description"] == "Wireless Controller", "node rule")
assert(node.apply_properties["audio.rate"] == 48000 and node.apply_properties["channelmix.disable"] == true, "node properties")
LUA
  how="and run against a stand-in rule table by $(basename "$lua_bin")"
else
  how="(no Lua interpreter here: the Lua file was compared textually only)"
fi
[ $fail = 0 ] && echo "test_wireplumber ok: the .conf (0.5+) and the .lua (0.4) rules match on $n patterns and properties $how"
exit $fail
