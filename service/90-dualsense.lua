-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copyright (C) 2026 Anderson S.
-- WirePlumber 0.4 (Lua configuration, e.g. Ubuntu 24.04). WirePlumber 0.5 and later ignore this file and read 90-dualsense.conf, which says what the rules are for.
-- Not run against a real 0.4 yet: the property names are the ones of the .conf (tests/test_wireplumber.sh checks that), the matching is the 0.4 glob form.
-- WirePlumber reads its configuration once at start: after installing this file run `systemctl --user restart wireplumber` (or log out and in).
table.insert(alsa_monitor.rules, {
  matches = {
    { { "device.name", "matches", "alsa_card.usb-Sony_Interactive_Entertainment_DualSense*" } },
  },
  apply_properties = {
    ["api.alsa.use-ucm"] = false,
    ["device.profile-set"] = "analog-surround-40.conf",
  },
})

table.insert(alsa_monitor.rules, {
  matches = {
    { { "node.name", "matches", "alsa_output.usb-Sony_Interactive_Entertainment_DualSense*.analog-surround-40" } },
  },
  apply_properties = {
    ["node.description"] = "Wireless Controller",
    ["node.nick"] = "Wireless Controller",
    ["audio.format"] = "S16LE",
    ["audio.rate"] = 48000,
    ["node.force-rate"] = 48000,
    ["channelmix.disable"] = true,
    ["priority.driver"] = 1500,
    ["priority.session"] = 1500,
  },
})
