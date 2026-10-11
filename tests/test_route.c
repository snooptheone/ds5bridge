// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// The audio route end to end, through the daemon: the game's route report (what Wine sends at start-up: audio control 0x3c, speaker volume 0x64) reaches the pad with
// --output headset and --speaker-volume 80 applied, and when sound starts the route is written to the pad again BEFORE the first 0x39 audio packet, whose speaker
// sub-packet then carries the headphone header (0xD6, not 0xD3). The unit tests only count the callback; this checks the bytes on the wire. Takes ~6 s.
#include "padtest.h"

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    PORT = free_port();
    alarm(60);
    char dir[] = "/tmp/ds5route.XXXXXX", link[160];
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
        execl("bin/ds5bridge", "ds5bridge", "--hidraw", link, "--fake-vhci", "--port", port, "--qos-latency", "0", "--output", "headset", "--speaker-volume", "80", (char*)NULL);
        _exit(127);
    }
    close(errpipe[1]);
    logfd = errpipe[0];
    CHECK(wait_for("ds5bridge started", 6));
    Pty p = new_pty();
    (void)!symlink(p.path, link);
    CHECK(wait_for("attached to vhci_hcd port 0 (fake)", 8));
    int game = tcp_import("1-1");
    CHECK(game >= 0);

    // 1. the game's route report is forwarded with the overrides: speaker volume 80 (not 0x64) and the output path headphones (bits 4-5 of audio control cleared, echo/noise bits kept)
    uint8_t rep[63] = {2, 0xA0}, m[600];
    rep[6] = 0x64; rep[8] = 0x3C;
    CHECK(game_output(game, rep) == 0);
    CHECK(pad_message(&p, m, 3) == 78 && m[0] == 0x31 && m[3] == 0xA0 && m[8] == 80 && m[10] == 0x0C);

    // 2. sound starts: the route is said again before the first audio packet
    for (int i = 0; i < 40; i++) game_audio(game, 392, 8000);
    int route_at = -1, audio_at = -1, n = 0, routes_before_audio = 0;
    size_t len;
    uint8_t audio[600] = {0};
    while ((len = pad_message(&p, m, 3)) != 0 && audio_at < 0) {
        if (len == 547 && m[0] == 0x39) { audio_at = n; memcpy(audio, m, len); }
        else if (len == 78 && m[0] == 0x31 && m[3] == 0xA0 && m[8] == 80 && m[10] == 0x0C) { if (route_at < 0) route_at = n; routes_before_audio++; }
        n++;
    }
    CHECK(audio_at >= 0);
    CHECK(route_at >= 0 && route_at < audio_at && routes_before_audio == 1);
    CHECK(audio[140] == 0xD6 && audio[141] == 200);   // the speaker sub-packet for the headphone output (0x16 | 0xC0), 200 bytes of Opus per frame

    set_mark();
    kill(pid, SIGTERM);
    CHECK(wait_for("stopped", 4));
    waitpid(pid, NULL, 0);
    close_pty(&p);
    unlink(link);
    rmdir(dir);
    if (fails) { fprintf(stderr, "--- daemon log ---\n%s\n", logbuf); printf("test_route FAILED (%d)\n", fails); return 1; }
    printf("test_route ok: the game's route is forced by --output/--speaker-volume, and written again before the first audio packet, which uses the headphone header\n");
    return 0;
}
