// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// Shared DualSense protocol helpers (header only): CRC32 seeds, Bluetooth report builders, hidraw discovery and feature-report access.
// Used by ds5bridge (the daemon), ds5ctl and ds5fw.
#ifndef DS5PROTO_H
#define DS5PROTO_H
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// ---------------------------------------------------------------- CRC32 (the pad checks every Bluetooth report with a seeded CRC)
static uint32_t ds5_crc_table[256];
static inline void ds5_crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        ds5_crc_table[i] = c;
    }
}
static inline uint32_t ds5_crc32_run(uint32_t crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) crc = ds5_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
// crc32(seed || b[0..n)) stored little endian at b[n..n+4). Seeds: 0xA1 input, 0xA2 output, 0xA3 feature (hid-playstation), 0x53 for the
// "Bluetooth control" feature report (what dualsensectl uses for power-off).
static inline void ds5_crc_tail(uint8_t seed, uint8_t* b, size_t n) {
    uint32_t c = ~ds5_crc32_run(ds5_crc32_run(0xFFFFFFFFu, &seed, 1), b, n);
    for (int i = 0; i < 4; i++) b[n + i] = c >> (8 * i);
}

// ---------------------------------------------------------------- Bluetooth output reports
#define DS5_STATE_LEN 78       // 0x31: seq, tag 0x10, 47 B common block, padding, crc
#define DS5_MIC_STATUS_LEN 142 // 0x32
#define DS5_AUDIO_LEN 547      // 0x39

static inline void ds5_state_packet(int seq, uint8_t* o) {  // flag0 = 0x01 (haptics mode), everything else zero
    memset(o, 0, DS5_STATE_LEN);
    o[0] = 0x31; o[1] = (seq & 0xF) << 4; o[2] = 0x10; o[3] = 0x01;
    ds5_crc_tail(0xA2, o, 74);
}
static inline void ds5_mic_status_packet(int seq, int on, uint8_t* o) {  // switches the pad mic stream on (0b11) or off (0b10)
    memset(o, 0, DS5_MIC_STATUS_LEN);
    o[0] = 0x32; o[1] = (seq & 0xF) << 4; o[2] = 0x91; o[3] = 1; o[4] = on ? 3 : 2;
    ds5_crc_tail(0xA2, o, 138);
}
static inline void ds5_mic_volume_packet(int seq, int vol, uint8_t* o) {  // only mic gain (flag0 bit 6) + internal mic select (audio_control bit 0)
    memset(o, 0, DS5_STATE_LEN);
    o[0] = 0x31; o[1] = (seq & 0xF) << 4; o[2] = 0x10; o[3] = 0xC0; o[9] = vol; o[10] = 0x01;
    ds5_crc_tail(0xA2, o, 74);
}
// [0x39, seq<<4, 0x91 len 6 (flags, buffer length x4, counter), 0xD2 len 64 + 2 haptic blocks, optional 0xD3/0xD6 len 200 + 2 Opus frames], crc
static inline void ds5_audio_packet(int seq, int counter, const uint8_t* hap128, const uint8_t* spk400, int headphone, int buflen, int mic, uint8_t* p) {
    memset(p, 0, DS5_AUDIO_LEN);
    p[0] = 0x39; p[1] = (seq & 0xF) << 4; p[2] = 0x91; p[3] = 6;
    p[4] = mic ? 0x7F : 0x7E;
    for (int i = 5; i < 9; i++) p[i] = buflen;
    p[9] = counter & 0xFF; p[10] = 0xD2; p[11] = 64;
    memcpy(p + 12, hap128, 128);
    if (spk400) {
        p[140] = (headphone ? 0x16 : 0x13) | 0xC0; p[141] = 200;
        memcpy(p + 142, spk400, 400);
    }
    ds5_crc_tail(0xA2, p, 543);
}
// USB output report 0x02 + 47 B common block -> Bluetooth 0x31 (78 B with crc)
static inline void ds5_usb_to_bt_output(const uint8_t* r, size_t rn, int seq, uint8_t* o) {
    uint8_t u[48] = {0};
    memcpy(u, r, MIN(rn, 48));
    memset(o, 0, DS5_STATE_LEN);
    o[0] = 0x31; o[1] = (seq & 0xF) << 4; o[2] = 0x10;
    memcpy(o + 3, u + 1, 47);
    ds5_crc_tail(0xA2, o, 74);
}
// feature report 0x08 ("Bluetooth control"), byte 1 = 2: the pad powers itself off (Bluetooth only), 47 B with crc
static inline void ds5_power_off_report(uint8_t* o) {
    memset(o, 0, 47);
    o[0] = 0x08; o[1] = 2;
    ds5_crc_tail(0x53, o, 43);
}

