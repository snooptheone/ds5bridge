// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// Protocol test of ds5bridge: starts bin/ds5bridge --no-attach --no-bt with a debug TCP port and plays the kernel's role (vhci_hcd) with the USB/IP wire protocol.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/descriptors.h"
#include "testutil.h"

static int PORT;   // chosen at start: a free port
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int connect_retry(void) {
    for (int i = 0; i < 50; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT)};
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(s, (struct sockaddr*)&a, sizeof a) == 0) { struct timeval tv = {3, 0}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); return s; }
        close(s);
        usleep(100000);
    }
    fprintf(stderr, "cannot connect to ds5bridge\n");
    exit(1);
}
static void rx(int s, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(s, (char*)buf + got, n - got, 0);
        if (r <= 0) { fprintf(stderr, "connection closed or timeout (%zu/%zu)\n", got, n); exit(1); }
        got += r;
    }
}
static void p32(uint8_t* b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static uint32_t g32(const uint8_t* b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

static uint32_t g_seq;
// one URB: returns status; the IN payload goes to `in` (cap inmax), its length to *inlen
static int urb(int s, int dir, int ep, const uint8_t* setup, const uint8_t* data, int dlen, int tlen, int npk, int interval, const uint32_t* descs, uint8_t* in, size_t* inlen) {
    uint8_t h[48] = {0}, d[16 * 16];
    uint32_t seq = ++g_seq;
    p32(h, 1); p32(h + 4, seq); p32(h + 8, 0x10002); p32(h + 12, dir); p32(h + 16, ep);
    p32(h + 24, tlen); p32(h + 32, npk); p32(h + 36, interval);
    if (setup) memcpy(h + 40, setup, 8);
    send(s, h, 48, 0);
    if (dir == 0 && dlen) send(s, data, dlen, 0);
    for (int k = 0; k < npk; k++) { p32(d + 16 * k, descs[2 * k]); p32(d + 16 * k + 4, descs[2 * k + 1]); p32(d + 16 * k + 8, 0); p32(d + 16 * k + 12, 0); }
    if (npk > 0) send(s, d, 16 * npk, 0);
    uint8_t r[48];
    rx(s, r, 48);
    CHECK(g32(r) == 3 && g32(r + 4) == seq);
    int status = (int32_t)g32(r + 20), actual = (int32_t)g32(r + 24), n = (int32_t)g32(r + 32);
    if (inlen) *inlen = 0;
    if (dir == 1 && actual > 0) { uint8_t tmp[8192]; rx(s, tmp, actual); if (in) memcpy(in, tmp, actual); if (inlen) *inlen = actual; }
    if (n > 0) { uint8_t tmp[16 * 16]; rx(s, tmp, 16 * n); }
    return status;
}
static uint8_t* setup(uint8_t* b, int bm, int br, int wv, int wi, int wl) { b[0] = bm; b[1] = br; b[2] = wv; b[3] = wv >> 8; b[4] = wi; b[5] = wi >> 8; b[6] = wl; b[7] = wl >> 8; return b; }

// a connection that has completed the USB/IP import handshake
static int import_conn(void) {
    int s = connect_retry();
    uint8_t imp[40] = {0x01, 0x11, 0x80, 0x03}, ir[8 + 312];
    memcpy(imp + 8, "1-1", 3);
    send(s, imp, 40, 0);
    rx(s, ir, sizeof ir);
    return s;
}
// 1 when the server closes the connection (EOF or reset) within the receive timeout, ignoring whatever it still sends first
static int closed_by_server(int s) {
    uint8_t tmp[4096];
    for (;;) {
        ssize_t r = recv(s, tmp, sizeof tmp, 0);
        if (r == 0) return 1;
        if (r < 0) return errno == ECONNRESET;
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    PORT = free_port();
    char port[16];
    snprintf(port, sizeof port, "%d", PORT);
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);   // never outlive the test, even if it exits early: a stale daemon on this port would answer the next run
        if (!freopen("/dev/null", "w", stderr)) _exit(127);
        execl("bin/ds5bridge", "ds5bridge", "--no-attach", "--no-bt", "--port", port, (char*)NULL);
        _exit(127);
    }
    // endpoints from the embedded config descriptor
    int int_in = -1, iso_in = -1, iso_out = -1;
    for (size_t i = 0; i < sizeof DS5_CONFIG_DESC; i += DS5_CONFIG_DESC[i] ? DS5_CONFIG_DESC[i] : 1)
        if (DS5_CONFIG_DESC[i + 1] == 5) {
            int a = DS5_CONFIG_DESC[i + 2], type = DS5_CONFIG_DESC[i + 3] & 3;
            if ((a & 0x80) && type == 3) int_in = a & 15;
            if ((a & 0x80) && type == 1) iso_in = a & 15;
            if (!(a & 0x80) && type == 1) iso_out = a & 15;
        }
    CHECK(int_in > 0 && iso_in > 0 && iso_out > 0);
    uint8_t st[8], buf[8192];
    size_t n;
    // devlist on its own connection
    int s = connect_retry();
    uint8_t req[8] = {0x01, 0x11, 0x80, 0x05, 0, 0, 0, 0}, rep[12];
    send(s, req, 8, 0);
    rx(s, rep, 12);
    CHECK(rep[2] == 0x00 && rep[3] == 0x05 && g32(rep + 8) == 1);
    uint8_t dl[312];
    rx(s, dl, 312);
    for (int i = 0; i < dl[311]; i++) rx(s, buf, 4);
    close(s);
    // import
    s = connect_retry();
    uint8_t imp[40] = {0x01, 0x11, 0x80, 0x03};
    memcpy(imp + 8, "1-1", 3);
    send(s, imp, 40, 0);
    uint8_t ir[8 + 312];
    rx(s, ir, sizeof ir);
    CHECK(ir[2] == 0 && ir[3] == 3 && g32(ir + 4) == 0 && ir[8 + 256 + 32 + 12 + 1] == 0x4C && ir[8 + 256 + 32 + 12] == 0x05);  // idVendor 0x054C
    CHECK(urb(s, 1, 0, setup(st, 0x80, 6, 0x0100, 0, 18), NULL, 0, 18, -1, 0, NULL, buf, &n) == 0 && n == 18 && !memcmp(buf, DS5_DEVICE_DESC, 18));
    CHECK(urb(s, 1, 0, setup(st, 0x80, 6, 0x0200, 0, 4096), NULL, 0, 4096, -1, 0, NULL, buf, &n) == 0 && n == sizeof DS5_CONFIG_DESC && !memcmp(buf, DS5_CONFIG_DESC, n));
    CHECK(urb(s, 1, 0, setup(st, 0x81, 6, 0x2200, 3, 4096), NULL, 0, 4096, -1, 0, NULL, buf, &n) == 0 && n == sizeof DS5_HID_REPORT_DESC && !memcmp(buf, DS5_HID_REPORT_DESC, n));
    CHECK(urb(s, 1, 0, setup(st, 0x80, 6, 0x0302, 0, 255), NULL, 0, 255, -1, 0, NULL, buf, &n) == 0 && n == 2 + 2 * strlen(DS5_STRING_PRODUCT) && buf[2] == 'D');
    CHECK(urb(s, 1, 0, setup(st, 0xA1, 1, 0x0309, 3, 20), NULL, 0, 20, -1, 0, NULL, buf, &n) == 0 && n == 20 && buf[0] == 9);   // pairing info report (placeholder without a pad)
    CHECK(urb(s, 1, 0, setup(st, 0xC0, 0x77, 0, 0, 4), NULL, 0, 4, -1, 0, NULL, buf, &n) == -32);                                // unknown request -> stall
    uint8_t v[2] = {0x34, 0x12};
    CHECK(urb(s, 0, 0, setup(st, 0x21, 1, 0x0200, 0x0201, 2), v, 2, 2, -1, 0, NULL, NULL, NULL) == 0);
    CHECK(urb(s, 1, 0, setup(st, 0xA1, 0x81, 0x0200, 0x0201, 2), NULL, 0, 2, -1, 0, NULL, buf, &n) == 0 && n == 2 && buf[0] == 0x34 && buf[1] == 0x12);
    uint8_t out[63] = {2};
    CHECK(urb(s, 0, 0, setup(st, 0x21, 9, 0x0202, 3, 63), out, 63, 63, -1, 0, NULL, NULL, NULL) == 0);                               // HID SET_REPORT output
    uint8_t fw[64] = {0xF4, 1, 2};
    CHECK(urb(s, 0, 0, setup(st, 0x21, 9, 0x03F4, 3, 64), fw, 64, 64, -1, 0, NULL, NULL, NULL) == 0);                                // feature SET: acknowledged, dropped
    CHECK(urb(s, 0, 0, setup(st, 0x01, 11, 1, 2, 0), NULL, 0, 0, -1, 0, NULL, NULL, NULL) == 0);                                     // SET_INTERFACE alt 1 on interface 2
    CHECK(urb(s, 1, 0, setup(st, 0x81, 10, 0, 2, 1), NULL, 0, 1, -1, 0, NULL, buf, &n) == 0 && n == 1 && buf[0] == 1);
    double t = now();                                                                                                                  // parked interrupt IN, answered by the feeder
    CHECK(urb(s, 1, int_in, NULL, NULL, 0, 64, -1, 0, NULL, buf, &n) == 0 && n == 64 && buf[0] == 1 && buf[1] == 0x80 && buf[8] == 0x08 && now() - t < 1);
    uint32_t d8[16];
    for (int k = 0; k < 8; k++) { d8[2 * k] = k * 392; d8[2 * k + 1] = 392; }
    uint8_t* zeros = calloc(1, 8 * 392);
    t = now();                                                                                                                         // iso OUT paced: 8 x 1 ms
    CHECK(urb(s, 0, iso_out, NULL, zeros, 8 * 392, 8 * 392, 8, 8, d8, NULL, NULL) == 0 && now() - t > 0.006 && now() - t < 0.5);
    free(zeros);
    for (int k = 0; k < 8; k++) { d8[2 * k] = k * 196; d8[2 * k + 1] = 196; }
    int ok = urb(s, 1, iso_in, NULL, NULL, 0, 8 * 196, 8, 8, d8, buf, &n) == 0 && n == 8 * 192;   // iso IN (mic): 48 stereo frames per ms of silence
    for (size_t i = 0; i < n; i++) ok &= buf[i] == 0;
    CHECK(ok);
    // UNLINK of a parked interrupt IN: the RET_UNLINK status is 0 or -ECONNRESET (if the feeder answered first, a RET_SUBMIT comes before it)
    uint8_t h[48] = {0};
    uint32_t parked = ++g_seq;
    p32(h, 1); p32(h + 4, parked); p32(h + 8, 0x10002); p32(h + 12, 1); p32(h + 16, int_in); p32(h + 24, 64); p32(h + 32, (uint32_t)-1);
    send(s, h, 48, 0);
    usleep(10000);
    memset(h, 0, 48);
    p32(h, 2); p32(h + 4, ++g_seq); p32(h + 8, 0x10002); p32(h + 20, parked);
    send(s, h, 48, 0);
    uint8_t r[48];
    rx(s, r, 48);
    while (g32(r) == 3) { int a = (int32_t)g32(r + 24); if (a > 0) rx(s, buf, a); rx(s, r, 48); }
    CHECK(g32(r) == 4 && ((int32_t)g32(r + 20) == 0 || (int32_t)g32(r + 20) == -104));
    close(s);
    // a debug-port client that asks for far more than the kernel ever does is dropped, not served until memory runs out
    s = import_conn();
    uint8_t big[48] = {0};
    p32(big, 1); p32(big + 4, ++g_seq); p32(big + 8, 0x10002); p32(big + 12, 0); p32(big + 16, 1); p32(big + 24, 0x7fffffff);   // an OUT URB of 2 GB
    send(s, big, 48, 0);
    CHECK(closed_by_server(s));
    close(s);
    s = import_conn();
    for (int i = 0; i < 100; i++) {                       // 100 interrupt-IN URBs at once: the server parks at most 64
        uint8_t u[48] = {0};
        p32(u, 1); p32(u + 4, ++g_seq); p32(u + 8, 0x10002); p32(u + 12, 1); p32(u + 16, int_in); p32(u + 24, 64);
        send(s, u, 48, 0);
    }
    CHECK(closed_by_server(s));
    close(s);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    if (fails) { printf("test_proto FAILED (%d)\n", fails); return 1; }
    printf("test_proto ok: devlist, import, descriptors, strings, HID get/set, feature SET dropped, stall, UAC controls, SET_INTERFACE, interrupt IN, iso OUT pacing, iso IN mic silence, UNLINK, oversized/flooding clients dropped\n");
    return 0;
}
