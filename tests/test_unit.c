// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// Unit tests of ds5bridge's internals (packets, descriptors, control requests, the haptics filter, Opus, the activity detector). They live here and not
// in the daemon: this file includes the daemon's source (its main renamed) so the tests can reach the static functions, and the shipped binary
// carries no test code.   ./bin/test_unit                     run the checks
//                          ./bin/test_unit --dump-packets      print the fixed packets compared with tests/golden.txt
#define main ds5bridge_main
#include "../src/ds5bridge.c"
#undef main

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static double zero_cross_freq(const float* y, size_t n, double rate) {
    int z = 0;
    for (size_t i = 1; i < n; i++) z += (y[i - 1] < 0) != (y[i] < 0);
    return z / 2.0 / (n / rate);
}
typedef struct { uint8_t p[512][DS5_AUDIO_LEN]; size_t n[512]; int count; } Sink;
static void sink_send(void* c, const uint8_t* pkt, size_t n) { Sink* s = c; if (s->count < 512) { memcpy(s->p[s->count], pkt, n); s->n[s->count++] = n; } }
static int one_seq(void* c) { (void)c; return 1; }
static Shadow sh;   // the remembered game state the tests build up
static int g_route_calls;
static void nop_send(void* c, const uint8_t* pkt, size_t n) { (void)c; (void)pkt; (void)n; }
static void count_route(void* c) { (void)c; g_route_calls++; }
static uint8_t g_got[64];
static size_t g_got_n;
static void capture_output(Device* d, const uint8_t* r, size_t n) { (void)d; g_got_n = MIN(n, 64); memcpy(g_got, r, g_got_n); }

// Feeds half a second of a 0.5-scale sine to channels ch0 and ch0+1 of a fresh BtAudio and collects the Bluetooth packets it produces.
static void run_tone(Sink* sink, double freq, int ch0, int speaker) {
    sink->count = 0;
    BtAudio ba;
    bta_init(&ba, sink_send, one_seq, sink, 1.0, speaker, 0, 100);
    enum { FR = 24000 };
    uint8_t* pcm = calloc(FR, 8);
    for (int i = 0; i < FR; i++) {
        int16_t x = (int16_t)(16384 * sin(2 * M_PI * freq * i / 48000.0));
        for (int c = ch0; c < ch0 + 2; c++) { pcm[i * 8 + c * 2] = x & 0xFF; pcm[i * 8 + c * 2 + 1] = (x >> 8) & 0xFF; }
    }
    for (int k = 0; k < FR; k += 384) bta_feed(&ba, pcm + k * 8, MIN(384, FR - k) * 8);
    free(pcm);
    bta_free(&ba);
}

