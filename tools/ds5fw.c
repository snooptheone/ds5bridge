// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// ds5fw: DualSense firmware flasher over USB, a C port of the protocol of Sony's FWupdater (libFWupdater.dll). Straight hidraw, no hidapi.
//
//   ds5fw                         list the USB controllers
//   ds5fw FILE.bin                check FILE.bin (size, version, product id) and list the controllers; does not flash
//   ds5fw --dry-run FILE.bin [OUT]  no device: write every report as a hex line to OUT (default dryrun.txt)
//   ds5fw --flash FILE.bin        flash (asks you to type FLASH)
//
// Protocol, all feature reports are 64 bytes:  set 0xF4: [F4, stage, len, data[57], pad[4]]   get 0xF5: [F5, stage, status, ...]
//   stage 0 header (first 256 B, 5 reports, then poll once)       stage 1 image (whole file, 0x8000-byte blocks of 57-byte reports, poll after each)
//   stage 2 verify (poll while status 0x10)                       stage 3 finalize (the controller reboots)
// The image's product id (offset 0x62, as dualsensectl checks it) must be the connected controller's: a DualSense Edge image is refused for a DualSense
// before anything is sent.
// Verified byte for byte against the reports of the official updater with a dry run; flashing itself has not been exercised on hardware here.
// Unverified: pad bytes are assumed zero; USB only (Bluetooth uses other report framing).
#include <errno.h>
#include <time.h>

#include "ds5proto.h"

#define IMAGE_SIZE 950272
#define HEADER_SIZE 0x100
#define BLOCK 0x8000
#define CHUNK 57

typedef struct { int fd; FILE* log; int stage; } Hid;  // fd < 0: dry run, the sets are logged and every status answers OK

static void msleep(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }

static void pkt(int stage, const uint8_t* data, size_t n, uint8_t out[64]) {
    memset(out, 0, 64);
    out[0] = 0xF4; out[1] = stage; out[2] = n;
    if (n) memcpy(out + 3, data, n);
}
static int hid_send(Hid* h, const uint8_t* b) {
    if (h->fd < 0) {
        h->stage = b[1];
        char hex[129];
        ds5_hex(b, 64, hex);
        fprintf(h->log, "%s\n", hex);
        return 0;
    }
    return ds5_set_feature(h->fd, b, 64) == 64 ? 0 : -1;
}
static int hid_status(Hid* h, uint8_t* r) {  // r = F5 report
    if (h->fd < 0) { memset(r, 0, 64); r[0] = 0xF5; r[1] = h->stage; return 0; }
    return ds5_get_feature(h->fd, 0xF5, r, 64) >= 3 ? 0 : -1;
}
static const char* status_name(int s) {
    switch (s) {
        case 1: return "CMAC check"; case 2: return "version check"; case 3: return "capability info"; case 5: return "flash erase";
        case 6: return "header info not received"; case 0x11: return "bad parameter"; case 0xFF: return "other"; default: return "unknown";
    }
}
// wait for F5 status 0; fails on an error status. Mirrors the DLL's status switch.
static int poll_status(Hid* h, int stage, char* err, size_t emax) {
    for (int i = 0; i < 600; i++) {
        uint8_t r[64];
        if (hid_status(h, r) < 0 || r[1] != stage) { snprintf(err, emax, "stage %d: bad F5 reply %02x %02x %02x", stage, r[0], r[1], r[2]); return -1; }
        if (r[2] == 0) return 0;
        if (r[2] == 4) { if (h->fd >= 0) msleep(10); }
        else if (r[2] == 0x10) { if (h->fd >= 0) msleep(50); }
        else { snprintf(err, emax, "stage %d: %s (status %#x)", stage, status_name(r[2]), r[2]); return -1; }
    }
    snprintf(err, emax, "stage %d: timeout", stage);
    return -1;
}
static int send_chunks(Hid* h, int stage, const uint8_t* data, size_t len, int poll_each, char* err, size_t emax) {
    for (size_t off = 0, i = 0; off < len; off += CHUNK, i++) {
        uint8_t p[64];
        pkt(stage, data + off, MIN((size_t)CHUNK, len - off), p);
        if (hid_send(h, p) < 0) { snprintf(err, emax, "stage %d: set feature failed: %s", stage, strerror(errno)); return -1; }
        if (poll_each) { if (poll_status(h, stage, err, emax) < 0) return -1; }
        else if (i == 0 && h->fd >= 0) msleep(50);
    }
    return poll_each ? 0 : poll_status(h, stage, err, emax);
}
static int flash(Hid* h, const uint8_t* img, char* err, size_t emax) {
    printf("stage 0: header\n");
    if (send_chunks(h, 0, img, HEADER_SIZE, 0, err, emax) < 0) return -1;
    for (size_t off = 0, n = 1; off < IMAGE_SIZE; off += BLOCK, n++) {
        printf("stage 1: block %zu/%d\r", n, IMAGE_SIZE / BLOCK);
        fflush(stdout);
        if (send_chunks(h, 1, img + off, MIN((size_t)BLOCK, (size_t)IMAGE_SIZE - off), 1, err, emax) < 0) return -1;
        if (h->fd >= 0) msleep(10);
    }
    uint8_t p[64];
    printf("\nstage 2: verify\n");
    pkt(2, NULL, 0, p);
    if (hid_send(h, p) < 0 || poll_status(h, 2, err, emax) < 0) return -1;
    printf("stage 3: finalize (controller reboots)\n");
    pkt(3, NULL, 0, p);
    return hid_send(h, p);
}
static uint8_t* load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n != IMAGE_SIZE) { fprintf(stderr, "%s: %ld bytes, expected %d\n", path, n, IMAGE_SIZE); exit(1); }
    uint8_t* b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { fprintf(stderr, "%s: read failed\n", path); exit(1); }
    fclose(f);
    printf("image version: %#06x, product id: %#06x\n", b[121] << 8 | b[120], b[0x63] << 8 | b[0x62]);
    return b;
}
// 0 when the image is for the controller with product id pad_pid; otherwise the reason is printed and -1 returned
static int check_pid(const uint8_t* img, int pad_pid) {
    int image_pid = img[0x63] << 8 | img[0x62];
    if (pad_pid < 0) { fprintf(stderr, "cannot read the controller's product id: refusing\n"); return -1; }
    if (image_pid != pad_pid) { fprintf(stderr, "the image is for product id %#06x but the controller is %#06x: refusing\n", image_pid, pad_pid); return -1; }
    return 0;
}
static int list_pads(char nodes[][300]) {
    int n = 0;
    for (; n < 8 && ds5_find_hidraw_nth(0, n, nodes[n], 300, NULL, 0); n++) printf("DualSense  %s\n", nodes[n]);
    if (!n) printf("no DualSense found (USB cable? hidraw permissions?)\n");
    return n;
}

