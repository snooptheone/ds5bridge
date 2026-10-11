// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// ds5ctl against a fake pad: `rate` and `stress` talk to the Bluetooth pad's hidraw node, which a pty plays (--hidraw). The first version of `stress` hung forever
// on a real pad (a blocking read that never ends) and nothing in `make test` noticed: this does. The fake pad sends input reports at ~70/s and swallows
// what ds5ctl writes.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include "fakepad.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// runs bin/ds5ctl --hidraw PAD args..., feeding the pad meanwhile; returns the exit status, the program's output goes to out
static int run_ctl(Pty* p, const char* a1, const char* a2, char* out, size_t cap) {
    int pipefd[2];
    if (pipe(pipefd)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], 1); dup2(pipefd[1], 2);
        close(pipefd[0]);
        execl("bin/ds5ctl", "ds5ctl", "--hidraw", p->path, a1, a2, (char*)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(p->master, F_SETFL, O_NONBLOCK);
    size_t n = 0;
    int status = -1;
    double end = fp_now() + 30;
    for (;;) {
        send_report(p, 0);
        uint8_t sink[4096];
        while (read(p->master, sink, sizeof sink) > 0) {}      // what ds5ctl writes to the "pad"
        ssize_t r;
        while ((r = read(pipefd[0], out + n, cap - 1 - n)) > 0) n += r;
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        if (fp_now() > end) { kill(pid, SIGKILL); waitpid(pid, &status, 0); fprintf(stderr, "ds5ctl did not finish in 30 s\n"); status = -1; break; }
        usleep(14000);
    }
    while ((n < cap - 1) && (read(pipefd[0], out + n, cap - 1 - n)) > 0) n = strlen(out);
    out[n] = 0;
    close(pipefd[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void) {
    alarm(90);
    signal(SIGPIPE, SIG_IGN);
    Pty p = new_pty();
    char out[8192] = "";
    CHECK(run_ctl(&p, "rate", "1", out, sizeof out) == 0);
    CHECK(strstr(out, "reports/s") && strstr(out, "interval ms"));
    memset(out, 0, sizeof out);
    CHECK(run_ctl(&p, "stress", "40", out, sizeof out) == 0);
    CHECK(strstr(out, "20 ") && strstr(out, "40 ") && strstr(out, "no problem up to 40 writes/s"));
    CHECK(!strstr(out, "write failed") && !strstr(out, "input stalled"));
    memset(out, 0, sizeof out);
    CHECK(run_ctl(&p, "battery", NULL, out, sizeof out) == 0);              // the fake pad reports status 0x08: discharging at level 8
    CHECK(strstr(out, "battery 85 % (discharging)"));
    fp_status = 0x18;                                                       // charging
    { uint8_t junk[4096]; fcntl(p.slave_fd, F_SETFL, O_NONBLOCK); while (read(p.slave_fd, junk, sizeof junk) > 0) {} }   // reports queued with the old status
    memset(out, 0, sizeof out);
    CHECK(run_ctl(&p, "battery", NULL, out, sizeof out) == 0);
    CHECK(strstr(out, "battery 85 % (charging)"));
    if (fails) { fprintf(stderr, "--- output ---\n%s\n", out); printf("test_ctl_pad FAILED (%d)\n", fails); return 1; }
    printf("test_ctl_pad ok: ds5ctl rate, stress and battery finish and report against a fake pad\n");
    close_pty(&p);
    return 0;
}
