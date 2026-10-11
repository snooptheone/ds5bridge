// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// What the two daemon tests (test_lifecycle, test_multipad) share: they start bin/ds5bridge with fake pads (ptys), read its log from a pipe and talk to its debug
// TCP listener the way the kernel's vhci_hcd would (USB/IP import, an interrupt-IN read, a HID SET_REPORT). Every function here is used by both tests.
#ifndef PADTEST_H
#define PADTEST_H
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../src/descriptors.h"
#include "fakepad.h"
#include "testutil.h"

static int PORT;
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char logbuf[1 << 20];
static size_t logn, mark;
static int logfd;

#define now fp_now
static void pump(double secs) {   // read the daemon's stderr for `secs`
    double end = now() + secs;
    for (;;) {
        double left = end - now();
        if (left <= 0) return;
        fd_set fs;
        FD_ZERO(&fs); FD_SET(logfd, &fs);
        struct timeval tv = {(time_t)left, (long)((left - (time_t)left) * 1e6)};
        if (select(logfd + 1, &fs, NULL, NULL, &tv) > 0) {
            ssize_t r = read(logfd, logbuf + logn, sizeof logbuf - 1 - logn);
            if (r > 0) { logn += r; logbuf[logn] = 0; }
        }
    }
}
static int seen(const char* needle) { return strstr(logbuf + mark, needle) != NULL; }
static int wait_for(const char* needle, double secs) {
    double end = now() + secs;
    while (now() < end) { pump(0.1); if (seen(needle)) return 1; }
    return 0;
}
static void set_mark(void) { mark = logn; }

static int int_in_ep(void) {
    for (size_t i = 0; i < sizeof DS5_CONFIG_DESC; i += DS5_CONFIG_DESC[i] ? DS5_CONFIG_DESC[i] : 1)
        if (DS5_CONFIG_DESC[i + 1] == 5 && (DS5_CONFIG_DESC[i + 2] & 0x80) && (DS5_CONFIG_DESC[i + 3] & 3) == 3) return DS5_CONFIG_DESC[i + 2] & 15;
    return -1;
}
// a connection to the debug TCP port that has completed the USB/IP import of `busid` (the kernel's part), or -1
static int tcp_import(const char* busid) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return -1; }
    struct timeval tv = {3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t imp[40] = {0x01, 0x11, 0x80, 0x03}, ir[8 + 312];
    memcpy(imp + 8, busid, strlen(busid));
    send(s, imp, 40, 0);
    if (recv(s, ir, sizeof ir, MSG_WAITALL) != (ssize_t)sizeof ir) { close(s); return -1; }
    return s;
}
// the input report that virtual pad serves right now; returns 0 on success
static inline int virtual_pad_input(const char* busid, uint8_t out[64]) {
    int s = tcp_import(busid);
    if (s < 0) return -1;
    uint8_t h[48] = {0}, rh[48];
    h[3] = 1; h[7] = 1; h[11] = 2; h[15] = 1; h[19] = int_in_ep(); h[27] = 64;
    send(s, h, 48, 0);
    int ok = recv(s, rh, 48, MSG_WAITALL) == 48 && recv(s, out, 64, MSG_WAITALL) == 64;
    close(s);
    return ok ? 0 : -1;
}
// what the game does: a HID SET_REPORT with a 63-byte output report, sent through the kernel's connection
static int game_output(int s, const uint8_t rep[63]) {
    static uint8_t seq = 10;
    uint8_t h[48] = {0}, rh[48];
    h[3] = 1; h[7] = ++seq; h[11] = 2; h[15] = 0; h[19] = 0;
    h[27] = 63;
    h[40] = 0x21; h[41] = 9; h[42] = 2; h[43] = 2; h[44] = 3; h[46] = 63;
    send(s, h, 48, 0);
    send(s, rep, 63, 0);
    return recv(s, rh, 48, MSG_WAITALL) == 48 ? 0 : -1;
}
// the next 78-byte Bluetooth output report the daemon wrote to that fake pad, or -1
static inline int pad_frame(Pty* p, uint8_t f[78], double secs) {
    double end = now() + secs;
    size_t n = 0;
    while (now() < end && n < 78) {
        fd_set fs;
        FD_ZERO(&fs); FD_SET(p->master, &fs);
        struct timeval tv = {0, 100000};
        if (select(p->master + 1, &fs, NULL, NULL, &tv) > 0) { ssize_t r = read(p->master, f + n, 78 - n); if (r > 0) n += r; }
    }
    return n == 78 ? 0 : -1;
}

// ---- audio: what the game's audio engine does (an isochronous OUT URB of 4-channel 16 bit PCM) and what comes out of the fake pad (reports of different sizes)
static inline void pt32(uint8_t* b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static inline int iso_out_ep(void) {
    for (size_t i = 0; i < sizeof DS5_CONFIG_DESC; i += DS5_CONFIG_DESC[i] ? DS5_CONFIG_DESC[i] : 1)
        if (DS5_CONFIG_DESC[i + 1] == 5 && !(DS5_CONFIG_DESC[i + 2] & 0x80) && (DS5_CONFIG_DESC[i + 3] & 3) == 1) return DS5_CONFIG_DESC[i + 2] & 15;
    return -1;
}
// one URB of `frames` frames (a multiple of 8) of a square tone on all four channels, sent without waiting for its (paced) answer
static inline void game_audio(int s, int frames, int amp) {
    static uint32_t seq = 100;
    uint8_t h[48] = {0};
    pt32(h, 1); pt32(h + 4, ++seq); pt32(h + 8, 0x10002); pt32(h + 12, 0); pt32(h + 16, iso_out_ep());
    pt32(h + 24, frames * 8); pt32(h + 32, 8); pt32(h + 36, 8);
    uint8_t* pcm = malloc(frames * 8);
    for (int i = 0; i < frames; i++) for (int c = 0; c < 4; c++) { int16_t v = i % 2 ? amp : -amp; pcm[i * 8 + 2 * c] = v & 255; pcm[i * 8 + 2 * c + 1] = v >> 8; }
    uint8_t d[16 * 8] = {0};
    for (int k = 0; k < 8; k++) { pt32(d + 16 * k, k * (frames / 8) * 8); pt32(d + 16 * k + 4, (frames / 8) * 8); }
    send(s, h, 48, 0); send(s, pcm, frames * 8, 0); send(s, d, sizeof d, 0);
    free(pcm);
}
// the next whole report the daemon wrote to the fake pad (0x31 = 78 bytes, 0x32 = 142, 0x39 = 547; the pty is a byte stream, so writes can arrive glued together), or 0
static inline size_t pad_message(Pty* p, uint8_t* out, double secs) {
    static uint8_t sb[8192];
    static size_t sn;
    double end = now() + secs;
    for (;;) {
        size_t want = sn ? (sb[0] == 0x39 ? 547 : sb[0] == 0x32 ? 142 : 78) : 0;
        if (want && sn >= want) { memcpy(out, sb, want); memmove(sb, sb + want, sn - want); sn -= want; return want; }
        if (now() >= end) return 0;
        fd_set fs;
        FD_ZERO(&fs); FD_SET(p->master, &fs);
        struct timeval tv = {0, 50000};
        if (select(p->master + 1, &fs, NULL, NULL, &tv) > 0) { ssize_t r = read(p->master, sb + sn, sizeof sb - sn); if (r > 0) sn += r; }
    }
}
#endif
