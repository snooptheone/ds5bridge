// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// Several Bluetooth pads at once: ptys play the pads' hidraw nodes (--hidraw, five of them: one more than the daemon bridges), vhci_hcd is faked (--fake-vhci). Checks that
// each pad gets its own virtual pad on its own vhci port, that a pad's input reaches only its own virtual pad, that the game's output for one virtual pad reaches only that
// real pad, that a pad which answers its feature reads slowly (--feature-delay-ms) does not stall the pad that is already running, that one pad dropping removes only its own
// virtual pad after the grace period, that the returning pad gets its own slot back, that a fifth pad is refused with a warning, and that it takes the slot a pad left for
// good as soon as there is one. Takes ~35 s.
#include "padtest.h"

#define DELAY_MS 1500   // every new connection spends this long reading its feature reports (in the daemon's thread)

static void link_pad(const char* link, Pty* p) { *p = new_pty(); (void)!symlink(p->path, link); }
static void drop_pad(const char* link, Pty* p) { close_pty(p); (void)!unlink(link); }

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    PORT = free_port();
    alarm(150);
    char dir[] = "/tmp/ds5multi.XXXXXX", link[5][160];
    if (!mkdtemp(dir)) return 1;
    for (int i = 0; i < 5; i++) snprintf(link[i], sizeof link[i], "%s/pad%c", dir, 'A' + i);
    int errpipe[2];
    if (pipe(errpipe)) return 1;
    char port[16], delay[16];
    snprintf(port, sizeof port, "%d", PORT);
    snprintf(delay, sizeof delay, "%d", DELAY_MS);
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        dup2(errpipe[1], 2);
        close(errpipe[0]);
        execl("bin/ds5bridge", "ds5bridge", "--hidraw", link[0], "--hidraw", link[1], "--hidraw", link[2], "--hidraw", link[3], "--hidraw", link[4], "--fake-vhci", "--port", port,
              "--qos-latency", "0", "--grace", "3", "--no-haptics", "--feature-delay-ms", delay, (char*)NULL);
        _exit(127);
    }
    close(errpipe[1]);
    logfd = errpipe[0];
    CHECK(wait_for("ds5bridge started", 6));

    // 1. pad A appears: its virtual pad comes up on vhci port 0 once the feature reads are in
    Pty a, b, c, d, e;
    link_pad(link[0], &a);
    CHECK(wait_for("attached to vhci_hcd port 0 (fake)", 10));

    // 2. pad B appears and takes DELAY_MS to answer: meanwhile pad A keeps working (its changing buttons keep reaching its virtual pad), and B's virtual pad comes up only afterwards
    set_mark();
    double t_b = now();
    link_pad(link[1], &b);
    int calls = 0, slow = 0;
    uint8_t in1[64] = {0}, in2[64] = {0};
    for (int i = 0; i < 60 && !seen("attached to vhci_hcd port 1 (fake)"); i++) {   // until B's virtual pad is up
        int btn = i % 2 ? 0x08 : 0x10;
        send_report(&a, btn);
        usleep(100000);
        double t0 = now();
        int ok = virtual_pad_input("1-1", in1) == 0 && in1[9] == btn;
        calls++;
        slow += !ok || now() - t0 > 0.4;   // every answer must come at once, with the latest button: a stalled loop shows up as a late (or stale) answer
        pump(0.05);
    }
    CHECK(calls >= 8 && slow == 0);
    CHECK(now() - t_b > 1.2);
    CHECK(seen("attached to vhci_hcd port 1 (fake)"));

    // 3. each virtual pad reports its own pad's buttons (USB byte 9: 0x08 on pad A, 0x10 on pad B)
    send_report(&a, 0x08);
    send_report(&b, 0x10);
    usleep(150000);
    CHECK(virtual_pad_input("1-1", in1) == 0 && in1[9] == 0x08);
    CHECK(virtual_pad_input("1-2", in2) == 0 && in2[9] == 0x10);

    // 4. the game's output for virtual pad 2 reaches pad B and not pad A
    int game2 = tcp_import("1-2");
    CHECK(game2 >= 0);
    uint8_t rep[63] = {2, 0x04}, frame[78];
    memset(rep + 11, 0x21, 11); rep[12] = 0xAA;
    CHECK(game_output(game2, rep) == 0 && pad_frame(&b, frame, 3) == 0);
    CHECK(frame[0] == 0x31 && frame[3] == 0x04 && frame[13] == 0x21 && frame[14] == 0xAA);
    CHECK(pad_frame(&a, frame, 0.6) != 0);   // nothing arrived at pad A

    // 5. pad B drops: after the grace period only its virtual pad goes; pad A's keeps working
    set_mark();
    drop_pad(link[1], &b);
    CHECK(wait_for("Bluetooth pad gone", 4));
    CHECK(wait_for("removing the virtual pad", 8) && wait_for("detached vhci_hcd port 1 (fake)", 2));
    CHECK(!seen("detached vhci_hcd port 0"));
    send_report(&a, 0x08);
    usleep(100000);
    CHECK(virtual_pad_input("1-1", in1) == 0 && in1[9] == 0x08);

    // 6. pad B comes back: it gets its own slot (and vhci port 1) again
    set_mark();
    link_pad(link[1], &b);
    CHECK(wait_for("attached to vhci_hcd port 1 (fake)", 10));
    send_report(&b, 0x20);
    usleep(150000);
    CHECK(virtual_pad_input("1-2", in2) == 0 && in2[9] == 0x20);

    // 7. pads C and D fill the other two slots; a fifth pad (E) is refused with a warning
    set_mark();
    link_pad(link[2], &c);
    link_pad(link[3], &d);
    CHECK(wait_for("attached to vhci_hcd port 2 (fake)", 10) && wait_for("attached to vhci_hcd port 3 (fake)", 10));
    set_mark();
    link_pad(link[4], &e);
    CHECK(wait_for("more than 4 Bluetooth pads", 6));
    pump(1.5);
    CHECK(!seen("attached to vhci_hcd port 4"));

    // 8. pad C leaves for good: its slot goes to the waiting pad E, whose virtual pad is the third one ("1-3")
    set_mark();
    drop_pad(link[2], &c);
    CHECK(wait_for("detached vhci_hcd port 2 (fake)", 10));
    CHECK(wait_for("attached to vhci_hcd port 2 (fake)", 10));
    send_report(&e, 0x40);
    usleep(150000);
    uint8_t in3[64] = {0};
    CHECK(virtual_pad_input("1-3", in3) == 0 && in3[9] == 0x40);

    // 9. SIGTERM detaches every virtual pad
    set_mark();
    kill(pid, SIGTERM);
    CHECK(wait_for("detached vhci_hcd port 0 (fake)", 4) && wait_for("detached vhci_hcd port 1 (fake)", 4) && wait_for("detached vhci_hcd port 3 (fake)", 4) && wait_for("stopped", 4));
    waitpid(pid, NULL, 0);
    close_pty(&a); close_pty(&b); close_pty(&d); close_pty(&e);
    for (int i = 0; i < 5; i++) unlink(link[i]);
    rmdir(dir);
    if (fails) { fprintf(stderr, "--- daemon log ---\n%s\n", logbuf); printf("test_multipad FAILED (%d)\n", fails); return 1; }
    printf("test_multipad ok: slow pad does not stall the running one, per-pad virtual pads, input and output kept apart, drop and return, fifth pad refused, freed slot reused\n");
    return 0;
}