static int selftest(void) {
    const char* s = "123456789";
    CHECK(~ds5_crc32_run(0xFFFFFFFFu, (const uint8_t*)s, 9) == 0xCBF43926u);  // plain crc32 check value
    uint8_t sp[DS5_STATE_LEN], mp[DS5_MIC_STATUS_LEN], mv[DS5_STATE_LEN], mo[DS5_MIC_STATUS_LEN];
    ds5_state_packet(2, sp);
    CHECK(sp[0] == 0x31 && sp[1] == 0x20 && sp[2] == 0x10 && sp[3] == 0x01);
    ds5_mic_status_packet(2, 1, mp); ds5_mic_status_packet(1, 0, mo);
    CHECK(mp[0] == 0x32 && mp[1] == 0x20 && mp[2] == 0x91 && mp[3] == 1 && mp[4] == 3 && mo[4] == 2);
    ds5_mic_volume_packet(3, 64, mv);
    CHECK(mv[3] == 0xC0 && mv[9] == 64 && mv[10] == 1);
    // the emulated device: embedded descriptors, per-unit features taken from the pad
    uint8_t f9[20] = {9, 0x27, 0xdf, 0x9d, 0xc1, 0xbc, 0xd0};
    Device dev;
    dev_init(&dev);
    dev_set_features(&dev, NULL, 0, f9, 20, NULL, 0);
    uint8_t su[8], rep[1024];
    size_t rn;
#define SETUP(bm, br, wv, wi, wl) do { su[0] = bm; su[1] = br; su[2] = (wv) & 255; su[3] = (wv) >> 8; su[4] = (wi) & 255; su[5] = (wi) >> 8; su[6] = (wl) & 255; su[7] = (wl) >> 8; } while (0)
    SETUP(0x80, 6, 0x0100, 0, 18);
    CHECK(dev_control(&dev, su, NULL, 0, rep, &rn) == 0 && rn == 18 && !memcmp(rep, DS5_DEVICE_DESC, 18));
    SETUP(0x80, 6, 0x0200, 0, 4096);
    CHECK(dev_control(&dev, su, NULL, 0, rep, &rn) == 0 && rn == sizeof DS5_CONFIG_DESC && !memcmp(rep, DS5_CONFIG_DESC, rn));
    SETUP(0x81, 6, 0x2200, 3, 4096);
    CHECK(dev_control(&dev, su, NULL, 0, rep, &rn) == 0 && rn == sizeof DS5_HID_REPORT_DESC);
    SETUP(0x80, 6, 0x0302, 0, 255);
    CHECK(dev_control(&dev, su, NULL, 0, rep, &rn) == 0 && rn == 2 + 2 * strlen(DS5_STRING_PRODUCT) && rep[2] == 'D');
    SETUP(0xA1, 1, 0x0309, 3, 20);
    CHECK(dev_control(&dev, su, NULL, 0, rep, &rn) == 0 && rn == 20 && rep[0] == 9 && rep[1] == 0x27 && rep[2] == 0xdf && rep[6] == (0xd0 ^ 0x02));  // the pad's address with the locally administered bit of its first octet flipped
    SETUP(0xC0, 0x77, 0, 0, 4);
    CHECK(dev_control(&dev, su, NULL, 0, rep, &rn) == EPIPE_);
    uint8_t v[2] = {0x34, 0x12};
    SETUP(0x21, 1, 0x0200, 0x0201, 2);
    CHECK(dev_control(&dev, su, v, 2, rep, &rn) == 0);
    SETUP(0xA1, 0x81, 0x0200, 0x0201, 2);
    CHECK(dev_control(&dev, su, NULL, 0, rep, &rn) == 0 && rn == 2 && rep[0] == 0x34 && rep[1] == 0x12);
    dev.on_output = capture_output;
    uint8_t o3[3] = {2, 4, 0};
    SETUP(0x21, 9, 0x0202, 3, 3);
    CHECK(dev_control(&dev, su, o3, 3, rep, &rn) == 0 && g_got_n == 3 && g_got[1] == 4);
    uint8_t fw[3] = {0xF4, 1, 2};   // a feature SET (firmware update, test commands...) is acknowledged and dropped, never reaches the pad
    g_got_n = 0;
    SETUP(0x21, 9, 0x03F4, 3, 3);
    CHECK(dev_control(&dev, su, fw, 3, rep, &rn) == 0 && g_got_n == 0);
    CHECK(dev.hid_if == 3);
    // activity detection: gyro noise and stick drift are not activity; buttons, triggers, touch and real stick moves are
    {
        uint8_t u[64] = {0}, ref[4];
        int have = 0;
        u[0] = 1; memset(u + 1, 0x80, 4); u[8] = 0x08; u[33] = u[37] = 0x80;
        CHECK(!pad_active(u, ref, &have));
        u[16] = 55; u[22] = 99; u[7] = 3; u[1] = 0x84;   // gyro/accel/counter noise, tiny stick drift
        CHECK(!pad_active(u, ref, &have));
        u[1] = 0x80 + 40;
        CHECK(pad_active(u, ref, &have));                // a real stick move
        CHECK(!pad_active(u, ref, &have));               // held still afterwards: no new activity
        u[9] = 0x08;
        CHECK(pad_active(u, ref, &have));                // a button
        u[9] = 0; u[5] = 200;
        CHECK(pad_active(u, ref, &have));                // a trigger
        u[5] = 0; u[8] = 0x18;
        CHECK(pad_active(u, ref, &have));                // a face button
        u[8] = 0x08; u[33] = 0x00;
        CHECK(pad_active(u, ref, &have));                // a finger on the touchpad
    }
    uint8_t po[47];
    ds5_power_off_report(po);
    CHECK(po[0] == 0x08 && po[1] == 2 && po[43] == po[43] && ds5_crc32_run(ds5_crc32_run(0xFFFFFFFFu, (const uint8_t[]){0x53}, 1), po, 43) == ~(uint32_t)(po[43] | po[44] << 8 | po[45] << 16 | (uint32_t)po[46] << 24));
    // haptics: 80 Hz on ch3-4 -> level ~63, state packet first, no speaker sub-packet
    {
        static Sink sink;
        run_tone(&sink, 80, 2, 1);
        CHECK(sink.count > 20 && sink.p[0][0] == 0x31);
        int pk = 0, ok = 1;
        for (int i = 1; i < sink.count; i++) {
            ok &= sink.n[i] == DS5_AUDIO_LEN && sink.p[i][0] == 0x39 && sink.p[i][140] == 0;
            for (int j = 12; j < 140; j++) pk = MAX(pk, abs((int8_t)sink.p[i][j]));
        }
        CHECK(ok && pk >= 55 && pk <= 66);
    }
    // haptics filter: flat in the haptic band, and tones above the new Nyquist (1.5 kHz) must not alias down into it
    // (a plain 16-sample average, the first version, was 1.6 dB down at 1 kHz and left ~12 of 63 at 2.5 kHz: this would fail for it)
    {
        static const struct { double freq; int lo, hi; } cases[] = {
            {100, 62, 65}, {500, 62, 65}, {1000, 62, 65},                       // passband: ideal amplitude 63.5 (0.5 full scale * 127)
            {2500, 0, 1}, {4000, 0, 1}, {7000, 0, 1}, {11000, 0, 1}};           // stopband: these alias to 500 / 1000 Hz
        for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
            static Sink sink;
            run_tone(&sink, cases[k].freq, 2, 0);
            // amplitude from the RMS of the left channel (exact for these tones; the sampled peak depends on the phase: 6 samples per cycle at 500 Hz)
            double sq = 0;
            int cnt = 0;
            for (int i = 4; i < sink.count; i++)
                for (int j = 12; j < 140; j += 2) { int v = (int8_t)sink.p[i][j]; sq += v * v; cnt++; }
            int amp = (int)lround(sqrt(2 * sq / cnt));
            if (amp < cases[k].lo || amp > cases[k].hi) fprintf(stderr, "haptics: %.0f Hz -> amplitude %d, expected %d..%d\n", cases[k].freq, amp, cases[k].lo, cases[k].hi);
            CHECK(amp >= cases[k].lo && amp <= cases[k].hi);
        }
    }
    // speaker: 440 Hz on ch1-2 -> 0xD3 sub-packets that decode to ~469 Hz (the dongle's 512->480 time compression)
    {
        static Sink sink;
        run_tone(&sink, 440, 0, 1);
        int err = 0, np = 0;
        OpusDecoder* d = opus_decoder_create(48000, 2, &err);
        float* y = malloc(sizeof(float) * 480 * 2 * 512);
        size_t ny = 0;
        for (int i = 1; i < sink.count; i++) {
            if (sink.p[i][140] != 0xD3) continue;
            CHECK(sink.p[i][141] == 200 && sink.n[i] == DS5_AUDIO_LEN);
            np++;
            for (int fr = 0; fr < 2; fr++) {
                float o[960];
                int n = opus_decode_float(d, &sink.p[i][142 + 200 * fr], 200, o, 480, 0);
                CHECK(n == 480);
                if (np > 2) for (int k = 0; k < 480; k++) y[ny++] = o[k * 2];
            }
        }
        opus_decoder_destroy(d);
        CHECK(np > 20);
        CHECK(fabs(zero_cross_freq(y, ny, 48000) - 469.3) < 12);
        free(y);
    }
    // what the game set is remembered field by field and restored after a drop: right and left triggers from separate reports, LEDs from another
    {
        memset(&sh, 0, sizeof sh);
        uint8_t r[63] = {0}, out[63];
        CHECK(!shadow_report(&sh, out, 0));                                          // nothing set yet: nothing to replay
        r[0] = 2; r[1] = 0x04; memset(r + 11, 0x21, 11); r[12] = 0xAA;       // right trigger effect (mode 0x21, byte 1 = 0xAA)
        shadow_merge(&sh, r, sizeof r);
        memset(r, 0, sizeof r); r[0] = 2; r[1] = 0x08; memset(r + 22, 0x26, 11);   // left trigger effect, a separate report
        shadow_merge(&sh, r, sizeof r);
        memset(r, 0, sizeof r); r[0] = 2; r[2] = 0x04; r[45] = 1; r[46] = 2; r[47] = 3; r[1] = 0x01; r[3] = 99;   // lightbar color; a rumble byte that must NOT be replayed
        shadow_merge(&sh, r, sizeof r);
        memset(r, 0, sizeof r); r[0] = 2; r[2] = 0x10; r[44] = 0x05;         // player LEDs
        shadow_merge(&sh, r, sizeof r);
        CHECK(shadow_report(&sh, out, 0));
        CHECK(out[0] == 2 && out[1] == 0x0C && out[2] == 0x14);              // both triggers, lightbar + player LEDs; the rumble flag is not there
        CHECK(out[11] == 0x21 && out[12] == 0xAA && out[22] == 0x26 && out[32] == 0x26);
        CHECK(out[45] == 1 && out[46] == 2 && out[47] == 3 && out[44] == 0x05 && out[3] == 0);
        memset(r, 0, sizeof r); r[0] = 2; r[1] = 0x04; r[11] = 0x05;         // the game turns the right trigger effect off: that is what comes back
        shadow_merge(&sh, r, sizeof r);
        shadow_report(&sh, out, 0);
        CHECK(out[11] == 0x05 && out[22] == 0x26);
        memset(&sh, 0, sizeof sh);
    }
    // the log note table: a game that varies its rumble must not fill it, and an error of a new kind must get through even when it is full
    {
        g_log = tmpfile();
        int before = g_nnotes;
        uint8_t o[63] = {2};
        for (int i = 0; i < 400; i++) { o[3] = i & 255; o[4] = (i * 7) & 255; o[11] = i >> 2; log_output(o, sizeof o); }
        CHECK(g_nnotes <= before + 1);                                      // 400 different rumble values and effects: one kind
        char k[32];
        for (int i = 0; i < 300; i++) { snprintf(k, sizeof k, "filler-%d", i); note("INFO", k, "x"); }
        CHECK(g_nnotes == 256);
        note("ERROR", "new-error-kind", "boom-after-full");
        fflush(g_log); rewind(g_log);
        char buf[65536];
        size_t n = fread(buf, 1, sizeof buf - 1, g_log);
        buf[n] = 0;
        CHECK(strstr(buf, "boom-after-full"));
        fclose(g_log); g_log = NULL;
    }
    // a slot that passes to another pad must not keep the previous pad's address when the new pad's feature reads fail
    {
        Device d;
        dev_init(&d);
        uint8_t f09[20] = {0x09, 0x10, 2, 3, 4, 5, 6};
        dev_set_features(&d, NULL, 0, f09, sizeof f09, NULL, 0);
        CHECK(d.feat[0x09][1] == 0x10 && d.feat[0x09][6] == (6 ^ 0x02));     // the pad's own, with the locally administered bit of the first octet flipped
        uint8_t kept = d.feat[0x09][6];
        uint8_t f05[41] = {0x05};
        dev_set_features(&d, f05, sizeof f05, NULL, 0, NULL, 0);             // a failed 0x09 read: the address stays as it was, it is not flipped back to the pad's own
        CHECK(d.feat[0x09][6] == kept);
        dev_placeholder_features(&d);
        CHECK(d.feat[0x09][0] == 0x09 && d.feat[0x09][1] == 0 && d.feat[0x09][6] == 0 && d.featlen[0x09] == 20);
    }
    // the microphone: the host having the virtual mic open means something to the pad only with --mic, and the state dies with the virtual pad
    {
        static Pad mp;
        pad_init(&mp);
        mp.dev.mic_active = 1;
        cfg.mic = 0;
        CHECK(!br_mic_on(&mp));
        uint8_t r[63] = {2, 0x80, 0, 0, 0, 0, 0, 0, 0x3C}, out[63] = {0};
        memset(&sh, 0, sizeof sh); sh.f0 = 0x80; sh.rep[8] = 0x3C;
        CHECK(route_report(&sh, out, br_mic_on(&mp)) && out[8] == 0x3C);       // without --mic the game's route goes out untouched (no internal-mic bit)
        cfg.mic = 1;
        CHECK(br_mic_on(&mp));
        route_override(r, br_mic_on(&mp));
        CHECK(r[8] == 0x3D);
        on_vpad_end(&mp);                                                       // the virtual pad is gone: the next session starts with the mic closed
        CHECK(!mp.dev.mic_active && !br_mic_on(&mp));
        cfg.mic = 0;
        memset(&sh, 0, sizeof sh);
    }
    // descriptors the handler table cannot hold are refused, not written past its end (ASan finds the old behaviour)
    {
        fd_add(5000, EPOLLIN, NULL, NULL); fd_mod(5000, EPOLLIN); fd_del(5000); fd_add(-1, EPOLLIN, NULL, NULL); fd_del(-1);
        CHECK(!FD_OK(4096) && FD_OK(4095) && !FD_OK(-1));
    }
    // two pads whose addresses differ only in the lowest bit (made one after the other): no virtual address may equal any real one, or hid-playstation refuses the virtual pad
    {
        uint8_t fa[20] = {0x09, 0x27, 0xDF, 0x9D, 0xC1, 0xBC, 0xD0}, fb[20] = {0x09, 0x26, 0xDF, 0x9D, 0xC1, 0xBC, 0xD0};   // ...:27 and ...:26 (little endian)
        Device da, db;
        dev_init(&da); dev_init(&db);
        dev_set_features(&da, NULL, 0, fa, sizeof fa, NULL, 0);
        dev_set_features(&db, NULL, 0, fb, sizeof fb, NULL, 0);
        CHECK(memcmp(da.feat[0x09] + 1, fa + 1, 6) && memcmp(da.feat[0x09] + 1, fb + 1, 6));   // virtual A is neither A nor B
        CHECK(memcmp(db.feat[0x09] + 1, fa + 1, 6) && memcmp(db.feat[0x09] + 1, fb + 1, 6));
        CHECK(memcmp(da.feat[0x09] + 1, db.feat[0x09] + 1, 6));                                 // and the two virtual pads differ
    }
    // Bluetooth QoS answers: each pad's socket sees every pad's event, only the one for its own connection handle ends its wait
    {
        static Pad qp;
        pad_init(&qp);
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
        qp.qos.fd = sv[0]; qp.qos.handle = 0x0040; qp.qos.latency = 5000;
        uint8_t other[] = {0x04, 0x0D, 0x0B, 0x00, 0x41, 0x00, 0, 0, 0, 0, 0, 0, 0}, own[] = {0x04, 0x0D, 0x0B, 0x00, 0x40, 0x00, 0, 0, 0, 0, 0, 0, 0};
        CHECK(send(sv[1], other, sizeof other, 0) > 0);
        qos_event(sv[0], EPOLLIN, &qp);
        CHECK(qp.qos.fd == sv[0]);                                           // another pad's event: still waiting
        CHECK(send(sv[1], own, sizeof own, 0) > 0);
        qos_event(sv[0], EPOLLIN, &qp);
        CHECK(qp.qos.fd == -1);                                              // its own: done (the socket is closed)
        close(sv[1]);
    }
    // battery: the same table as hid-playstation (10 % steps, +5; charging counts the same way; full is 100; the error states give 0)
    {
        CHECK(ds5_battery(0x00).percent == 5 && !strcmp(ds5_battery(0x00).state, "discharging"));
        CHECK(ds5_battery(0x08).percent == 85 && ds5_battery(0x0A).percent == 100 && ds5_battery(0x0F).percent == 100);   // levels above 10 cannot go over 100
        CHECK(ds5_battery(0x18).percent == 85 && !strcmp(ds5_battery(0x18).state, "charging"));
        CHECK(ds5_battery(0x2A).percent == 100 && !strcmp(ds5_battery(0x2A).state, "full"));
        CHECK(ds5_battery(0xA3).percent == 0 && strstr(ds5_battery(0xA3).state, "not charging"));
        CHECK(ds5_battery(0xF1).percent == 0 && !strcmp(ds5_battery(0xF1).state, "charging error"));
    }
    // the audio route: what Wine sends at start-up (captured from a real USB pad: audio control 0x3c, speaker volume 0x64) is remembered, replayed, and can be forced
    {
        memset(&sh, 0, sizeof sh);
        uint8_t r[63] = {0}, out[63];
        r[0] = 2; r[1] = 0x80; r[2] = 0x40; r[8] = 0x3C;                     // audio control: speaker path + echo/noise bits, audio control 2
        shadow_merge(&sh, r, sizeof r);
        memset(r, 0, sizeof r); r[0] = 2; r[1] = 0x70; r[2] = 0x40; r[6] = 0x64; r[7] = 9;   // volumes; the microphone volume (flag bit 6) is not remembered
        shadow_merge(&sh, r, sizeof r);
        CHECK(route_report(&sh, out, 0) && out[1] == 0xB0 && out[2] == 0x40 && out[6] == 0x64 && out[8] == 0x3C && out[7] == 0);
        CHECK(shadow_report(&sh, out, 0) && out[1] == 0xB0 && out[8] == 0x3C && out[6] == 0x64);
        cfg.output = 2;                                                       // --output headset: the path bits go to 0, the echo/noise bits stay
        CHECK(route_report(&sh, out, 0) && out[8] == 0x0C);
        cfg.output = 1; cfg.speaker_volume = 80;                              // --output speaker --speaker-volume 80
        CHECK(route_report(&sh, out, 0) && (out[8] & 0x30) == 0x30 && out[6] == 80);
        r[0] = 2; r[1] = 0x20; r[6] = 0x50;                                   // a game report is forced the same way before it is forwarded
        route_override(r, 0);
        CHECK(r[6] == 80);
        memset(&sh, 0, sizeof sh);
        cfg.output = 1; cfg.speaker_volume = -1;                              // the game said nothing: --output alone still produces a route report
        CHECK(route_report(&sh, out, 0) && out[1] == 0x80 && out[8] == 0x30 && out[2] == 0);
        cfg.output = 0;
        CHECK(!route_report(&sh, out, 0));                                            // nothing remembered, nothing forced: nothing to say
        CHECK(route_report(&sh, out, 1) && out[1] == 0x80 && out[8] == 0x01);         // the pad mic is on: its select bit stays even when the game said nothing
        sh.rep[8] = 0x3C; sh.f0 |= 0x80;
        CHECK(route_report(&sh, out, 1) && out[8] == 0x3D && route_report(&sh, out, 0) && out[8] == 0x3C);   // with the game's 0x3c: speaker path kept, mic bit added only when the mic is on
        // the route is repeated before the first speaker packet after every silence, not once per packet
        BtAudio* a = malloc(sizeof *a);
        bta_init(a, nop_send, one_seq, NULL, 1.0, 1, 0, 100);
        a->route = count_route;
        g_route_calls = 0;
        uint8_t* tone = calloc(1024, 8);
        for (int i = 0; i < 1024; i++) { int16_t v = i % 2 ? 8000 : -8000; tone[i * 8] = v & 255; tone[i * 8 + 1] = v >> 8; tone[i * 8 + 2] = v & 255; tone[i * 8 + 3] = v >> 8; }
        uint8_t* quiet = calloc(1024, 8);
        bta_feed(a, tone, 1024 * 8); bta_feed(a, tone, 1024 * 8);
        CHECK(g_route_calls == 1);                                            // sound is playing: one route report, before the first packet
        for (int i = 0; i < 40; i++) bta_feed(a, quiet, 1024 * 8);            // more than 30 silent chunks: the speaker lane goes quiet
        bta_feed(a, tone, 1024 * 8);
        CHECK(g_route_calls == 2);                                            // sound again: the route is said again
        bta_free(a); free(a); free(tone); free(quiet);
    }
    // microphone: decoder + 30 ms jitter buffer, then the decoded tone
    {
        int err = 0;
        OpusEncoder* e = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &err);
        opus_encoder_ctl(e, OPUS_SET_BITRATE(56800));
        opus_encoder_ctl(e, OPUS_SET_VBR(0));
        opus_encoder_ctl(e, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
        Mic* mic = mic_new();
        int16_t z[480];
        mic_read(mic, 48, z);
        int any = 0;
        for (int i = 0; i < 48; i++) any |= z[i] != 0;
        CHECK(!any);
        uint8_t fr[40][400];
        int fl[40];
        for (int k = 0; k < 40; k++) {
            float pcm[480];
            for (int i = 0; i < 480; i++) pcm[i] = 0.5f * sinf(2 * M_PI * 440 * (k * 480 + i) / 48000.0);
            fl[k] = opus_encode_float(e, pcm, 480, fr[k], 400);
            CHECK(fl[k] > 40);
        }
        opus_encoder_destroy(e);
        for (int k = 0; k < 2; k++) mic_feed(mic, fr[k], fl[k]);
        mic_read(mic, 48, z);
        any = 0;
        for (int i = 0; i < 48; i++) any |= z[i] != 0;
        CHECK(!mic->primed && !any);  // 20 ms < 30 ms: still silence
        for (int k = 2; k < 40; k++) mic_feed(mic, fr[k], fl[k]);
        float y[480 * 29];
        size_t ny = 0;
        int mx = 0;
        for (int k = 0; k < 30; k++) {
            mic_read(mic, 480, z);
            for (int i = 0; i < 480; i++) { if (k >= 1) y[ny++] = z[i]; mx = MAX(mx, abs(z[i])); }
        }
        CHECK(mx > 8000 && fabs(zero_cross_freq(y, ny, 48000) - 440) < 8);
        mic_free(mic);
    }
    printf(g_fail ? "selftest FAILED (%d)\n" : "selftest ok: CRC, packets, descriptors, strings, HID get/set, stall, haptics ch3-4 (FIR response), Opus speaker ch1-2 (decoded back), output-state replay, audio route (remembered, replayed, forced by --output), microphone (Opus decode, jitter buffer)\n", g_fail);
    return g_fail ? 1 : 0;
}

