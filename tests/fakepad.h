// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// A fake Bluetooth pad for the tests: a pty whose slave end plays the pad's hidraw node (daemon and ds5ctl take it with --hidraw).
#ifndef FAKEPAD_H
#define FAKEPAD_H
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct { int master, slave_fd; char path[128]; } Pty;

static double fp_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static Pty new_pty(void) {
    Pty p;
    p.master = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(p.master); unlockpt(p.master);
    snprintf(p.path, sizeof p.path, "%s", ptsname(p.master));
    p.slave_fd = open(p.path, O_RDWR | O_NOCTTY);
    struct termios t;
    tcgetattr(p.slave_fd, &t); cfmakeraw(&t); tcsetattr(p.slave_fd, TCSANOW, &t);   // raw: a 78-byte write arrives as one read
    return p;
}
static void close_pty(Pty* p) { close(p->master); close(p->slave_fd); }

// a Bluetooth input report 0x31: sticks centred, d-pad released, touch points idle, `button` in the first button byte (L1/R1/L2/R2/create/options/L3/R3)
static uint8_t fp_status = 0x08;   // USB byte 53: battery status (low nibble level, high nibble charging state): 0x08 = discharging at level 8 = 85 %
static inline void send_report(Pty* p, int button) {
    uint8_t r[78] = {0};
    r[0] = 0x31; r[1] = 0x01;
    memset(r + 2, 0x80, 4);        // USB bytes 1-4: sticks
    r[9] = 0x08;                   // USB byte 8: hat switch at rest
    r[10] = button;                // USB byte 9
    r[2 + 32] = r[2 + 36] = 0x80;  // touch points: not touching
    r[54] = fp_status;             // USB byte 53: battery
    (void)!write(p->master, r, sizeof r);
}
#endif
