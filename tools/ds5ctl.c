// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// ds5ctl: small command line tool for a DualSense over USB or Bluetooth, straight through hidraw.
//   ds5ctl trigger right feedback 3 6        position 0-9, strength 1-8: resistance from zone 3 onward
//   ds5ctl trigger left  weapon   2 5 6      start 2-7, end start+1..8, strength 1-8: click at the end
//   ds5ctl trigger right vibration 0 4 40    position 0-9, amplitude 1-8, frequency 1-255 Hz
//   ds5ctl trigger both  off
//   ds5ctl power-off                         power the Bluetooth pad off
//   ds5ctl rate [SECONDS]                    input reports per second of the Bluetooth pad (needs ds5bridge's QoS to be at its best)
//   ds5ctl stress [MAX_HZ]                   find how many output reports per second the Bluetooth pad takes (see cmd_stress)
// Options: --dry-run (print the report, send nothing), --bus usb|bt (pick/force the bus; needed with --dry-run), -h.
// The Bluetooth pad's hidraw node is root-only when ds5bridge's udev rules are installed: use sudo for the Bluetooth pad.
//
// Trigger report layout, from hid-playstation's dualsense_output_worker (the "common" block of the output report):
//   [0] valid_flag0 (bit 2 = right trigger effect, bit 3 = left)   [10..20] right effect (11 B)   [21..31] left effect
//   USB: 0x02 + common padded to 63 B.  BT: 0x31, seq<<4, 0x10, common, padded to 74 B + crc32 = 78 B.
// The effect mode bytes (0x21 feedback, 0x25 weapon, 0x26 vibration, 0x05 off) and the zone encoding come from community documentation.
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>

#include "ds5proto.h"

static void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static uint32_t zone_mask(int pos) { uint32_t m = 0; for (int i = pos; i < 10; i++) m |= 1u << i; return m; }
static uint32_t zone_strengths(int pos, int s) { uint32_t m = 0; for (int i = pos; i < 10; i++) m |= (uint32_t)(s - 1) << (3 * i); return m; }

static int make_effect(const char* mode, const int* a, int na, uint8_t e[11]) {
    memset(e, 0, 11);
    if (!strcmp(mode, "off")) { if (na != 0) return -1; e[0] = 0x05; return 0; }
    if (!strcmp(mode, "feedback")) {
        if (na != 2 || a[0] < 0 || a[0] > 9 || a[1] < 1 || a[1] > 8) return -1;
        e[0] = 0x21;
        uint32_t z = zone_mask(a[0]), st = zone_strengths(a[0], a[1]);
        e[1] = z; e[2] = z >> 8;
        for (int i = 0; i < 4; i++) e[3 + i] = st >> (8 * i);
        return 0;
    }
    if (!strcmp(mode, "weapon")) {
        if (na != 3 || a[0] < 2 || a[0] > 7 || a[1] <= a[0] || a[1] > 8 || a[2] < 1 || a[2] > 8) return -1;
        e[0] = 0x25;
        uint32_t z = (1u << a[0]) | (1u << a[1]);
        e[1] = z; e[2] = z >> 8;
        e[3] = a[2] - 1;
        return 0;
    }
    if (!strcmp(mode, "vibration")) {
        if (na != 3 || a[0] < 0 || a[0] > 9 || a[1] < 1 || a[1] > 8 || a[2] < 1 || a[2] > 255) return -1;
        e[0] = 0x26;
        uint32_t z = zone_mask(a[0]), st = zone_strengths(a[0], a[1]);
        e[1] = z; e[2] = z >> 8;
        for (int i = 0; i < 4; i++) e[3 + i] = st >> (8 * i);
        e[9] = a[2];
        return 0;
    }
    return -1;
}

// the report for `side`, USB (63 B) or BT (78 B); returns its length
static int build_report(int bt, const char* side, const uint8_t eff[11], int seq, uint8_t* out) {
    uint8_t c[47] = {0};
    if (!strcmp(side, "right") || !strcmp(side, "both")) { c[0] |= 0x04; memcpy(c + 10, eff, 11); }
    if (!strcmp(side, "left") || !strcmp(side, "both")) { c[0] |= 0x08; memcpy(c + 21, eff, 11); }
    uint8_t usb[63] = {0x02};
    memcpy(usb + 1, c, 47);
    if (!bt) { memcpy(out, usb, 63); return 63; }
    ds5_usb_to_bt_output(usb, 63, seq, out);
    return DS5_STATE_LEN;
}

static const char* g_hidraw;   // --hidraw PATH: use this node as the Bluetooth pad (tests)
static int open_pad(int bus /* -1 auto, 0 usb, 1 bt */, int* is_bt, char* node, size_t nmax) {
    int found = 0;
    if (g_hidraw) { snprintf(node, nmax, "%s", g_hidraw); *is_bt = 1; found = 1; }
    else if (bus != 0 && ds5_find_hidraw(1, node, nmax, NULL, 0)) { *is_bt = 1; found = 1; }
    else if (bus != 1 && ds5_find_hidraw(0, node, nmax, NULL, 0)) { *is_bt = 0; found = 1; }
    if (!found) die("no DualSense hidraw found (is it connected%s?)", bus == 1 ? " over Bluetooth" : bus == 0 ? " by USB cable" : "");
    int fd = open(node, O_RDWR);
    if (fd < 0) die("cannot open %s: %s%s", node, strerror(errno), *is_bt ? " (the Bluetooth pad is root-only when ds5bridge's udev rules are installed: use sudo)" : "");
    return fd;
}