static inline void ds5_hex(const uint8_t* p, size_t n, char* out) {  // out needs 2 * n + 1 bytes
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]);
    out[2 * n] = 0;
}

// ---------------------------------------------------------------- battery
// Status byte of the input report (USB report 0x01 byte 53, Bluetooth 0x31 byte 54): the low nibble is the charge in 10 % steps (0-10), the high nibble the charging
// state. Same table as hid-playstation (DualSenseClient reads "charging" as (level - 1) * 10, the kernel as level * 10 + 5; the kernel is the reference here).
typedef struct { int percent; const char* state; } Ds5Battery;
static inline Ds5Battery ds5_battery(uint8_t s) {
    int level = s & 0x0F;
    switch (s >> 4) {
        case 0x0: return (Ds5Battery){MIN(level * 10 + 5, 100), "discharging"};
        case 0x1: return (Ds5Battery){MIN(level * 10 + 5, 100), "charging"};
        case 0x2: return (Ds5Battery){100, "full"};
        case 0xA: case 0xB: return (Ds5Battery){0, "not charging (voltage or temperature out of range)"};
        default: return (Ds5Battery){0, "charging error"};   // 0xF and anything unknown
    }
}

// ---------------------------------------------------------------- hidraw
// Finds a DualSense (0CE6) or Edge (0DF2) hidraw node. bt = 1: Bluetooth (HID bus 0005); bt = 0: a real USB pad (bus 0003, never the
// virtual one served by vhci_hcd). node gets "/dev/hidrawN", uniq the HID_UNIQ (Bluetooth address of a BT pad). Returns 1 if found.
static inline int ds5_find_hidraw_nth(int bt, int nth, char* node, size_t nmax, char* uniq, size_t umax) {
    DIR* d = opendir("/sys/class/hidraw");
    if (!d) return 0;
    struct dirent* e;
    int found = 0;
    int seen = 0;
    while (!found && (e = readdir(d))) {
        if (strncmp(e->d_name, "hidraw", 6)) continue;
        char path[PATH_MAX], line[256], real[PATH_MAX], u[128] = "";
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        int match = 0;
        const char* want = bt ? "HID_ID=0005:0000054C:" : "HID_ID=0003:0000054C:";
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, want, strlen(want)) && (strstr(line, "00000CE6") || strstr(line, "00000DF2"))) match = 1;
            if (!strncmp(line, "HID_UNIQ=", 9)) { snprintf(u, sizeof u, "%s", line + 9); u[strcspn(u, "\n")] = 0; }
        }
        fclose(f);
        if (!match) continue;
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device", e->d_name);
        if (!bt && realpath(path, real) && strstr(real, "vhci_hcd")) continue;
        if (seen++ < nth) continue;
        snprintf(node, nmax, "/dev/%s", e->d_name);
        if (uniq) snprintf(uniq, umax, "%s", u);
        found = 1;
    }
    closedir(d);
    return found;
}
static inline int ds5_find_hidraw(int bt, char* node, size_t nmax, char* uniq, size_t umax) { return ds5_find_hidraw_nth(bt, 0, node, nmax, uniq, umax); }
// The USB product id (0x0CE6 DualSense, 0x0DF2 Edge) of a hidraw node such as "/dev/hidraw4", from its HID_ID; -1 if it cannot be read.
static inline int ds5_hidraw_pid(const char* node) {
    char path[PATH_MAX], line[256];
    snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", node + 5);   // skip "/dev/"
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int pid = -1;
    unsigned v;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "HID_ID=%*x:%*x:%x", &v) == 1) pid = v;
    fclose(f);
    return pid;
}
static inline int ds5_get_feature(int fd, int id, uint8_t* buf, int n) {  // buf[0] = id on return; returns bytes read or -1
    memset(buf, 0, n);
    buf[0] = id;
    return ioctl(fd, HIDIOCGFEATURE(n), buf);
}
static inline int ds5_set_feature(int fd, const uint8_t* buf, int n) { return ioctl(fd, HIDIOCSFEATURE(n), buf); }

#endif