int main(int argc, char** argv) {
    const char *flash_path = NULL, *dry_path = NULL, *check_path = NULL, *dry_out = "dryrun.txt";
    int pad_pid_override = -1;   // tests: --pad-pid 0x0CE6 makes --dry-run run the product id check against that id
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--flash") && i + 1 < argc) flash_path = argv[++i];
        else if (!strcmp(argv[i], "--dry-run") && i + 1 < argc) { dry_path = argv[++i]; if (i + 1 < argc && argv[i + 1][0] != '-') dry_out = argv[++i]; }
        else if (!strcmp(argv[i], "--pad-pid") && i + 1 < argc) pad_pid_override = (int)strtol(argv[++i], NULL, 0);
        else if (argv[i][0] != '-') check_path = argv[i];
        else { fprintf(stderr, "usage: ds5fw [FILE.bin | --dry-run FILE.bin [OUT] | --flash FILE.bin]\n"); return 2; }
    }
    char err[200] = "";
    if (dry_path) {
        uint8_t* img = load(dry_path);
        if (pad_pid_override >= 0 && check_pid(img, pad_pid_override) < 0) return 1;
        FILE* log = fopen(dry_out, "w");
        if (!log) { fprintf(stderr, "%s: %s\n", dry_out, strerror(errno)); return 1; }
        Hid h = {-1, log, 0};
        int rc = flash(&h, img, err, sizeof err);
        fclose(log);
        if (rc < 0) { fprintf(stderr, "%s\n", err); return 1; }
        printf("dry run done -> %s\n", dry_out);
        return 0;
    }
    char nodes[8][300];
    int n = list_pads(nodes);
    uint8_t* img = NULL;
    if (flash_path) img = load(flash_path);
    else if (check_path) {
        uint8_t* chk = load(check_path);
        if (n >= 1) printf("the image %s the controller on %s\n", check_pid(chk, ds5_hidraw_pid(nodes[0])) == 0 ? "matches" : "does NOT match", nodes[0]);
        return 0;
    }
    if (!flash_path) return 0;
    if (n != 1) { fprintf(stderr, "need exactly one controller connected by USB\n"); return 1; }
    if (check_pid(img, ds5_hidraw_pid(nodes[0])) < 0) return 1;
    printf("Flashing can brick the controller. Type FLASH to continue: ");
    fflush(stdout);
    char line[64] = "";
    if (!fgets(line, sizeof line, stdin) || strcmp(line, "FLASH\n")) { fprintf(stderr, "aborted\n"); return 1; }
    int fd = open(nodes[0], O_RDWR);
    if (fd < 0) { fprintf(stderr, "cannot open %s: %s\n", nodes[0], strerror(errno)); return 1; }
    Hid h = {fd, NULL, 0};
    if (flash(&h, img, err, sizeof err) < 0) {
        // same recovery as the official app: finalize/reboot the controller
        fprintf(stderr, "\nFAILED: %s\nrebooting controller\n", err);
        uint8_t p[64];
        pkt(3, NULL, 0, p);
        hid_send(&h, p);
        return 1;
    }
    printf("done; check the version after the controller reconnects\n");
    return 0;
}