// Battery level and charging state, from one input report (the same table as hid-playstation).
static int cmd_battery(int bus) {
    char node[300];
    int bt;
    int fd = open_pad(bus, &bt, node, sizeof node);
    if (bt) { uint8_t f[41]; ds5_get_feature(fd, 0x05, f, sizeof f); }   // the Bluetooth pad sends its full 0x31 reports only after this read (what hid-playstation does); a failed read changes nothing
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9 > 3) break;
        uint8_t r[128];
        struct timeval tv = {0, 200000};
        fd_set fs;
        FD_ZERO(&fs); FD_SET(fd, &fs);
        if (select(fd + 1, &fs, NULL, NULL, &tv) <= 0) continue;
        ssize_t n = read(fd, r, sizeof r);
        int s = -1;
        if (bt && n >= 65 && r[0] == 0x31 && !(r[1] & 2)) s = r[54];          // not a microphone frame
        else if (!bt && n >= 54 && r[0] == 0x01) s = r[53];
        if (s < 0) continue;
        Ds5Battery b = ds5_battery((uint8_t)s);
        printf("%s: battery %d %% (%s)\n", node, b.percent, b.state);
        return 0;
    }
    die("no input report with a battery level in 3 s");
    return 1;
}

static int cmd_rate(int bus, double secs) {
    char node[300];
    int bt;
    int fd = open_pad(bus == 0 ? 1 : bus, &bt, node, sizeof node);
    if (!bt) die("rate measures the Bluetooth pad");
    static double ts[20000];
    int n = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
        if (el >= secs || n >= 20000) break;
        uint8_t r[128];
        struct timeval tv = {0, 200000};
        fd_set fs;
        FD_ZERO(&fs); FD_SET(fd, &fs);
        if (select(fd + 1, &fs, NULL, NULL, &tv) > 0 && read(fd, r, sizeof r) > 0 && r[0] == 0x31) ts[n++] = el;
    }
    if (n < 3) die("only %d reports in %.0f s", n, secs);
    double gaps[20000], mx = 0, sum = 0;
    for (int i = 1; i < n; i++) { gaps[i - 1] = (ts[i] - ts[i - 1]) * 1000; sum += gaps[i - 1]; if (gaps[i - 1] > mx) mx = gaps[i - 1]; }
    int g = n - 1;
    for (int i = 0; i < g; i++) for (int j = i + 1; j < g; j++) if (gaps[j] < gaps[i]) { double x = gaps[i]; gaps[i] = gaps[j]; gaps[j] = x; }
    printf("%.1f reports/s\ninterval ms: median %.1f  mean %.1f  p90 %.1f  max %.1f\n", n / secs, gaps[g / 2], sum / g, gaps[(int)(g * 0.9)], mx);
    return 0;
}

// Output-report rate limit of the Bluetooth pad. Writes harmless state reports (flag0 = 0x01, motors at zero: nothing changes on the pad) at 20, 40, 60, 80,
// 100, 125, 150, 200, 250, 333 reports/s, 3 s each with 1 s of rest, while counting the input reports that keep coming back. Stops at the first rate where a write
// fails (the pad closed its HID channel: EIO/ENODEV) or the input reports stall for more than 1 s. The pad may DROP the Bluetooth link at that point: press PS to
// reconnect it. Stop ds5bridge first (sudo systemctl stop ds5bridge): it writes to the pad too.
static void cmd_stress(int bus, int max_hz) {
    char node[300];
    int bt;
    int fd = open_pad(bus == 0 ? 1 : bus, &bt, node, sizeof node);
    if (!bt) die("stress measures the Bluetooth pad");
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);   // the drain loop below must not wait for the next input report
    static const int rates[] = {20, 40, 60, 80, 100, 125, 150, 200, 250, 333};
    printf("%s: stop ds5bridge first; the pad may drop at the limit (press PS to bring it back)\n", node);
    printf("%6s %8s %8s %7s %12s %14s\n", "Hz", "writes", "EAGAIN", "errors", "input/s", "max gap (ms)");
    signal(SIGALRM, SIG_DFL);
    int last_good = 0, seq = 0;
    for (size_t k = 0; k < sizeof rates / sizeof *rates && rates[k] <= max_hz; k++) {
        int hz = rates[k], ok = 0, busy = 0, err = 0, inputs = 0, stalled = 0, dots = 0;
        alarm(20);   // one rate takes 3 s: if it takes 20, something blocks (a write on the pad?) and the tool says so instead of hanging
        printf("%6d ", hz); fflush(stdout);
        double gap_max = 0;
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        double last_in = 0, el = 0;
        for (long n = 0; ; n++) {
            double due = (double)n / hz;
            if (due >= 3.0) break;
            if ((int)due > dots) { dots = (int)due; printf("."); fflush(stdout); }   // one dot per second: shows where it stops
            struct timespec t = {t0.tv_sec + (time_t)due, t0.tv_nsec + (long)((due - (time_t)due) * 1e9)};
            if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
            uint8_t rep[DS5_STATE_LEN], r[128];
            ds5_state_packet(++seq, rep);
            ssize_t w = write(fd, rep, sizeof rep);
            if (w == (ssize_t)sizeof rep) ok++; else if (w < 0 && errno == EAGAIN) busy++; else err++;
            ssize_t rd;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) * 1e-9;
            while ((rd = read(fd, r, sizeof r)) > 0)
                if (r[0] == 0x31) { inputs++; if (last_in > 0 && (el - last_in) * 1000 > gap_max) gap_max = (el - last_in) * 1000; last_in = el; }
            if (err || (last_in > 0 && el - last_in > 1.0) || (last_in == 0 && el > 1.5)) { stalled = !err; break; }
        }
        alarm(0);
        printf(" %8d %8d %7d %12.0f %14.0f%s\n", ok, busy, err, inputs / 3.0, gap_max, err ? "   <- write failed: the pad dropped its HID channel" : stalled ? "   <- input stalled" : "");
        fflush(stdout);
        if (err || stalled) {
            printf("\nthe limit is below %d writes/s%s\n", hz, last_good ? "" : " (even the first rate failed)");
            if (last_good) printf("highest rate without any problem: %d writes/s\n", last_good);
            exit(3);
        }
        last_good = hz;
        sleep(1);
    }
    printf("\nno problem up to %d writes/s (the highest rate tried)\n", last_good);
}