// Fixed-input packets for tests/golden.txt (recorded from the original Python implementation, which the C code matches byte for byte)
static void dump_packets(void) {
    uint8_t b[DS5_AUDIO_LEN];
    char hex[2 * DS5_AUDIO_LEN + 1];
#define P(name, n) do { ds5_hex(b, n, hex); printf("%s %s\n", name, hex); } while (0)
    ds5_state_packet(5, b); P("state", DS5_STATE_LEN);
    ds5_mic_status_packet(7, 1, b); P("mic_on", DS5_MIC_STATUS_LEN);
    ds5_mic_status_packet(8, 0, b); P("mic_off", DS5_MIC_STATUS_LEN);
    ds5_mic_volume_packet(9, 64, b); P("mic_volume", DS5_STATE_LEN);
    uint8_t hap[128], spk[400];
    for (int i = 0; i < 128; i++) hap[i] = i * 3;
    for (int i = 0; i < 400; i++) spk[i] = i * 7;
    ds5_audio_packet(3, 4, hap, NULL, 0, 100, 0, b); P("audio_plain", DS5_AUDIO_LEN);
    ds5_audio_packet(15, 250, hap, spk, 0, 100, 1, b); P("audio_spk_mic", DS5_AUDIO_LEN);
    ds5_audio_packet(2, 258, hap, spk, 1, 48, 0, b); P("audio_headphone", DS5_AUDIO_LEN);
    uint8_t out[63];
    for (int i = 0; i < 63; i++) out[i] = i + 1;
    out[0] = 0x02;
    ds5_usb_to_bt_output(out, 63, 6, b); P("usb_to_bt", DS5_STATE_LEN);
}


int main(int argc, char** argv) {
    ds5_crc_init();
    if (argc > 1 && !strcmp(argv[1], "--dump-packets")) { dump_packets(); return 0; }
    return selftest();
}
