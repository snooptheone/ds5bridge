// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// Lifecycle test of ds5bridge with a fake pad: a pty plays the hidraw node of the Bluetooth pad (--hidraw) and vhci_hcd is faked (--fake-vhci), so no
// hardware and no root are needed. Checks, from the daemon's log: the virtual pad appears when the pad does, a report becomes the virtual pad's input,
// activity keeps the idle timer from firing and stillness lets it fire, a pad that returns inside the grace period keeps its virtual pad, a pad that
// stays away loses it, and a new pad gets a new one. Takes ~25 s.
#include "padtest.h"

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    PORT = free_port();
    alarm(120);   // a hung daemon must fail the test, not hang it (the child dies with us: PDEATHSIG)
    char dir[] = "/tmp/ds5lifecycle.XXXXXX", link[160];
    if (!mkdtemp(dir)) return 1;
    snprintf(link, sizeof link, "%s/pad", dir);
    int errpipe[2];
    if (pipe(errpipe)) return 1;
    char port[16];
    snprintf(port, sizeof port, "%d", PORT);
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        dup2(errpipe[1], 2);
        close(errpipe[0]);
        execl("bin/ds5bridge", "ds5bridge", "--hidraw", link, "--fake-vhci", "--port", port, "--qos-latency", "0", "--idle-off-sec", "2", "--grace", "4", "--no-haptics", (char*)NULL);
        _exit(127);
    }
    close(errpipe[1]);
    logfd = errpipe[0];
    CHECK(wait_for("ds5bridge started", 6));

    // 1. the pad appears: the daemon opens it and attaches a virtual pad
    Pty p1 = new_pty();
    (void)!symlink(p1.path, link);
    CHECK(wait_for("Bluetooth pad ", 8) && wait_for("attached to vhci_hcd port 0 (fake)", 4));

    // 2. a report from the pad is what the virtual pad reports to the host (USB byte 9 = the button byte, byte 54 bit 3 = "plugged by USB")
    send_report(&p1, 0x08);
    usleep(100000);
    uint8_t in[64] = {0};
    CHECK(virtual_pad_input("1-1", in) == 0 && in[0] == 1 && in[1] == 0x80 && in[8] == 0x08 && in[9] == 0x08 && (in[54] & 0x08));

    // 2b. the game sets a right-trigger effect: it reaches the pad as a Bluetooth report (flag0 bit 2, effect in the common block at 10..20 -> frame 13..23)
    int game = tcp_import("1-1");
    CHECK(game >= 0);
    uint8_t out_rep[63] = {2, 0x04}, frame[78];
    memset(out_rep + 11, 0x21, 11); out_rep[12] = 0xAA;
    CHECK(game_output(game, out_rep) == 0 && pad_frame(&p1, frame, 3) == 0);
    CHECK(frame[0] == 0x31 && frame[2] == 0x10 && frame[3] == 0x04 && frame[13] == 0x21 && frame[14] == 0xAA);

    // 3. activity keeps the idle timer (2 s) from firing; stillness lets it fire
    set_mark();
    for (int i = 0; i < 10; i++) { send_report(&p1, 0x08); pump(0.3); }                  // 3 s of a held button
    CHECK(!seen("powering the pad off"));
    for (int i = 0; i < 27 && !seen("powering the pad off"); i++) { send_report(&p1, 0x00); pump(0.3); }   // then stillness, for as long as a slow machine needs
    CHECK(seen("powering the pad off"));

    // 4. the pad drops and comes back inside the grace period (4 s): same virtual pad
    Pty p2 = new_pty();
    set_mark();
    close_pty(&p1);   // the link now points at a pty that no longer exists: the pad is away until it is re-pointed
    CHECK(wait_for("Bluetooth pad gone", 4));
    uint8_t led_rep[63] = {2, 0, 0x04};
    led_rep[45] = 7; led_rep[46] = 8; led_rep[47] = 9;                      // lightbar color, set while the pad is away: only remembered
    CHECK(game_output(game, led_rep) == 0);
    (void)!unlink(link);
    (void)!symlink(p2.path, link);
    CHECK(wait_for("pad is back within the grace period", 8));
    double t_back = now();
    CHECK(pad_frame(&p2, frame, 5) == 0);
    CHECK(now() - t_back > 0.6);                                            // not at once: the kernel's own start-up reports must go first                                   // the replay (a second after the pad is back): the trigger effect and the color, in one report
    CHECK(frame[0] == 0x31 && frame[3] == 0x04 && frame[4] == 0x04 && frame[13] == 0x21 && frame[14] == 0xAA && frame[47] == 7 && frame[48] == 8 && frame[49] == 9 && frame[5] == 0);
    pump(5);
    CHECK(!seen("removing the virtual pad") && !seen("detached vhci_hcd"));

    // 5. the pad drops and stays away: after the grace period the virtual pad is removed
    set_mark();
    close_pty(&p2);
    unlink(link);
    CHECK(wait_for("Bluetooth pad gone", 4));
    CHECK(wait_for("removing the virtual pad", 10) && wait_for("detached vhci_hcd port 0 (fake)", 2));

    // 6. a new pad gets a new virtual pad
    Pty p3 = new_pty();
    set_mark();
    (void)!symlink(p3.path, link);
    CHECK(wait_for("Bluetooth pad ", 8) && wait_for("attached to vhci_hcd port 0 (fake)", 4));

    // 7. SIGTERM: detaches and stops
    set_mark();
    kill(pid, SIGTERM);
    CHECK(wait_for("detached vhci_hcd port 0 (fake)", 4) && wait_for("stopped", 4));
    waitpid(pid, NULL, 0);
    close_pty(&p3);
    unlink(link);
    rmdir(dir);
    if (fails) { fprintf(stderr, "--- daemon log ---\n%s\n", logbuf); printf("test_lifecycle FAILED (%d)\n", fails); return 1; }
    printf("test_lifecycle ok: attach on pad, report -> virtual input, idle timer, return inside grace, removal after grace, new pad, clean stop\n");
    return 0;
}