#ifndef DS5_VERSION
#define DS5_VERSION "dev"
#endif
static void usage(void) {
    puts("usage: ds5ctl [--dry-run] [--bus usb|bt] trigger left|right|both off|feedback P S|weapon S E S|vibration P A F\n"
         "       ds5ctl [--bus bt] power-off\n"
         "       ds5ctl battery             (level and charging state)\n"
         "       ds5ctl rate [SECONDS]\n"
         "       ds5ctl stress [MAX_HZ]     (Bluetooth pad: how many output reports/s it takes; stop ds5bridge first)");
}

int main(int argc, char** argv) {
    ds5_crc_init();
    int dry = 0, bus = -1, na = 0;
    const char* words[16];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run")) dry = 1;
        else if (!strcmp(argv[i], "--hidraw") && i + 1 < argc) g_hidraw = argv[++i];
        else if (!strcmp(argv[i], "--bus") && i + 1 < argc) { i++; bus = !strcmp(argv[i], "bt") ? 1 : !strcmp(argv[i], "usb") ? 0 : -2; if (bus == -2) die("--bus is usb or bt"); }
        else if (!strcmp(argv[i], "--version")) { puts("ds5ctl " DS5_VERSION); return 0; }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (na < 16) words[na++] = argv[i];
    }
    if (na < 1) { usage(); return 2; }
    if (!strcmp(words[0], "battery")) return cmd_battery(bus);
    if (!strcmp(words[0], "rate")) return cmd_rate(bus, na > 1 ? atof(words[1]) : 3);
    if (!strcmp(words[0], "stress")) { cmd_stress(bus, na > 1 ? atoi(words[1]) : 333); return 0; }
    if (!strcmp(words[0], "power-off")) {
        char node[300];
        int bt;
        uint8_t rep[47];
        ds5_power_off_report(rep);
        if (dry) { char h[100]; ds5_hex(rep, 47, h); printf("- (BT) 47 B: %s\n", h); return 0; }
        int fd = open_pad(1, &bt, node, sizeof node);
        if (ds5_set_feature(fd, rep, sizeof rep) < 0) die("power-off failed: %s", strerror(errno));
        printf("%s: power-off sent\n", node);
        return 0;
    }
    if (strcmp(words[0], "trigger") || na < 3) { usage(); return 2; }
    const char* side = words[1];
    if (strcmp(side, "left") && strcmp(side, "right") && strcmp(side, "both")) die("side is left, right or both");
    int a[8], k = 0;
    for (int i = 3; i < na && k < 8; i++) a[k++] = atoi(words[i]);
    uint8_t eff[11];
    if (make_effect(words[2], a, k, eff) < 0) die("%s: wrong number of numbers or a parameter out of range (see the header of tools/ds5ctl.c)", words[2]);
    char node[300] = "";
    int bt, fd = -1;
    if (dry && bus >= 0) bt = bus;
    else { fd = open_pad(bus, &bt, node, sizeof node); }
    uint8_t rep[DS5_STATE_LEN];
    int n = build_report(bt, side, eff, 1, rep);
    char hex[2 * DS5_STATE_LEN + 1];
    ds5_hex(rep, n, hex);
    printf("%s (%s) %d B: %s\n", node[0] ? node : "-", bt ? "BT" : "USB", n, hex);
    if (!dry && write(fd, rep, n) != n) die("write failed: %s", strerror(errno));
    return 0;
}
