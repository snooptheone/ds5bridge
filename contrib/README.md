# contrib

`hid-playstation-mic.patch` (against Linux 7.2.9, `drivers/hid/hid-playstation.c`, GPL-2.0 like the file it patches): skips Bluetooth input report
`0x31` when its byte 1 bit 1 is set. With the pad microphone streaming, that report carries Opus audio instead of controller state, and the stock
driver parses it as state: ghost touchpad fingers, stick values and, through the mute button bit, a flickering mute LED that locks the pad up.

**You do not need it by default.** ds5bridge leaves the pad microphone off (`--mic` turns it on), so the pad never sends those reports and the stock
driver is fine. Apply it only if you use `--mic` and see the cursor moving by itself.

Build it out of tree: copy the kernel's `drivers/hid/hid-playstation.c` and `hid-ids.h` next to a one-line Makefile (`obj-m := hid-playstation.o`),
apply the patch and build with the compiler your kernel was built with (`make LLVM=1` on kernels built with clang/LTO, e.g. CachyOS).
