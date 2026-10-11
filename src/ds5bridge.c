// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// ds5bridge: makes a Bluetooth DualSense look like a USB DualSense (adaptive triggers, HD haptics and the speaker work in Proton/Wine games).
// A virtual USB device is served to the kernel's vhci_hcd (which speaks the USB/IP wire protocol over a socket; here an in-process socketpair) and bridged to the
// real pad's hidraw node. One thread, epoll + timerfd, plain C11. Deps: libopus, libm. Build: make. Tests: make test
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <opus/opus.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "descriptors.h"
#include "ds5proto.h"

#ifndef DS5_VERSION
#define DS5_VERSION "dev"   // the Makefile passes the real one (VERSION)
#endif

// ---------------------------------------------------------------- config
#define MAX_PADS 4   // real pads bridged at once (each gets its own virtual USB pad)
static struct {
    int attach, no_bt, no_haptics, no_speaker, mic, port, audio_buffer, mic_volume, qos_us, idle_off_min, nhidraw, feature_delay_ms;
    int output;                          // 0 auto (what the game and the headphone jack say), 1 speaker, 2 headset; "off" sets no_speaker
    int speaker_volume;                  // -1: whatever the game sets; 0-255 forces the pad's speaker volume (the pad takes about 61-100)
    double haptics_gain, mic_gain, grace_s, idle_off_sec;
    int fake_vhci;                       // tests: pretend to attach to vhci_hcd (no root needed)
    char log_file[700], hidraw[MAX_PADS + 1][300];  // hidraw: tests: use these nodes as the pads instead of searching /sys/class/hidraw (one more than the limit, to test it)
} cfg = {.attach = 1, .audio_buffer = 100, .mic_volume = 64, .qos_us = 5000, .idle_off_min = 15, .haptics_gain = 1.0, .mic_gain = 6.0, .grace_s = 3.0, .speaker_volume = -1};

#define ECONNRESET_ (-104)
#define EPIPE_ (-32)
// the debug TCP port exports the pads as busids "1-1" (slot 0, always there) ... "1-4"
#define VER 0x0111
#define OP_REQ_DEVLIST 0x8005
#define OP_REP_DEVLIST 0x0005
#define OP_REQ_IMPORT 0x8003
#define OP_REP_IMPORT 0x0003
#define CMD_SUBMIT 1u
#define CMD_UNLINK 2u
#define RET_SUBMIT 3u
#define RET_UNLINK 4u
// feature report sizes (incl. the id byte), used to answer unknown GET_REPORTs with zeros
static const struct { int id, size; } SIZES[] = {{0x05, 41}, {0x08, 48}, {0x09, 20}, {0x0A, 27}, {0x0B, 42}, {0x0C, 42}, {0x20, 64}, {0x21, 5},
                                                 {0x22, 64}, {0x80, 64}, {0x81, 64}, {0x82, 10}, {0x83, 64}, {0x84, 64}, {0x85, 3}, {0xA0, 2},
                                                 {0xE0, 64}, {0xF0, 64}, {0xF1, 64}, {0xF2, 16}, {0xF4, 64}, {0xF5, 4}};

// ---------------------------------------------------------------- time, log
static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static FILE* g_log = NULL;
static void logmsg(const char* lvl, const char* fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    char tb[32];
    strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &t);
    FILE* fs[2] = {g_log, stderr};  // stderr goes to the journal when run as a service
    for (int i = 0; i < 2; i++) {
        if (!fs[i]) continue;
        fprintf(fs[i], "%s,%03ld %s %s\n", tb, ts.tv_nsec / 1000000, lvl, msg);
        fflush(fs[i]);
    }
}
#define LOGI(...) logmsg("INFO", __VA_ARGS__)
#define LOGW(...) logmsg("WARNING", __VA_ARGS__)
#define LOGE(...) logmsg("ERROR", __VA_ARGS__)

// First occurrence of each kind, then every 1000th (a chatty client cannot flood the log). The table holds 256 kinds; once it is full a warning or an error of a
// new kind is still logged (one in 100), so a failure can never disappear because of earlier chatter.
static struct { char kind[112]; long n; } g_notes[256];
static int g_nnotes;
static long g_note_overflow;
static void note(const char* lvl, const char* kind, const char* fmt, ...) {
    int i;
    for (i = 0; i < g_nnotes; i++) if (!strcmp(g_notes[i].kind, kind)) break;
    long n = 0;
    int show;
    if (i < g_nnotes) { n = ++g_notes[i].n; show = n == 1 || n % 1000 == 0; }
    else if (g_nnotes < 256) { snprintf(g_notes[g_nnotes].kind, sizeof g_notes[0].kind, "%s", kind); n = g_notes[g_nnotes++].n = 1; show = 1; }
    else show = lvl[0] != 'I' && g_note_overflow++ % 100 == 0;
    if (!show) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n) logmsg(lvl, "%s (x%ld): %s", kind, n, msg);
    else logmsg(lvl, "%s: %s", kind, msg);
}

// ---------------------------------------------------------------- byte helpers
typedef struct { uint8_t* p; size_t n, cap; } Buf;
static void buf_add(Buf* b, const void* d, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = MAX(b->cap * 2, b->n + n + 256);
        b->p = realloc(b->p, b->cap);
        if (!b->p) { LOGE("out of memory"); exit(1); }
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void buf_zero(Buf* b, size_t n) { uint8_t z[64] = {0}; while (n) { size_t k = MIN(n, sizeof z); buf_add(b, z, k); n -= k; } }
static void put32(Buf* b, uint32_t v) { uint8_t x[4] = {v >> 24, v >> 16, v >> 8, v}; buf_add(b, x, 4); }
static void put16(Buf* b, uint16_t v) { uint8_t x[2] = {v >> 8, v}; buf_add(b, x, 2); }
static void buf_del_front(Buf* b, size_t n) { memmove(b->p, b->p + n, b->n - n); b->n -= n; }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd16le(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }


// ---------------------------------------------------------------- event loop (epoll + one timerfd)
typedef void (*FdFn)(int fd, uint32_t ev, void* ctx);
typedef void (*TimerFn)(void* arg);
static struct { FdFn fn; void* ctx; } fdh[4096];
static int ep_fd = -1, tfd = -1, running = 1;
typedef struct { double t; uint64_t id; TimerFn fn; void* arg; } Timer;
static Timer* timers;
static size_t ntimers, captimers;
static uint64_t next_id;
typedef struct { TimerFn fn; void* arg; } Deferred;
static Deferred deferred[256];
static int ndeferred;

static void timerfd_rearm(void) {
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    if (ntimers) {
        double t = timers[0].t;
        for (size_t i = 1; i < ntimers; i++) if (timers[i].t < t) t = timers[i].t;
        if (t < 1e-6) t = 1e-6;
        its.it_value.tv_sec = (time_t)t;
        its.it_value.tv_nsec = (long)((t - (double)its.it_value.tv_sec) * 1e9);
    }
    timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);
}
static uint64_t timer_at(double t, TimerFn fn, void* arg) {
    if (ntimers == captimers) { captimers = captimers ? captimers * 2 : 64; timers = realloc(timers, captimers * sizeof *timers); }
    timers[ntimers++] = (Timer){t, ++next_id, fn, arg};
    timerfd_rearm();
    return next_id;
}
static void timer_cancel(uint64_t id) {
    for (size_t i = 0; i < ntimers; i++)
        if (timers[i].id == id) { timers[i] = timers[--ntimers]; timerfd_rearm(); return; }
}
static void defer(TimerFn fn, void* arg) { if (ndeferred < 256) deferred[ndeferred++] = (Deferred){fn, arg}; }  // run after the current event
#define FD_OK(fd) ((fd) >= 0 && (fd) < 4096)   // the handler table's size: a larger descriptor is never registered (the loop would never dispatch it either)
static void fd_add(int fd, uint32_t ev, FdFn fn, void* ctx) {
    if (!FD_OK(fd)) return;
    fdh[fd].fn = fn; fdh[fd].ctx = ctx;
    struct epoll_event e = {.events = ev, .data.fd = fd};
    epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &e);
}
static void fd_mod(int fd, uint32_t ev) { if (!FD_OK(fd)) return; struct epoll_event e = {.events = ev, .data.fd = fd}; epoll_ctl(ep_fd, EPOLL_CTL_MOD, fd, &e); }
static void fd_del(int fd) { if (!FD_OK(fd)) return; epoll_ctl(ep_fd, EPOLL_CTL_DEL, fd, NULL); fdh[fd].fn = NULL; }
static void tfd_cb(int fd, uint32_t ev, void* ctx) { (void)ev; (void)ctx; uint64_t x; (void)!read(fd, &x, 8); }
static void loop_init(void) {
    ep_fd = epoll_create1(EPOLL_CLOEXEC);
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    fd_add(tfd, EPOLLIN, tfd_cb, NULL);
}
static void loop_run(void) {
    struct epoll_event evs[32];
    while (running) {
        int n = epoll_wait(ep_fd, evs, 32, -1);
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (FD_OK(fd) && fdh[fd].fn) fdh[fd].fn(fd, evs[i].events, fdh[fd].ctx);
        }
        for (;;) {  // run due timers, earliest first (callbacks may add or cancel timers)
            double now = mono();
            size_t best = (size_t)-1;
            for (size_t k = 0; k < ntimers; k++) if (timers[k].t <= now && (best == (size_t)-1 || timers[k].t < timers[best].t)) best = k;
            if (best == (size_t)-1) break;
            Timer t = timers[best];
            timers[best] = timers[--ntimers];
            t.fn(t.arg);
        }
        timerfd_rearm();
        int nd = ndeferred;
        Deferred d[256];
        memcpy(d, deferred, nd * sizeof *d);
        ndeferred = 0;
        for (int k = 0; k < nd; k++) d[k].fn(d[k].arg);
    }
}

// ---------------------------------------------------------------- Opus
typedef struct { OpusEncoder* e; } OpusEnc;  // 48 kHz stereo, 10 ms frames, CBR 160 kbps -> 200-byte packets (what the pad expects)
static void opus_enc_init(OpusEnc* o) {
    int err = 0;
    o->e = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    if (err) { LOGE("opus_encoder_create failed: %d", err); exit(1); }
    opus_encoder_ctl(o->e, OPUS_SET_BITRATE(160000));
    opus_encoder_ctl(o->e, OPUS_SET_VBR(0));
    opus_encoder_ctl(o->e, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(o->e, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
}
static void opus_enc_encode(OpusEnc* o, const float* pcm960, uint8_t* out200) {
    uint8_t buf[400];
    int n = opus_encode_float(o->e, pcm960, 480, buf, 400);
    if (n != 200) note("ERROR", "opus-size", "encoder returned %d B, the pad expects 200", n);
    memset(out200, 0, 200);
    if (n > 0) memcpy(out200, buf, MIN(n, 200));
}

#define MIC_CAP 48480
typedef struct {  // pad microphone: Opus frames (71 B, mono, 48 kHz, 10 ms) -> PCM for the host's iso IN endpoint, 30 ms jitter buffer
    OpusDecoder* d;
    int16_t buf[MIC_CAP];
    size_t n;
    int primed;
} Mic;
static Mic* mic_new(void) {
    Mic* m = calloc(1, sizeof *m);
    int err = 0;
    m->d = opus_decoder_create(48000, 1, &err);
    if (err) { LOGE("opus_decoder_create failed: %d", err); exit(1); }
    return m;
}
static void mic_free(Mic* m) { if (m) { opus_decoder_destroy(m->d); free(m); } }
static void mic_feed(Mic* m, const uint8_t* frame, int len) {
    float out[480];
    int n = opus_decode_float(m->d, frame, len, out, 480, 0);
    if (n <= 0) { note("WARNING", "mic-decode", "opus_decode_float returned %d for %d B", n, len); return; }
    for (int i = 0; i < n; i++) {
        double v = (double)out[i] * 32767 * cfg.mic_gain;
        m->buf[m->n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (m->n > 48000) { memmove(m->buf, m->buf + m->n - 24000, 24000 * sizeof(int16_t)); m->n = 24000; }  // at most 0.5 s: drop the oldest
}
static void mic_read(Mic* m, size_t n, int16_t* out) {  // n samples, silence while the buffer refills
    if (!m->primed && m->n >= 1440) m->primed = 1;
    if (!m->primed || m->n < n) { m->primed = 0; memset(out, 0, n * sizeof(int16_t)); return; }
    memcpy(out, m->buf, n * sizeof(int16_t));
    memmove(m->buf, m->buf + n, (m->n - n) * sizeof(int16_t));
    m->n -= n;
}

// ---------------------------------------------------------------- host audio -> Bluetooth
// 4 ch s16le 48 kHz: channels 3-4 become haptics (low-pass FIR, decimate by 16 -> 3 kHz int8), channels 1-2 the pad speaker/headphone stream
// (512 frames -> 480 by linear interpolation like the dongle, then Opus). Two 512-frame chunks make one 0x39 packet.
// Haptics are sent at 3 kHz, so 48 kHz audio is low-passed before it is decimated by 16: a 385-tap Kaiser-windowed sinc (beta 5.65, cutoff 1250 Hz): flat within
// 0.01 dB up to 1 kHz, -0.4 dB at 1.1 kHz, at least 59 dB down from 1.5 kHz (the new Nyquist), 192 samples (4 ms) of delay. A plain 16-sample average is
// 1.6 dB down at 1 kHz and barely attenuates what would alias.
#define HF_TAPS 385
static float hf_taps[HF_TAPS];
static int hf_ready;
static double bessel_i0(double x) { double sum = 1, term = 1; for (int k = 1; k < 60; k++) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; } return sum; }
static void hf_init(void) {
    if (hf_ready) return;
    const int m = HF_TAPS - 1;
    const double beta = 5.65, fc = 1250.0 / 48000.0;
    double sum = 0, tmp[HF_TAPS];
    for (int i = 0; i < HF_TAPS; i++) {
        double n = i - m / 2.0, sinc = n == 0 ? 2 * fc : sin(2 * M_PI * fc * n) / (M_PI * n);
        double r = 2.0 * i / m - 1, w = bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta);
        tmp[i] = sinc * w;
        sum += tmp[i];
    }
    for (int i = 0; i < HF_TAPS; i++) hf_taps[i] = (float)(tmp[i] / sum);  // unity gain at DC
    hf_ready = 1;
}

typedef void (*SendFn)(void* ctx, const uint8_t* pkt, size_t n);
typedef struct {
    SendFn send;
    int (*next_seq)(void*);
    int (*headphone)(void*);
    int (*mic_on)(void*);
    void (*route)(void*);    // called just before the first speaker packet after a silence: the pad may have reset its audio route (the GE-Proton patches saw that over USB)
    void* ctx;
    double gain;
    int paced, rate_hack, buflen, have_opus;
    OpusEnc opus;
    float acc[512 * 4];
    int nacc;
    float hist[2][HF_TAPS];  // last 385 samples of channels 3 and 4 (circular)
    int hpos, hphase;
    struct { uint8_t hap[64]; uint8_t spk[200]; } units[2];
    int nunits, counter, quiet_h, quiet_s, state_sent, spk_active;
    uint8_t outq[16][DS5_AUDIO_LEN];  // paced: packets wait here for the pacer timer
    int qhead, qn;
} BtAudio;

static int zero_i(void* c) { (void)c; return 0; }
static void bta_init(BtAudio* a, SendFn send, int (*next_seq)(void*), void* ctx, double gain, int speaker, int paced, int buflen) {
    memset(a, 0, sizeof *a);
    a->send = send; a->next_seq = next_seq; a->ctx = ctx; a->gain = gain; a->paced = paced; a->buflen = buflen;
    a->headphone = zero_i; a->mic_on = zero_i; a->rate_hack = 1; a->quiet_h = a->quiet_s = 99;
    hf_init();
    if (speaker) { a->have_opus = 1; opus_enc_init(&a->opus); }
}
static void bta_free(BtAudio* a) { if (a->have_opus) opus_encoder_destroy(a->opus.e); }
static void bta_chunk(BtAudio* a, const float* c) {
    uint8_t hap[64];
    int any = 0, g = 0;
    for (int f = 0; f < 512; f++) {  // every 16th frame yields one stereo output frame (3 kHz)
        for (int ch = 0; ch < 2; ch++) a->hist[ch][a->hpos] = c[f * 4 + 2 + ch];
        a->hpos = (a->hpos + 1) % HF_TAPS;
        if (++a->hphase < 16) continue;
        a->hphase = 0;
        for (int ch = 0; ch < 2; ch++) {
            double y = 0;
            for (int i = 0, p = a->hpos; i < HF_TAPS; i++, p = p + 1 == HF_TAPS ? 0 : p + 1) y += (double)hf_taps[i] * a->hist[ch][p];
            double v = nearbyint(y * 127 * a->gain);  // round half to even, like numpy.rint
            int8_t h = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
            hap[g * 2 + ch] = (uint8_t)h;
            any |= h != 0;
        }
        g++;
    }
    a->quiet_h = any ? 0 : a->quiet_h + 1;
    uint8_t spk[200] = {0};
    if (a->have_opus) {
        float s[960], peak = 0;
        for (int k = 0; k < 480; k++) {
            double x = k * (512.0 / 480.0);
            int i = MIN((int)x, 510);
            double f = x - i;
            for (int ch = 0; ch < 2; ch++) {
                double v = a->rate_hack ? c[i * 4 + ch] * (1 - f) + c[(i + 1) * 4 + ch] * f : c[k * 4 + ch];
                s[k * 2 + ch] = (float)v;
                peak = MAX(peak, fabsf((float)v));
            }
        }
        a->quiet_s = peak > 1e-4f ? 0 : a->quiet_s + 1;
        opus_enc_encode(&a->opus, s, spk);
    }
    memcpy(a->units[a->nunits].hap, hap, 64);
    memcpy(a->units[a->nunits].spk, spk, 200);
    if (++a->nunits < 2) return;
    a->nunits = 0;
    int want_h = a->quiet_h <= 30, want_s = a->have_opus && a->quiet_s <= 30;
    if (!want_s) a->spk_active = 0;
    if (!(want_h || want_s)) return;
    if (!a->state_sent) {  // once: ask the pad for haptics mode
        uint8_t st[DS5_STATE_LEN];
        ds5_state_packet(a->next_seq(a->ctx), st);
        a->send(a->ctx, st, sizeof st);
        a->state_sent = 1;
    }
    if (want_s && !a->spk_active && a->route) a->route(a->ctx);
    a->spk_active = want_s;
    uint8_t h128[128] = {0}, s400[400];
    if (want_h) { memcpy(h128, a->units[0].hap, 64); memcpy(h128 + 64, a->units[1].hap, 64); }
    if (want_s) { memcpy(s400, a->units[0].spk, 200); memcpy(s400 + 200, a->units[1].spk, 200); }
    uint8_t pkt[DS5_AUDIO_LEN];
    ds5_audio_packet(a->next_seq(a->ctx), a->counter, h128, want_s ? s400 : NULL, a->headphone(a->ctx), a->buflen, a->mic_on(a->ctx), pkt);
    a->counter = (a->counter + 2) & 0xFF;
    if (a->paced) {
        if (a->qn == 8) { a->qhead = (a->qhead + 1) & 15; a->qn--; note("WARNING", "audio-queue-overflow", "dropped the oldest packet (producer faster than 21.33 ms/packet)"); }
        memcpy(a->outq[(a->qhead + a->qn) & 15], pkt, DS5_AUDIO_LEN);
        a->qn++;
    } else a->send(a->ctx, pkt, sizeof pkt);
}
static void bta_feed(BtAudio* a, const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
        for (int c = 0; c < 4; c++) a->acc[a->nacc * 4 + c] = (int16_t)(p[i + 2 * c] | p[i + 2 * c + 1] << 8) / 32768.0f;
        if (++a->nacc == 512) { bta_chunk(a, a->acc); a->nacc = 0; }
    }
}

// ---------------------------------------------------------------- the emulated USB device
typedef struct Device {
    uint8_t dev[18];
    uint8_t* cfg; size_t cfglen;
    uint8_t* hid_desc; size_t hid_desc_len;
    char* str[4];
    uint8_t* feat[256]; size_t featlen[256];
    uint8_t input[64];
    int ep_type[16][2];  // [number][is_in] -> transfer type, -1 = absent
    struct { int wi, wv; uint8_t d[8]; size_t n; } ctl[32];
    int nctl, alt[16], cfg_value, hid_if, n;
    // pad side
    int bt_fd;
    uint8_t live[64];
    int has_live;
    double live_t;
    void (*on_output)(struct Device*, const uint8_t*, size_t);
    void (*on_audio)(struct Device*, const uint8_t*, size_t);
    void (*on_mic)(struct Device*, int);
    Mic* mic;
    int mic_active;
    void* user;          // the Pad this device belongs to
} Device;

static void log_output(const uint8_t* r0, size_t rn) {   // one kind for all: the values change all the time and would fill the note table
    uint8_t r[48] = {0};
    if (rn) memcpy(r, r0, MIN(rn, 48));   // a zero-length OUT URB brings a NULL pointer
    char hex[100];
    ds5_hex(r, 48, hex);
    note("INFO", "game-output", "flag0=0x%02x flag1=0x%02x rumbleR/L=%d/%d rightFFB=0x%02x leftFFB=0x%02x %s", r[1], r[2], r[3], r[4], r[11], r[22], hex);
}
static void dev_default_output(Device* d, const uint8_t* r, size_t n) { (void)d; log_output(r, n); }
static void dev_default_audio(Device* d, const uint8_t* p, size_t n) { (void)d; (void)p; (void)n; }
static void dev_default_mic(Device* d, int on) { (void)d; (void)on; }

// Zeroed 0x05 / 0x09 / 0x20 reports (id byte only): what the virtual pad serves until a pad's own are read. Also used when a slot passes to a different pad, so a failed read
// can never leave the new virtual pad with the previous pad's address.
static void dev_placeholder_features(Device* d) {
    static const struct { int id, len; } placeholder[] = {{0x05, 41}, {0x09, 20}, {0x20, 64}};
    for (int i = 0; i < 3; i++) {
        free(d->feat[placeholder[i].id]);
        d->feat[placeholder[i].id] = calloc(1, placeholder[i].len);
        d->featlen[placeholder[i].id] = placeholder[i].len;
        d->feat[placeholder[i].id][0] = placeholder[i].id;
    }
}
// The descriptors are the same for every DualSense (see descriptors.h). Per-unit feature reports are placeholders until dev_set_features() gets the
// real pad's: 0x05 calibration, 0x09 pairing info (MAC), 0x20 firmware info.
static void dev_init(Device* d) {
    memset(d, 0, sizeof *d);
    memcpy(d->dev, DS5_DEVICE_DESC, sizeof DS5_DEVICE_DESC);
    d->cfglen = sizeof DS5_CONFIG_DESC;
    d->cfg = malloc(d->cfglen);
    memcpy(d->cfg, DS5_CONFIG_DESC, d->cfglen);
    d->str[1] = strdup(DS5_STRING_MANUFACTURER); d->str[2] = strdup(DS5_STRING_PRODUCT); d->str[3] = strdup("");
    d->hid_desc_len = sizeof DS5_HID_REPORT_DESC;
    d->hid_desc = malloc(d->hid_desc_len);
    memcpy(d->hid_desc, DS5_HID_REPORT_DESC, d->hid_desc_len);
    dev_placeholder_features(d);
    d->input[0] = 1;                       // neutral input report: sticks centred, d-pad released
    memset(d->input + 1, 0x80, 4);
    d->input[8] = 0x08;
    memset(d->ep_type, -1, sizeof d->ep_type);
    d->cfg_value = 1; d->hid_if = 3; d->bt_fd = -1;
    d->on_output = dev_default_output; d->on_audio = dev_default_audio; d->on_mic = dev_default_mic;
    int found = 0;
    for (size_t i = 0; i + 1 < d->cfglen; i += d->cfg[i] ? d->cfg[i] : 1) {
        if (d->cfg[i + 1] == 5) { int a = d->cfg[i + 2]; d->ep_type[a & 15][!!(a & 0x80)] = d->cfg[i + 3] & 3; }
        if (!found && d->cfg[i + 1] == 4 && d->cfg[i + 5] == 3) { d->hid_if = d->cfg[i + 2]; found = 1; }
    }
}
// Feature reports read from the real pad (NULL = keep what is there). The pairing report's MAC gets its "locally administered" bit flipped (bit 1 of the first octet,
// the last byte of the little-endian address in the report): hid-playstation refuses a second device with an address it already has, and the real pads are bound to
// theirs over Bluetooth. Flipping the lowest bit instead would give pad A's virtual pad the real address of a pad B that differs from A only there. A real Sony address
// never has this bit set. Only a report read just now is flipped, so a failed read can never flip an old one back to the pad's own address.
static void dev_set_features(Device* d, const uint8_t* f05, int n05, const uint8_t* f09, int n09, const uint8_t* f20, int n20) {
    const struct { int id; const uint8_t* p; int n; } in[] = {{0x05, f05, n05}, {0x09, f09, n09}, {0x20, f20, n20}};
    for (int i = 0; i < 3; i++) {
        if (!in[i].p || in[i].n <= 0) continue;
        free(d->feat[in[i].id]);
        d->feat[in[i].id] = malloc(in[i].n);
        memcpy(d->feat[in[i].id], in[i].p, in[i].n);
        d->featlen[in[i].id] = in[i].n;
    }
    if (f09 && n09 > 6) d->feat[0x09][6] ^= 0x02;
}
static void dev_wire_device(const Device* d, const char* busid, int speed, Buf* b) {
    char path[256] = {0}, bid[32] = {0};
    snprintf(path, sizeof path, "/sys/devices/platform/ds5bridge/usb1/%s", busid);
    snprintf(bid, sizeof bid, "%s", busid);
    buf_add(b, path, 256); buf_add(b, bid, 32);
    put32(b, 1); put32(b, 2); put32(b, speed);
    put16(b, rd16le(d->dev + 8)); put16(b, rd16le(d->dev + 10)); put16(b, rd16le(d->dev + 12));
    uint8_t t[6] = {d->dev[4], d->dev[5], d->dev[6], d->cfg[5], d->dev[17], d->cfg[4]};
    buf_add(b, t, 6);
}
static void dev_wire_interfaces(const Device* d, Buf* b) {
    for (size_t i = 0; i + 8 <= d->cfglen; i += d->cfg[i] ? d->cfg[i] : 1)
        if (d->cfg[i + 1] == 4 && d->cfg[i + 3] == 0) { uint8_t t[4] = {d->cfg[i + 5], d->cfg[i + 6], d->cfg[i + 7], 0}; buf_add(b, t, 4); }
}
static void dev_next_input(Device* d, uint8_t* out) {
    if (d->has_live && mono() - d->live_t < 0.5) { memcpy(out, d->live, 64); return; }
    d->n++;
    d->input[7] = d->n & 0xFF;
    uint32_t ts = (uint32_t)d->n * 4000u * 4u;
    for (int i = 0; i < 4; i++) d->input[28 + i] = ts >> (8 * i);
    memcpy(out, d->input, 64);
}
static size_t dev_hid_get(Device* d, int rtype, int rid, uint8_t* out) {  // out: 64+ bytes
    if (rtype == 1) {
        if (rid == 1) { dev_next_input(d, out); return 64; }
        memset(out, 0, 64);
        out[0] = rid;
        return 64;
    }
    if (d->feat[rid]) { memcpy(out, d->feat[rid], d->featlen[rid]); return d->featlen[rid]; }
    char kind[64];
    snprintf(kind, sizeof kind, "hid-get-unknown-%#x", rid);
    note("WARNING", kind, "type %d: answered with zeros", rtype);
    size_t n = 64;
    for (size_t i = 0; i < sizeof SIZES / sizeof *SIZES; i++) if (SIZES[i].id == rid) n = SIZES[i].size;
    memset(out, 0, n);
    out[0] = rid;
    return n;
}
static size_t dev_string_desc(const Device* d, int idx, uint8_t* out) {
    if (idx == 0) { out[0] = 4; out[1] = 3; out[2] = 9; out[3] = 4; return 4; }
    const char* s = idx < 4 && d->str[idx] ? d->str[idx] : "";
    size_t n = strlen(s);
    out[0] = 2 + 2 * n; out[1] = 3;
    for (size_t i = 0; i < n; i++) { out[2 + 2 * i] = s[i]; out[3 + 2 * i] = 0; }
    return 2 + 2 * n;
}
// -> status (0 ok, EPIPE_ = stall); reply bytes in out (at least 1024), length in *rn
static int dev_control(Device* d, const uint8_t* s, const uint8_t* data, size_t dn, uint8_t* out, size_t* rn) {
    int bm = s[0], br = s[1], wv = s[2] | s[3] << 8, wi = s[4] | s[5] << 8, wl = s[6] | s[7] << 8;
    int typ = (bm >> 5) & 3, rec = bm & 31;
    uint8_t tmp[1024];
    size_t n;
    *rn = 0;
#define REPLY(src, len) do { size_t _n = (len); if (_n > (size_t)wl) _n = wl; if (_n) memcpy(out, (src), _n); *rn = _n; return 0; } while (0)
    if (typ == 0) {
        if (br == 6 && (bm & 0x80)) {
            int dt = wv >> 8, idx = wv & 255;
            if (dt == 1) REPLY(d->dev, 18);
            if (dt == 2 && idx == 0) REPLY(d->cfg, d->cfglen);
            if (dt == 3) { n = dev_string_desc(d, idx, tmp); REPLY(tmp, n); }
            if (dt == 0x22) REPLY(d->hid_desc, d->hid_desc_len);
            return EPIPE_;  // device_qualifier / other_speed / bos
        }
        if (br == 0) { tmp[0] = tmp[1] = 0; REPLY(tmp, 2); }
        if (br == 1 || br == 3 || br == 5) return 0;
        if (br == 8) { tmp[0] = d->cfg_value; REPLY(tmp, 1); }
        if (br == 9) { d->cfg_value = wv & 255; return 0; }
        if (br == 10) { tmp[0] = d->alt[wi & 15]; REPLY(tmp, 1); }
        if (br == 11) {
            if (d->alt[wi & 15] != wv) LOGI("interface %d -> alt %d", wi, wv);
            d->alt[wi & 15] = wv;
            if (wi == 2 && d->mic_active != (wv != 0)) { d->mic_active = wv != 0; d->on_mic(d, d->mic_active); }  // interface 2 = the microphone stream
            return 0;
        }
    } else if (typ == 1 && rec == 1 && (wi & 255) == d->hid_if) {  // HID class
        int rt = wv >> 8, rid = wv & 255;
        if (br == 1) { n = dev_hid_get(d, rt, rid, tmp); REPLY(tmp, n); }
        if (br == 9) {
            if (rt == 2) d->on_output(d, data, dn);
            else { char kind[64], hex[40]; snprintf(kind, sizeof kind, "hid-set-feature-dropped-%#x", rid); ds5_hex(data, MIN(dn, 16), hex); note("INFO", kind, "%s", hex); }  // never forwarded
            return 0;
        }
        if (br == 2) { tmp[0] = 0; REPLY(tmp, 1); }
        if (br == 3) { tmp[0] = 1; REPLY(tmp, 1); }
        if (br == 0x0A || br == 0x0B) return 0;
    } else if (typ == 1 && (rec == 1 || rec == 2)) {  // audio class (UAC1): controls and sampling rate
        int cs = wv >> 8;
        if (br >= 1 && br <= 4) {  // SET_CUR/MIN/MAX/RES: remember CUR only
            if (br == 1) {
                int k;
                for (k = 0; k < d->nctl; k++) if (d->ctl[k].wi == wi && d->ctl[k].wv == wv) break;
                if (k == d->nctl && d->nctl < 32) d->nctl++;
                if (k < 32) { d->ctl[k].wi = wi; d->ctl[k].wv = wv; d->ctl[k].n = MIN(dn, 8); memcpy(d->ctl[k].d, data, d->ctl[k].n); }
            }
            return 0;
        }
        if (rec == 2 && cs == 1) { tmp[0] = 0x80; tmp[1] = 0xBB; tmp[2] = 0; REPLY(tmp, 3); }  // endpoint sampling frequency 48000
        if (br == 0x81)
            for (int k = 0; k < d->nctl; k++) if (d->ctl[k].wi == wi && d->ctl[k].wv == wv) REPLY(d->ctl[k].d, d->ctl[k].n);
        if (br >= 0x81 && br <= 0x84) {
            int16_t v = br == 0x82 ? -12800 : br == 0x84 ? 256 : 0;
            if (cs == 2) { tmp[0] = v & 0xFF; tmp[1] = (v >> 8) & 0xFF; REPLY(tmp, 2); }
            memset(tmp, 0, sizeof tmp);
            REPLY(tmp, MIN(wl, 1024));
        }
    }
    char kind[64];
    snprintf(kind, sizeof kind, "control-unhandled-%02x-%02x", bm, br);
    note("WARNING", kind, "wValue=%d wIndex=%d wLength=%d", wv, wi, wl);
    return EPIPE_;
#undef REPLY
}

// ---------------------------------------------------------------- one real pad and its virtual twin
typedef struct Conn Conn;
typedef struct { uint8_t rep[63]; uint8_t f0, f1, f2; } Shadow;   // what the game set that the pad forgets on a drop (see shadow_merge)
typedef struct Pad {
    int claimed;                       // a real pad has used this slot
    char key[128], tag[24], node[300]; // key: what identifies the pad (its Bluetooth address, or the hidraw path in tests); tag: short name for the log
    Device dev;
    Conn* vconn;                       // server end of the socketpair handed to vhci_hcd
    int vport, fake_fd, want_vpad;     // vhci_hcd port (-1 = none), tests: stand-in for the kernel's end, whether the virtual pad should exist
    uint64_t vretry;                   // the one pending attach-retry timer
    struct FeatJob* job;               // the thread reading the pad's feature reports while a new connection starts
    BtAudio* audio;
    int seq, started, sent, under, depth, have_ref, rate_n, batt_seen;
    uint64_t pacer, grace_timer, idle_timer, replay_timer;
    double pacer_next, pacer_log, rate_t, rate_last;   // pacer clock and statistics; input-report rate window
    double last_activity;              // last time a person (or the game's haptics/audio) did something; drives --idle-off
    uint8_t ref[4], batt_last;
    Shadow shadow;
    struct { int fd, handle; uint64_t timer; int latency; } qos;
} Pad;
static Pad g_pads[MAX_PADS];

// ---------------------------------------------------------------- the kernel's connection (USB/IP wire protocol: CMD_SUBMIT / RET_SUBMIT / UNLINK)
typedef struct Comp {  // a delayed RET_SUBMIT (isochronous completion)
    struct Conn* c;
    uint32_t seq;
    int npk, sf;
    Buf data, iso;
    size_t actual;
} Comp;
struct Conn {
    int fd, imported, close_after, closed;
    Buf rbuf, wbuf;
    struct { uint32_t seq; uint64_t timer; Comp* comp; } pending[256];  // timer 0 = parked interrupt-IN
    int npending;
    uint32_t int_in[64];
    int nint_in;
    double busy[16][2];
    uint64_t audio_bytes, feeder;
    long peak[4];
    double stat_t, feeder_next;
    void (*on_end)(Pad*);
    Pad* pad;
};
static Conn* conns[16];

static void conn_free(void* p) { Conn* c = p; free(c->rbuf.p); free(c->wbuf.p); free(c); }
static void conn_close(Conn* c) {
    if (c->closed) return;
    c->closed = 1;
    for (int i = 0; i < c->npending; i++) {
        if (c->pending[i].timer) timer_cancel(c->pending[i].timer);
        if (c->pending[i].comp) { free(c->pending[i].comp->data.p); free(c->pending[i].comp->iso.p); free(c->pending[i].comp); }
    }
    c->npending = 0;
    if (c->feeder) timer_cancel(c->feeder);
    fd_del(c->fd);
    close(c->fd);
    LOGI("client gone");
    for (int i = 0; i < 16; i++) if (conns[i] == c) conns[i] = NULL;
    void (*on_end)(Pad*) = c->on_end;
    Pad* pad = c->pad;
    defer(conn_free, c);
    if (on_end) on_end(pad);
}
static void conn_send(Conn* c, const uint8_t* b, size_t n) {
    if (c->closed) return;
    if (c->wbuf.n == 0) {
        ssize_t w = send(c->fd, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { conn_close(c); return; }
        if (w == (ssize_t)n) return;
        buf_add(&c->wbuf, b + (w > 0 ? w : 0), n - (w > 0 ? w : 0));
    } else buf_add(&c->wbuf, b, n);
    fd_mod(c->fd, EPOLLIN | EPOLLOUT);
}
static void conn_flush(Conn* c) {
    if (c->wbuf.n) {
        ssize_t w = send(c->fd, c->wbuf.p, c->wbuf.n, MSG_NOSIGNAL);
        if (w < 0 && errno != EAGAIN) { conn_close(c); return; }
        if (w > 0) buf_del_front(&c->wbuf, w);
    }
    if (c->wbuf.n == 0) { fd_mod(c->fd, EPOLLIN); if (c->close_after) conn_close(c); }
}
static void ret_submit(Conn* c, uint32_t seq, int status, int actual, const uint8_t* data, size_t dn, int npk, int sf, const uint8_t* iso, size_t isn) {
    Buf b = {0};
    put32(&b, RET_SUBMIT); put32(&b, seq); put32(&b, 0); put32(&b, 0); put32(&b, 0);
    put32(&b, status); put32(&b, actual); put32(&b, sf); put32(&b, npk); put32(&b, 0);
    buf_zero(&b, 8);
    buf_add(&b, data, dn);
    buf_add(&b, iso, isn);
    conn_send(c, b.p, b.n);
    free(b.p);
}
static void pending_remove(Conn* c, uint32_t seq) {
    for (int i = 0; i < c->npending; i++) if (c->pending[i].seq == seq) { c->pending[i] = c->pending[--c->npending]; return; }
}
static int pending_add(Conn* c, uint32_t seq, uint64_t timer, Comp* comp) {  // 0 = table full
    if (c->npending == 256) return 0;
    c->pending[c->npending].seq = seq; c->pending[c->npending].timer = timer; c->pending[c->npending].comp = comp;
    c->npending++;
    return 1;
}
static void comp_cb(void* arg) {
    Comp* k = arg;
    Conn* c = k->c;
    pending_remove(c, k->seq);
    ret_submit(c, k->seq, 0, k->actual, k->data.p, k->data.n, k->npk, k->sf, k->iso.p, k->iso.n);
    free(k->data.p); free(k->iso.p); free(k);
}
// complete a URB at `deadline`: payload goes after the header (IN), `actual` is the length reported
static void complete_later(Conn* c, uint32_t seq, double deadline, size_t actual, int npk, int sf, const Buf* data, const Buf* iso) {
    Comp* k = calloc(1, sizeof *k);
    k->c = c; k->seq = seq; k->npk = npk; k->sf = sf; k->actual = actual;
    if (data) buf_add(&k->data, data->p, data->n);
    buf_add(&k->iso, iso->p, iso->n);
    uint64_t id = timer_at(deadline, comp_cb, k);
    if (!pending_add(c, seq, id, k)) {  // a client that submits far more than the kernel ever does: drop it, and the timer with it
        timer_cancel(id);
        free(k->data.p); free(k->iso.p); free(k);
        note("WARNING", "too-many-urbs", "more than 256 pending URBs: closing the connection");
        conn_close(c);
    }
}
static void feeder_cb(void* arg) {  // ~250 Hz input reports, like the real pad
    Conn* c = arg;
    if (c->closed) return;
    if (c->nint_in) {
        uint32_t seq = c->int_in[0];
        memmove(c->int_in, c->int_in + 1, --c->nint_in * sizeof(uint32_t));
        pending_remove(c, seq);
        uint8_t rep[64];
        dev_next_input(&c->pad->dev, rep);
        ret_submit(c, seq, 0, 64, rep, 64, -1, 0, NULL, 0);
    }
    if (c->closed) return;
    c->feeder_next = MAX(c->feeder_next + 0.004, mono());
    c->feeder = timer_at(c->feeder_next, feeder_cb, c);
}
static void conn_start_feeder(Conn* c) { c->feeder_next = mono() + 0.004; c->feeder = timer_at(c->feeder_next, feeder_cb, c); }

static void conn_submit(Conn* c, uint32_t seq, uint32_t direction, uint32_t ep, int tlen, int sf, int npk, int interval, const uint8_t* setup,
                        const uint8_t* data, size_t dn, const uint32_t* descs) {
    double now = mono();
    Device* d = &c->pad->dev;
    if (ep == 0) {
        uint8_t reply[1024];
        size_t rn;
        int status = dev_control(d, setup, data, dn, reply, &rn);
        if (direction == 0) ret_submit(c, seq, status, status == 0 ? tlen : 0, NULL, 0, -1, 0, NULL, 0);
        else ret_submit(c, seq, status, rn, reply, rn, -1, 0, NULL, 0);
        return;
    }
    int kind = ep < 16 ? d->ep_type[ep][direction ? 1 : 0] : -1;
    if (npk > 0) {  // isochronous: complete after the time the packets would take on the wire (high speed)
        double period = (interval ? interval : 1) * 0.000125;
        double* busy = &c->busy[ep & 15][direction ? 1 : 0];
        double start = MAX(now, *busy);
        double end = *busy = start + npk * period;
        Buf iso = {0};
        if (direction == 0) {
            c->audio_bytes += dn;
            d->on_audio(d, data, dn);
            size_t frames = dn / 8;
            for (size_t i = 0; i < frames; i++)
                for (int ch = 0; ch < 4; ch++) {
                    int v = abs((int16_t)(data[i * 8 + 2 * ch] | data[i * 8 + 2 * ch + 1] << 8));
                    if (v > c->peak[ch]) c->peak[ch] = v;
                    if (v > 200) c->pad->last_activity = mono();
                }
            if (mono() - c->stat_t > 2) {
                LOGI("%saudio from host: %lu B total, peak last 2 s  ch1-2 (speaker) %ld/%ld   ch3-4 (haptics) %ld/%ld%s", c->pad->tag, (unsigned long)c->audio_bytes,
                     c->peak[0], c->peak[1], c->peak[2], c->peak[3], (c->peak[0] | c->peak[1] | c->peak[2] | c->peak[3]) ? "" : "   [all zeros]");
                memset(c->peak, 0, sizeof c->peak);
                c->stat_t = mono();
            }
            for (int k = 0; k < npk; k++) { put32(&iso, descs[4 * k]); put32(&iso, descs[4 * k + 1]); put32(&iso, descs[4 * k + 1]); put32(&iso, 0); }
            complete_later(c, seq, end, dn, npk, sf, NULL, &iso);
        } else {  // microphone: 2 ch, 16 bit, 48 kHz (silence unless the pad mic is on)
            uint32_t per = (uint32_t)lround(192 * period * 1000), total = 0;
            for (int k = 0; k < npk; k++) {
                uint32_t len = descs[4 * k + 1], cc = MIN(len, per);
                put32(&iso, descs[4 * k]); put32(&iso, len); put32(&iso, cc); put32(&iso, 0);
                total += cc;
            }
            size_t frames = total / 4;
            int16_t* pcm = calloc(frames ? frames : 1, sizeof(int16_t));
            if (d->mic && d->mic_active) mic_read(d->mic, frames, pcm);
            Buf payload = {0};
            for (size_t i = 0; i < frames; i++) { uint8_t t[4] = {pcm[i] & 0xFF, (pcm[i] >> 8) & 0xFF, pcm[i] & 0xFF, (pcm[i] >> 8) & 0xFF}; buf_add(&payload, t, 4); }  // L = R
            free(pcm);
            complete_later(c, seq, end, payload.n, npk, sf, &payload, &iso);
            free(payload.p);
        }
        free(iso.p);
    } else if (direction == 0) {  // interrupt/bulk OUT: an output report
        d->on_output(d, data, dn);
        ret_submit(c, seq, 0, dn, NULL, 0, -1, 0, NULL, 0);
    } else if (kind == 3) {  // interrupt IN: park until the feeder has a report
        if (c->nint_in >= 64 || !pending_add(c, seq, 0, NULL)) { note("WARNING", "too-many-urbs", "too many parked interrupt URBs: closing the connection"); conn_close(c); return; }
        c->int_in[c->nint_in++] = seq;
    } else {
        char k[64];
        snprintf(k, sizeof k, "urb-unhandled-ep%u-%s", ep, direction ? "in" : "out");
        note("WARNING", k, "type %d", kind);
        ret_submit(c, seq, EPIPE_, 0, NULL, 0, -1, 0, NULL, 0);
    }
    (void)tlen;
}

static void conn_process(Conn* c) {
    if (!c->imported) {
        if (c->rbuf.n < 8) return;
        uint16_t cmd = c->rbuf.p[2] << 8 | c->rbuf.p[3];
        Buf b = {0};
        if (cmd == OP_REQ_DEVLIST) {
            int cnt = 0;
            for (int i = 0; i < MAX_PADS; i++) cnt += i == 0 || g_pads[i].claimed;
            put16(&b, VER); put16(&b, OP_REP_DEVLIST); put32(&b, 0); put32(&b, cnt);
            for (int i = 0; i < MAX_PADS; i++) {   // slot i is "1-<i+1>"; slot 0 is always there (the debug pad), the others once a real pad has used them
                if (i && !g_pads[i].claimed) continue;
                char id[16];
                snprintf(id, sizeof id, "1-%d", i + 1);
                dev_wire_device(&g_pads[i].dev, id, 3, &b);
                dev_wire_interfaces(&g_pads[i].dev, &b);
            }
            c->rbuf.n = 0;
            c->close_after = 1;
            conn_send(c, b.p, b.n);
            free(b.p);
            if (c->wbuf.n == 0) conn_close(c);
            return;
        }
        if (cmd == OP_REQ_IMPORT) {
            if (c->rbuf.n < 40) return;
            char busid[33] = {0};
            memcpy(busid, c->rbuf.p + 8, 32);
            buf_del_front(&c->rbuf, 40);
            put16(&b, VER); put16(&b, OP_REP_IMPORT);
            int slot = -1;
            for (int i = 0; i < MAX_PADS; i++) {
                char id[16];
                snprintf(id, sizeof id, "1-%d", i + 1);
                if (!strcmp(busid, id) && (i == 0 || g_pads[i].claimed)) slot = i;
            }
            if (slot < 0) {
                put32(&b, 1);
                note("WARNING", "import-wrong-busid", "%s", busid);
                c->close_after = 1;
                conn_send(c, b.p, b.n);
                free(b.p);
                if (c->wbuf.n == 0) conn_close(c);
                return;
            }
            put32(&b, 0);
            c->pad = &g_pads[slot];
            dev_wire_device(&c->pad->dev, busid, 3, &b);
            conn_send(c, b.p, b.n);
            free(b.p);
            c->imported = 1;
            LOGI("imported");
            conn_start_feeder(c);
        } else {
            char k[32];
            snprintf(k, sizeof k, "op-unknown-%#x", cmd);
            note("WARNING", k, "");
            conn_close(c);
            return;
        }
    }
    while (!c->closed && c->rbuf.n >= 48) {
        const uint8_t* h = c->rbuf.p;
        uint32_t cmd = rd32(h), seq = rd32(h + 4), direction = rd32(h + 12), ep = rd32(h + 16);
        if (cmd == CMD_SUBMIT) {
            int32_t tlen = (int32_t)rd32(h + 24), sf = (int32_t)rd32(h + 28), npk = (int32_t)rd32(h + 32), interval = (int32_t)rd32(h + 36);
            size_t dn = direction == 0 && tlen > 0 ? (size_t)tlen : 0, need = 48 + dn + (npk > 0 ? 16 * (size_t)npk : 0);
            if (need > (1u << 20)) { note("WARNING", "urb-too-big", "a URB of %zu bytes: closing the connection", need); conn_close(c); return; }
            if (c->rbuf.n < need) return;
            uint8_t setup[8];
            memcpy(setup, h + 40, 8);
            uint32_t* descs = NULL;
            if (npk > 0) {
                descs = malloc(npk * 4 * sizeof(uint32_t));
                for (int k = 0; k < npk; k++) for (int j = 0; j < 4; j++) descs[4 * k + j] = rd32(h + 48 + dn + 16 * k + 4 * j);
            }
            uint8_t* data = NULL;
            if (dn) { data = malloc(dn); memcpy(data, h + 48, dn); }
            buf_del_front(&c->rbuf, need);
            conn_submit(c, seq, direction, ep, tlen, sf, npk, interval, setup, data, dn, descs);
            free(data); free(descs);
        } else if (cmd == CMD_UNLINK) {
            uint32_t unlink = rd32(h + 20);
            buf_del_front(&c->rbuf, 48);
            int cancelled = 0;
            for (int i = 0; i < c->npending; i++)
                if (c->pending[i].seq == unlink) {
                    if (c->pending[i].timer) timer_cancel(c->pending[i].timer);
                    if (c->pending[i].comp) { free(c->pending[i].comp->data.p); free(c->pending[i].comp->iso.p); free(c->pending[i].comp); }
                    c->pending[i] = c->pending[--c->npending];
                    cancelled = 1;
                    break;
                }
            for (int i = 0; i < c->nint_in; i++) if (c->int_in[i] == unlink) { memmove(c->int_in + i, c->int_in + i + 1, (--c->nint_in - i) * sizeof(uint32_t)); break; }
            Buf b = {0};
            put32(&b, RET_UNLINK); put32(&b, seq); put32(&b, 0); put32(&b, 0); put32(&b, 0);
            put32(&b, cancelled ? (uint32_t)ECONNRESET_ : 0);
            buf_zero(&b, 24);
            conn_send(c, b.p, b.n);
            free(b.p);
        } else {
            char k[32], hex[48];
            snprintf(k, sizeof k, "cmd-unknown-%u", cmd);
            ds5_hex(h, 20, hex);
            note("WARNING", k, "%s", hex);
            buf_del_front(&c->rbuf, 48);
        }
    }
}
static void conn_event(int fd, uint32_t ev, void* ctx) {
    Conn* c = ctx;
    (void)fd;
    if (ev & EPOLLOUT) conn_flush(c);
    if (c->closed) return;
    if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
        uint8_t buf[65536];
        for (;;) {
            ssize_t n = recv(c->fd, buf, sizeof buf, 0);
            if (n > 0) { buf_add(&c->rbuf, buf, n); if (c->rbuf.n > (2u << 20)) { note("WARNING", "urb-too-big", "more than 2 MB buffered: closing the connection"); conn_close(c); return; } continue; }
            if (n == 0) { conn_close(c); return; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            conn_close(c);
            return;
        }
        conn_process(c);
    }
}
static Conn* conn_new(int fd, int imported, void (*on_end)(Pad*), Pad* pad) {
    Conn* c = calloc(1, sizeof *c);
    c->fd = fd; c->imported = imported; c->on_end = on_end; c->pad = pad; c->stat_t = mono();
    for (int i = 0; i < 16; i++) if (!conns[i]) { conns[i] = c; break; }
    fd_add(fd, EPOLLIN, conn_event, c);
    return c;
}

// ---------------------------------------------------------------- the pads
// One Pad per real Bluetooth DualSense (up to MAX_PADS): its emulated USB device, its virtual pad on vhci_hcd and everything the bridge keeps for it. All of them are
// initialised at start so the debug listener (--port) and the tests have a device to serve before any pad shows up; the first real pad takes slot 0.
static void br_on_output(Device* d, const uint8_t* r, size_t n);
static void pad_init(Pad* p) {
    memset(p, 0, sizeof *p);
    dev_init(&p->dev);
    p->dev.user = p;
    p->dev.on_output = br_on_output;
    p->vport = -1; p->fake_fd = -1; p->qos.fd = -1;
}

// ---------------------------------------------------------------- attaching the virtual pads to vhci_hcd
// Hand a connected SOCK_STREAM to vhci_hcd with "port sockfd devid speed" (sysfs `attach`). Here the other end of the
// socket is a socketpair inside this process, so no TCP and no handshake are needed. Needs root (the sysfs file is 0200).
// Every pad has its own vhci port: attach tries the ports in turn and takes the first free one (EBUSY = in use).
#define VHCI_DEVID ((1u << 16) | 2u)
#define VHCI_SPEED_HIGH 3u
#define VHCI "/sys/devices/platform/vhci_hcd.0"

static int vhci_attach(Pad* p, int kernel_fd) {
    if (cfg.fake_vhci) { p->fake_fd = dup(kernel_fd); p->vport = (int)(p - g_pads); LOGI("%sattached to vhci_hcd port %d (fake)", p->tag, p->vport); return 1; }
    char err[128] = "no free high-speed port";
    for (int port = 0; port < 8; port++) {
        int fd = open(VHCI "/attach", O_WRONLY | O_CLOEXEC);
        if (fd < 0) { note("ERROR", "vhci-open", "cannot open " VHCI "/attach: %s (is vhci_hcd loaded? are you root?)", strerror(errno)); return 0; }
        char buf[64];
        int n = snprintf(buf, sizeof buf, "%d %d %u %u", port, kernel_fd, VHCI_DEVID, VHCI_SPEED_HIGH);
        ssize_t w = write(fd, buf, n);
        int e = errno;
        close(fd);
        if (w == n) { p->vport = port; LOGI("%sattached to vhci_hcd port %d", p->tag, port); return 1; }
        snprintf(err, sizeof err, "%s", strerror(e));
        if (e != EBUSY) break;  // EBUSY = port in use, try the next one
    }
    note("ERROR", "vhci-attach", "vhci_hcd attach failed: %s", err);
    return 0;
}
static void vhci_detach(Pad* p) {
    if (p->vport < 0) return;
    if (cfg.fake_vhci) { close(p->fake_fd); p->fake_fd = -1; LOGI("%sdetached vhci_hcd port %d (fake)", p->tag, p->vport); p->vport = -1; return; }
    int fd = open(VHCI "/detach", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { char s[16]; int n = snprintf(s, sizeof s, "%d", p->vport); (void)!write(fd, s, n); close(fd); LOGI("%sdetached vhci_hcd port %d", p->tag, p->vport); }
    p->vport = -1;
}
// The virtual pad exists only while the real one is connected (plus a short grace period): vpad_set(p, 1) attaches it, vpad_set(p, 0) detaches it.
static void vpad_attach(void* arg);
static void vpad_retry_in(Pad* p, double s) {   // one pending retry timer per pad, so repeated failures never pile up chains of timers
    if (p->vretry) timer_cancel(p->vretry);
    p->vretry = timer_at(mono() + s, vpad_attach, p);
}
static void on_vpad_end(Pad* p) {  // the kernel dropped its end: a detach we asked for, or something else
    p->vconn = NULL;
    p->vport = -1;
    if (p->dev.mic_active) { p->dev.mic_active = 0; if (p->dev.bt_fd >= 0) p->dev.on_mic(&p->dev, 0); }   // the host's mic stream died with the device: a new session must not inherit it
    if (p->want_vpad) { LOGW("%svhci_hcd dropped the virtual pad; attaching again", p->tag); vpad_retry_in(p, 1.0); }
}
static void vpad_attach(void* arg) {
    Pad* p = arg;
    if (p->vretry) { timer_cancel(p->vretry); p->vretry = 0; }   // a no-op when the timer itself called us
    if (!p->want_vpad || p->vconn) return;
    int sv[2];
    int ok = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0;
    if (ok && vhci_attach(p, sv[0])) {
        close(sv[0]);  // the kernel holds its own reference
        p->vconn = conn_new(sv[1], 1, on_vpad_end, p);
        conn_start_feeder(p->vconn);
        return;
    }
    if (ok) { close(sv[0]); close(sv[1]); }
    vpad_retry_in(p, 3.0);
}
static void vpad_set(Pad* p, int want) {
    if (want == p->want_vpad) return;
    p->want_vpad = want;
    if (want) vpad_attach(p);
    else vhci_detach(p);  // the kernel closes its end; our end sees the hang-up and on_vpad_end runs
}

// ---------------------------------------------------------------- Bluetooth side
// HCI "QoS Setup" on the pad's ACL link: ~70 -> ~300 input reports/s. Lost at every reconnect, needs root (raw HCI socket). The command is sent here and
// its answer is read from the event loop (with a 2 s timeout), so a slow controller can never stall the virtual pad. One command in flight per pad.
static void qos_finish(Pad* p, const char* msg) {
    LOGI("%sBluetooth QoS: %s", p->tag, msg);
    if (p->qos.timer) timer_cancel(p->qos.timer);
    p->qos.timer = 0;
    if (p->qos.fd >= 0) { fd_del(p->qos.fd); close(p->qos.fd); p->qos.fd = -1; }
}
static void qos_timeout(void* arg) { Pad* p = arg; p->qos.timer = 0; qos_finish(p, "no answer from the controller"); }
static void qos_event(int fd, uint32_t ev, void* ctx) {
    (void)ev;
    Pad* p = ctx;
    uint8_t b[260];
    ssize_t r;
    char t[96];
    while ((r = recv(fd, b, sizeof b, MSG_DONTWAIT)) > 0) {
        if (r < 4) continue;
        if (b[0] == 0x04 && b[1] == 0x0F && b[3]) { snprintf(t, sizeof t, "Command Status %#x", b[3]); qos_finish(p, t); return; }
        if (b[0] == 0x04 && b[1] == 0x0D) {   // QoS Setup Complete: every pad's socket sees every pad's event, so only the one for this pad's connection counts
            if (r < 6 || (b[4] | b[5] << 8) != p->qos.handle) continue;
            snprintf(t, sizeof t, "QoS Setup Complete: status %#x (0 = applied), latency %d us", b[3], p->qos.latency); qos_finish(p, t); return; }
        if (b[0] == 0x04 && b[1] == 0x0E && r > 6) { snprintf(t, sizeof t, "Command Complete: status %#x", b[6]); qos_finish(p, t); return; }
    }
}
static void bt_qos_begin(Pad* p, const char* mac_str, int latency_us) {
    if (p->qos.fd >= 0) qos_finish(p, "superseded by a new connection");
    uint8_t mac[6];
    unsigned m[6];
    char t[96];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) { LOGI("%sBluetooth QoS: bad pad address '%s'", p->tag, mac_str); return; }
    for (int i = 0; i < 6; i++) mac[i] = m[i];
    int s = socket(31 /*AF_BLUETOOTH*/, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 1 /*BTPROTO_HCI*/);
    if (s < 0) { LOGI("%sBluetooth QoS: raw HCI socket: %s", p->tag, strerror(errno)); return; }
    struct { uint32_t type_mask, event_mask[2]; uint16_t opcode; } flt = {0xFFFFFFFFu, {0xFFFFFFFFu, 0xFFFFFFFFu}, 0};
    setsockopt(s, 0 /*SOL_HCI*/, 2 /*HCI_FILTER*/, &flt, sizeof flt);
    struct { sa_family_t fam; uint16_t dev, chan; } addr = {31, 0, 0};
    if (bind(s, (struct sockaddr*)&addr, sizeof addr) < 0) { snprintf(t, sizeof t, "bind: %s", strerror(errno)); goto fail; }
    uint8_t cl[4 + 16 * 16] = {0};
    cl[2] = 16;  // conn_num
    if (ioctl(s, 0x800448D4 /*HCIGETCONNLIST*/, cl) < 0) { snprintf(t, sizeof t, "HCIGETCONNLIST: %s", strerror(errno)); goto fail; }
    int handle = -1, n = cl[2] | cl[3] << 8;
    for (int i = 0; i < n && i < 16; i++) {
        const uint8_t* c = cl + 4 + 16 * i;  // handle(2) bdaddr(6, little endian) type(1) out(1) state(2) mode(4)
        int same = 1;
        for (int k = 0; k < 6; k++) same &= c[2 + k] == mac[5 - k];
        if (same && c[8] == 1) handle = c[0] | c[1] << 8;
    }
    if (handle < 0) { snprintf(t, sizeof t, "no ACL connection to the pad found"); goto fail; }
    uint8_t cmd[4 + 20] = {0x01, 0x07, 0x08, 20};  // params: handle(2) flags(1) service type(1) token rate(4) peak(4) latency(4) delay variation(4)
    cmd[4] = handle & 0xFF; cmd[5] = handle >> 8; cmd[7] = 2;  // flags 0, service type 2 = guaranteed
    for (int i = 0; i < 4; i++) { cmd[16 + i] = latency_us >> (8 * i); cmd[20 + i] = latency_us >> (8 * i); }
    if (write(s, cmd, sizeof cmd) < 0) { snprintf(t, sizeof t, "send: %s", strerror(errno)); goto fail; }
    p->qos.fd = s;
    p->qos.latency = latency_us;
    p->qos.handle = handle;
    fd_add(s, EPOLLIN, qos_event, p);
    p->qos.timer = timer_at(mono() + 2.0, qos_timeout, p);
    return;
fail:
    close(s);
    LOGI("%sBluetooth QoS: %s", p->tag, t);
}

// Activity = anything a person does with the pad. Sticks count when they move away from where they were at the last activity; buttons, triggers and
// touch when they are not at rest. The gyro and accelerometer are ignored (they never rest). u = the 64-byte USB-style input report.
static int pad_active(const uint8_t* u, uint8_t ref[4], int* have_ref) {
    int act = 0;
    if (!*have_ref) { memcpy(ref, u + 1, 4); *have_ref = 1; }
    for (int i = 0; i < 4; i++) if (abs((int)u[1 + i] - ref[i]) > 12) act = 1;
    if (u[5] > 10 || u[6] > 10) act = 1;                                           // triggers
    if ((u[8] & 0x0F) != 8 || (u[8] & 0xF0) || u[9] || (u[10] & 0x07)) act = 1;    // d-pad, face buttons, L1/R1/L2/R2/create/options/L3/R3, PS/touchpad/mute
    if (!(u[33] & 0x80) || !(u[37] & 0x80)) act = 1;                               // touchpad contacts (bit 7 set = not touching)
    if (act) memcpy(ref, u + 1, 4);
    return act;
}

// What the game has asked of the pad that the pad forgets when its Bluetooth link drops: the adaptive trigger effects, the LEDs and the audio route. They are merged
// field by field as the game's output reports go by (even while the pad is away) and replayed when the pad comes back inside the grace period; a new session starts clean.
// Offsets are in the 63-byte USB output report: flag0 [1] (bit 2 right trigger, bit 3 left, bit 4 headphone volume, bit 5 speaker volume, bit 7 audio control),
// flag1 [2] (bit 2 lightbar, bit 4 player LEDs, bit 6 audio control 2), flag2 [39] (bit 1 lightbar setup), headphone volume [5], speaker volume [6], audio control [8]
// (bits 4-5: output path, 0 headphones, 3 speaker), right effect [11..21], left effect [22..32], audio control 2 [38] (speaker pre-gain), lightbar setup [42],
// brightness [43], player LEDs [44], lightbar RGB [45..47]. Wine's DualSense code sends the speaker route (0x3c) and volume (0x64) at start-up.
#define AUDIO_F0 0xB0   // flag0 bits that carry the audio route: headphone volume, speaker volume, audio control
static void route_override(uint8_t r[63], int mic_on) {   // --output and --speaker-volume win over what the game says; the microphone select bit stays while the pad mic is on
    if (cfg.speaker_volume >= 0 && (r[1] & 0x20)) r[6] = (uint8_t)cfg.speaker_volume;
    if (cfg.output && (r[1] & 0x80)) r[8] = (uint8_t)((r[8] & ~0x30) | (cfg.output == 1 ? 0x30 : 0x00));
    if (mic_on && (r[1] & 0x80)) r[8] = (uint8_t)((r[8] & ~0x03) | 0x01);   // audio control bits 0-1: 1 = internal microphone (what ds5_mic_volume_packet selects)
}
static void shadow_merge(Shadow* s, const uint8_t* r0, size_t n) {
    uint8_t r[63] = {0};
    memcpy(r, r0, MIN(n, sizeof r));
    if (r[1] & 0x10) { s->rep[5] = r[5]; s->f0 |= 0x10; }
    if (r[1] & 0x20) { s->rep[6] = r[6]; s->f0 |= 0x20; }
    if (r[1] & 0x80) { s->rep[8] = r[8]; s->f0 |= 0x80; }
    if (r[2] & 0x40) { s->rep[38] = r[38]; s->f1 |= 0x40; }
    if (r[1] & 0x04) { memcpy(s->rep + 11, r + 11, 11); s->f0 |= 0x04; }
    if (r[1] & 0x08) { memcpy(s->rep + 22, r + 22, 11); s->f0 |= 0x08; }
    if (r[2] & 0x04) { memcpy(s->rep + 45, r + 45, 3); s->f1 |= 0x04; }
    if (r[2] & 0x10) { s->rep[44] = r[44]; s->f1 |= 0x10; }
    if (r[39] & 0x02) { s->rep[42] = r[42]; s->rep[43] = r[43]; s->f2 |= 0x02; }
}
static int shadow_report(const Shadow* s, uint8_t out[63], int mic_on) {  // the report that restores the remembered state; 0 when there is nothing to restore
    memcpy(out, s->rep, 63);
    out[0] = 0x02; out[1] = s->f0; out[2] = s->f1; out[39] = s->f2;
    route_override(out, mic_on);
    return (s->f0 | s->f1 | s->f2) != 0;
}
// Only the audio part of the remembered state, plus what --output and --speaker-volume ask for even if the game said nothing; 0 when there is nothing to say.
static int route_report(const Shadow* s, uint8_t r[63], int mic_on) {
    memset(r, 0, 63);
    r[0] = 0x02;
    r[1] = s->f0 & AUDIO_F0; r[2] = s->f1 & 0x40;
    r[5] = s->rep[5]; r[6] = s->rep[6]; r[8] = s->rep[8]; r[38] = s->rep[38];
    if (cfg.output || mic_on) r[1] |= 0x80;
    if (cfg.speaker_volume >= 0) r[1] |= 0x20;
    route_override(r, mic_on);
    return r[1] || r[2];
}
static int br_next_seq(void* c) { Pad* p = c; p->seq = (p->seq + 1) & 15; return p->seq; }
static void br_send(void* c, const uint8_t* pkt, size_t n) {
    Pad* p = c;
    if (p->dev.bt_fd < 0) return;
    if (write(p->dev.bt_fd, pkt, n) < 0) {
        if (errno == EAGAIN) note("WARNING", "bt-write-dropped", "pad busy");
        else note("ERROR", "bt-write-failed", "%s", strerror(errno));
    }
}
static int br_headphone(void* c) { Pad* p = c; if (cfg.output) return cfg.output == 2; return p->dev.has_live && (p->dev.live[54] & 1); }
static int br_mic_on(void* c) { return cfg.mic && ((Pad*)c)->dev.mic_active; }   // the host having the virtual mic open means nothing to the pad unless --mic

static void br_on_output(Device* d, const uint8_t* r, size_t n) {
    Pad* p = d->user;
    log_output(r, n);
    if (n && r[0] == 0x02) {
        shadow_merge(&p->shadow, r, n);
        uint8_t u[63] = {0}, o[DS5_STATE_LEN];
        memcpy(u, r, MIN(n, sizeof u));
        route_override(u, br_mic_on(p));
        ds5_usb_to_bt_output(u, sizeof u, br_next_seq(p), o);
        br_send(p, o, sizeof o);   // does nothing while the pad is away
    }
}
static void br_route(void* c) {   // the speaker is about to start: say again where the audio goes and how loud (the pad may have reset it when the audio lane opened)
    Pad* p = c;
    uint8_t r[63], o[DS5_STATE_LEN];
    if (!route_report(&p->shadow, r, br_mic_on(p))) return;
    ds5_usb_to_bt_output(r, sizeof r, br_next_seq(p), o);
    br_send(p, o, sizeof o);
}
#define REPLAY_DELAY_S 1.0   // the kernel's hid-playstation also sends start-up reports (lightbar, player LEDs) when the pad connects; waiting a second should let them go first (the order was not measured)
static void br_replay(Pad* p) {
    uint8_t r[63], o[DS5_STATE_LEN];
    if (!shadow_report(&p->shadow, r, br_mic_on(p))) return;
    ds5_usb_to_bt_output(r, sizeof r, br_next_seq(p), o);
    br_send(p, o, sizeof o);
    LOGI("%sreplayed the trigger effects, LEDs and audio route the game had set", p->tag);
}
static void br_replay_cb(void* arg) { Pad* p = arg; p->replay_timer = 0; if (p->dev.bt_fd >= 0) br_replay(p); }
static void br_on_audio(Device* d, const uint8_t* b, size_t n) { Pad* p = d->user; if (p->audio) bta_feed(p->audio, b, n); }
static void br_on_mic(Device* d, int on) {
    Pad* p = d->user;
    uint8_t m[DS5_MIC_STATUS_LEN];
    ds5_mic_status_packet(br_next_seq(p), on, m);
    br_send(p, m, sizeof m);
    if (on) { uint8_t v[DS5_STATE_LEN]; ds5_mic_volume_packet(br_next_seq(p), cfg.mic_volume, v); br_send(p, v, sizeof v); }
    LOGI("%spad microphone %s", p->tag, on ? "ON" : "off");
}
static void br_cleanup(Pad* p) {  // drop everything that belongs to the connected pad
    fd_del(p->dev.bt_fd);
    close(p->dev.bt_fd);
    p->dev.bt_fd = -1;
    p->dev.has_live = 0;
    p->dev.on_audio = dev_default_audio; p->dev.on_mic = dev_default_mic;   // on_output stays: it keeps recording what the game sets while the pad is away
    mic_free(p->dev.mic);
    p->dev.mic = NULL;
    if (p->pacer) timer_cancel(p->pacer);
    if (p->idle_timer) timer_cancel(p->idle_timer);
    if (p->replay_timer) timer_cancel(p->replay_timer);
    p->pacer = p->idle_timer = p->replay_timer = 0;
    if (p->qos.fd >= 0) qos_finish(p, "the pad is gone");
    if (p->audio) { bta_free(p->audio); free(p->audio); p->audio = NULL; }
}
static void br_grace_cb(void* arg) {  // the pad did not come back in time: the virtual pad goes away too (no ghost controller)
    Pad* p = arg;
    p->grace_timer = 0;
    if (p->dev.bt_fd >= 0) return;
    LOGI("%spad gone for %.0f s: removing the virtual pad", p->tag, cfg.grace_s);
    memset(&p->shadow, 0, sizeof p->shadow);
    if (cfg.attach) vpad_set(p, 0);
}
static void br_lost(Pad* p, const char* why) {
    LOGW("%sBluetooth pad gone (%s)", p->tag, why);
    br_cleanup(p);
    if (cfg.attach && p->want_vpad) p->grace_timer = timer_at(mono() + cfg.grace_s, br_grace_cb, p);
}
// Input reports per second in 10 s windows: logged after the first window of a connection (shows whether the QoS took) and after that only when it moves by a quarter.
static void br_count_rate(Pad* p, double now) {
    p->rate_n++;
    if (now - p->rate_t < 10) return;
    double r = p->rate_n / (now - p->rate_t);
    if (p->rate_last == 0 || r < p->rate_last * 0.75 || r > p->rate_last * 1.25) { LOGI("%spad input: %.0f reports/s", p->tag, r); p->rate_last = r; }
    p->rate_n = 0; p->rate_t = now;
}
// The battery is logged when a connection starts and whenever its level step or charging state changes (the pad reports 10 % steps); a low level is a warning.
static void br_battery(Pad* p, uint8_t status) {
    if (p->batt_seen && status == p->batt_last) return;
    p->batt_seen = 1; p->batt_last = status;
    Ds5Battery b = ds5_battery(status);
    if ((status >> 4) == 0 && b.percent <= 15) LOGW("%spad battery low: %d %% (%s)", p->tag, b.percent, b.state);
    else LOGI("%spad battery %d %% (%s)", p->tag, b.percent, b.state);
}
static void br_on_read(int fd, uint32_t ev, void* ctx) {
    (void)ev;
    Pad* p = ctx;
    for (;;) {
        uint8_t r[128];
        ssize_t n = read(fd, r, sizeof r);
        if (n == 0) { br_lost(p, "end of file"); return; }   // never from a real hidraw (it fails with EIO), but a loop on 0 would spin the whole daemon
        if (n < 0) {
            if (errno == EAGAIN) return;
            if (errno == EINTR) continue;
            br_lost(p, strerror(errno));
            return;
        }
        if (n >= 74 && r[0] == 0x31 && ((r[1] >> 1) & 1)) {  // HasMic: Opus audio, not pad state
            if (p->dev.mic) mic_feed(p->dev.mic, r + 3, 71);
        } else if (n >= 65 && r[0] == 0x31) {  // BT 0x31 -> USB 0x01 (63 B payload at offset 2)
            Device* d = &p->dev;
            d->live[0] = 1;
            memcpy(d->live + 1, r + 2, 63);
            if (pad_active(d->live, p->ref, &p->have_ref)) p->last_activity = mono();
            d->live[54] |= 0x08;  // PluggedUsbData: the virtual pad is USB for the host
            d->has_live = 1;
            d->live_t = mono();
            br_count_rate(p, d->live_t);
            br_battery(p, d->live[53]);
        } else {
            char k[48];
            snprintf(k, sizeof k, "bt-input-unexpected-%02x", n > 0 ? r[0] : 0);
            note("INFO", k, "len=%zd", n);  // e.g. the 10 B simple report
        }
    }
}
static void br_pacer_cb(void* arg) {  // 0x39 packets at a steady 21.33 ms (two 512-frame chunks), 2-packet prebuffer
    Pad* p = arg;
    BtAudio* a = p->audio;
    if (!a) return;
    p->pacer_next += 1024.0 / 48000;
    if (!p->started && a->qn >= 2) p->started = 1;
    if (p->started) {
        if (a->qn) { br_send(p, a->outq[a->qhead], DS5_AUDIO_LEN); a->qhead = (a->qhead + 1) & 15; a->qn--; p->sent++; p->depth = MAX(p->depth, a->qn); }
        else { p->started = 0; p->under++; p->pacer_next = MAX(p->pacer_next, mono()); }  // underrun: re-prebuffer instead of bursting
    }
    if (mono() - p->pacer_log > 5) {
        if (p->sent || p->under) LOGI("%sbt audio: %.1f packets/s (ideal 46.9), queue max %d, underruns %d", p->tag, p->sent / (mono() - p->pacer_log), p->depth, p->under);
        p->sent = p->under = p->depth = 0;
        p->pacer_log = mono();
    }
    p->pacer = timer_at(p->pacer_next, br_pacer_cb, p);
}
static double idle_limit(void) { return cfg.idle_off_sec > 0 ? cfg.idle_off_sec : cfg.idle_off_min * 60.0; }  // seconds, 0 = never
static double idle_tick(void) { return MIN(10.0, MAX(0.25, idle_limit() / 4)); }
static void br_idle_cb(void* arg) {  // idle too long: tell the pad to power itself off (it then drops the Bluetooth link)
    Pad* p = arg;
    p->idle_timer = 0;
    if (p->dev.bt_fd < 0) return;
    if (mono() - p->last_activity > idle_limit()) {
        uint8_t rep[47];
        ds5_power_off_report(rep);
        LOGI("%sno activity for %.0f s: powering the pad off", p->tag, idle_limit());
        if (ds5_set_feature(p->dev.bt_fd, rep, sizeof rep) < 0) LOGW("%spower-off report failed: %s", p->tag, strerror(errno));
        p->last_activity = mono();  // if it stays on, try again after another full period
    }
    p->idle_timer = timer_at(mono() + idle_tick(), br_idle_cb, p);
}
// A new connection starts by reading three feature reports from the real pad (calibration, pairing info, firmware info: the virtual pad serves them). The ioctl blocks
// until the pad answers (30 ms on a good pad, the kernel's timeout on one that does not), so it runs in a thread: the loop, and with it every other pad, keeps going.
// The thread only touches its own FeatJob and tells the loop through an eventfd; everything else about the pad starts in pad_start() once the answers are in.
typedef struct FeatJob {
    int fd, efd;
    uint8_t f05[64], f09[64], f20[64];
    int n05, n09, n20, err;
    double ms;
    pthread_t th;
} FeatJob;
static void* feat_thread(void* arg) {
    FeatJob* j = arg;
    double t0 = mono();
    if (cfg.feature_delay_ms) usleep((useconds_t)cfg.feature_delay_ms * 1000);   // tests: a pad that answers slowly
    j->n05 = ds5_get_feature(j->fd, 0x05, j->f05, 41);
    j->n09 = ds5_get_feature(j->fd, 0x09, j->f09, 20);
    j->n20 = ds5_get_feature(j->fd, 0x20, j->f20, 64);
    j->err = errno;
    j->ms = (mono() - t0) * 1000;
    uint64_t one = 1;
    (void)!write(j->efd, &one, sizeof one);
    return NULL;
}
static void pad_start(Pad* p, int back);
static void feat_done(int efd, uint32_t ev, void* ctx) {
    (void)ev;
    Pad* p = ctx;
    FeatJob* j = p->job;
    pthread_join(j->th, NULL);
    p->job = NULL;
    fd_del(efd);
    close(efd);
    if (j->ms > 500) LOGW("%sreading the pad's feature reports took %.0f ms (the other pads kept running)", p->tag, j->ms);
    else LOGI("%spad feature reports read in %.0f ms", p->tag, j->ms);
    if (j->n05 < 0 || j->n09 < 0 || j->n20 < 0) LOGW("%scould not read the pad's feature reports (%s); the virtual pad will have placeholders", p->tag, strerror(j->err));
    dev_set_features(&p->dev, j->n05 > 0 ? j->f05 : NULL, j->n05, j->n09 > 0 ? j->f09 : NULL, j->n09, j->n20 > 0 ? j->f20 : NULL, j->n20);
    free(j);
    pad_start(p, 0);
}
// Short name for the log: the last three bytes of the pad's Bluetooth address, or the end of the hidraw path when there is none (tests).
static void pad_name(Pad* p, const char* key) {
    size_t n = strlen(key);
    snprintf(p->tag, sizeof p->tag, "[%s] ", n > 8 ? key + n - 8 : key);
}
static int pad_attach(Pad* p, const char* node, const char* key) {
    int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { note("WARNING", "bt-attach-failed", "%s (often udev permissions right after the pad connects); retrying", strerror(errno)); return 0; }
    p->dev.bt_fd = fd;
    if (p->claimed && strcmp(p->key, key)) dev_placeholder_features(&p->dev);   // the slot passes to a different pad
    snprintf(p->key, sizeof p->key, "%s", key);
    snprintf(p->node, sizeof p->node, "%s", node);
    pad_name(p, key);
    p->claimed = 1;
    if (p->grace_timer) { timer_cancel(p->grace_timer); p->grace_timer = 0; LOGI("%spad is back within the grace period", p->tag); pad_start(p, 1); return 1; }
    if (!cfg.attach || p->want_vpad) { pad_start(p, 0); return 1; }
    // a new connection: take the real pad's per-unit feature reports (this also makes the pad send full reports)
    memset(&p->shadow, 0, sizeof p->shadow);   // nothing carries over from an earlier session
    FeatJob* j = calloc(1, sizeof *j);
    j->fd = fd;
    j->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (j->efd < 0 || pthread_create(&j->th, NULL, feat_thread, j)) {   // no thread: read them here, blocking, as the first version did
        if (j->efd >= 0) close(j->efd);
        j->n05 = ds5_get_feature(fd, 0x05, j->f05, 41); j->n09 = ds5_get_feature(fd, 0x09, j->f09, 20); j->n20 = ds5_get_feature(fd, 0x20, j->f20, 64);
        dev_set_features(&p->dev, j->n05 > 0 ? j->f05 : NULL, j->n05, j->n09 > 0 ? j->f09 : NULL, j->n09, j->n20 > 0 ? j->f20 : NULL, j->n20);
        free(j);
        pad_start(p, 0);
        return 1;
    }
    p->job = j;
    fd_add(j->efd, EPOLLIN, feat_done, p);
    return 1;
}
// Everything else a connection needs, once the pad's feature reports are in: QoS, the input reader, timers, audio, and the virtual pad.
static void pad_start(Pad* p, int back) {
    int fd = p->dev.bt_fd;
    if (cfg.qos_us && !cfg.nhidraw) {
        if (strchr(p->key, ':')) { char mac[128]; snprintf(mac, sizeof mac, "%s", p->key); for (char* c = mac; *c; c++) if (*c >= 'a' && *c <= 'f') *c -= 32; bt_qos_begin(p, mac, cfg.qos_us); }
        else LOGI("%sBluetooth QoS: pad address unknown", p->tag);
    }
    fd_add(fd, EPOLLIN, br_on_read, p);
    p->have_ref = 0;
    p->rate_n = 0; p->rate_last = 0; p->rate_t = mono(); p->batt_seen = 0;
    p->last_activity = mono();
    if (idle_limit() > 0) p->idle_timer = timer_at(mono() + idle_tick(), br_idle_cb, p);
    if (cfg.mic) {
        p->dev.mic = mic_new();
        p->dev.on_mic = br_on_mic;
        if (p->dev.mic_active) br_on_mic(&p->dev, 1);  // the host already had the mic open when the pad (re)connected
    }
    if (!cfg.no_haptics) {
        p->audio = malloc(sizeof *p->audio);
        bta_init(p->audio, br_send, br_next_seq, p, cfg.haptics_gain, !cfg.no_speaker, 1, cfg.audio_buffer);
        p->audio->headphone = br_headphone; p->audio->mic_on = br_mic_on; p->audio->route = br_route;
        p->dev.on_audio = br_on_audio;
        p->started = 0;
        p->pacer_next = p->pacer_log = mono();
        p->pacer = timer_at(p->pacer_next, br_pacer_cb, p);
    }
    LOGI("%sBluetooth pad %s: input -> virtual pad, game output -> pad%s", p->tag, p->node,
         cfg.no_haptics ? "" : (cfg.no_speaker ? ", audio ch3-4 -> haptics" : ", audio ch3-4 -> haptics, ch1-2 -> speaker"));
    if (back) p->replay_timer = timer_at(mono() + REPLAY_DELAY_S, br_replay_cb, p);
    if (cfg.attach) vpad_set(p, 1);
}
// The slot of a pad we already know by its address, else an unused one, else one whose pad is gone for good (no virtual pad left); NULL when all are busy.
static Pad* pad_for(const char* key) {
    for (int i = 0; i < MAX_PADS; i++) if (g_pads[i].claimed && !strcmp(g_pads[i].key, key)) return &g_pads[i];
    for (int i = 0; i < MAX_PADS; i++) if (!g_pads[i].claimed) return &g_pads[i];
    for (int i = 0; i < MAX_PADS; i++) {
        Pad* p = &g_pads[i];
        if (p->dev.bt_fd < 0 && !p->grace_timer && !p->want_vpad && !p->vconn) return p;
    }
    return NULL;
}
static void br_watch_cb(void* arg) {  // a pad may be off when the daemon starts, or turn up or go away later: look for them every second
    (void)arg;
    for (int i = 0;; i++) {
        char node[300], key[128] = "";
        if (cfg.nhidraw) {   // tests: the nodes are given
            if (i >= cfg.nhidraw) break;
            snprintf(node, sizeof node, "%s", cfg.hidraw[i]);
            if (access(node, F_OK) != 0) { note("INFO", "no-bt-pad", "waiting for %s", node); continue; }
        } else if (!ds5_find_hidraw_nth(1, i, node, sizeof node, key, sizeof key)) {
            if (i == 0) note("INFO", "no-bt-pad", "waiting for the Bluetooth DualSense");
            break;
        }
        if (!key[0]) snprintf(key, sizeof key, "%s", node);   // no address (tests): the path is the identity
        Pad* p = pad_for(key);
        if (!p) { note("WARNING", "too-many-pads", "more than %d Bluetooth pads: %s is ignored", MAX_PADS, node); continue; }
        if (p->dev.bt_fd >= 0) continue;                      // attached already
        pad_attach(p, node, key);
    }
    timer_at(mono() + 1.0, br_watch_cb, NULL);
}

// ---------------------------------------------------------------- listener
static int g_lfd = -1;
static void accept_cb(int fd, uint32_t ev, void* ctx) {
    (void)ev; (void)ctx;
    for (;;) {
        int c = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) return;
        if (!FD_OK(c)) { close(c); continue; }   // cannot be served: the handler table is full
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        conn_new(c, 0, NULL, &g_pads[0]);   // the debug listener serves the first pad
    }
}
static int listen_on(int port) {
    g_lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(g_lfd, (struct sockaddr*)&a, sizeof a) < 0 || listen(g_lfd, 8) < 0) { LOGE("cannot listen on 127.0.0.1:%d: %s", port, strerror(errno)); return 0; }
    fd_add(g_lfd, EPOLLIN, accept_cb, NULL);
    return 1;
}

// ---------------------------------------------------------------- main
static void usage(void) {
    printf("ds5bridge: makes a Bluetooth DualSense look like a USB DualSense (adaptive triggers, HD haptics, speaker for Proton/Wine games)\n"
           "usage: ds5bridge [options]      (run as root: it writes to vhci_hcd's sysfs and opens the pad's hidraw node)\n"
           "  --idle-off MIN        power the pad off after MIN minutes without input or haptics (default 15, 0 = never)\n"
           "  --grace SEC           keep the virtual pad this long after the real one drops (default 3)\n"
           "  --haptics-gain G      haptics level 0.1-2 (default 1)       --audio-buffer N  pad audio buffer 16-127 (default 100)\n"
           "  --qos-latency US      Bluetooth poll latency, 0 = off (default 5000)\n"
           "  --output WHERE        where channels 1-2 play: auto (the game and the headphone jack decide, default), speaker, headset or off (= --no-speaker)\n"
           "  --speaker-volume N    force the pad's speaker volume 0-255 (the pad takes about 61-100; default: what the game sets)\n"
           "  --no-speaker          do not send channels 1-2 to the pad speaker   --no-haptics  do not turn channels 3-4 into haptics\n"
           "  --mic                 enable the pad microphone (needs --mic-volume 0-64, --mic-gain; not usable on every pad)\n"
           "  --no-attach           do not attach to vhci_hcd (use with --port for debugging and the tests)\n"
           "  --port N              also serve the virtual pad over TCP on 127.0.0.1:N (debugging and tests, default off)\n"
           "  --no-bt               do not look for the Bluetooth pad     --log FILE  also log to FILE (stderr is the journal)\n"
           "  --print-config        print the parsed options and exit (the unit tests live in tests/test_unit.c)\n"
           "  --version             print the version and exit\n");
}
// SIGINT/SIGTERM stop the daemon. They are blocked and read from a signalfd, so there is no window in which a signal can arrive between the loop's
// check and epoll_wait.
static void sig_cb(int fd, uint32_t ev, void* ctx) {
    (void)ev; (void)ctx;
    struct signalfd_siginfo si;
    while (read(fd, &si, sizeof si) == (ssize_t)sizeof si) running = 0;
}

int main(int argc, char** argv) {
    ds5_crc_init();
    int do_print = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
#define VAL() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "%s needs a value\n", a), exit(2), (char*)0))
        if (!strcmp(a, "--version")) { puts("ds5bridge " DS5_VERSION); return 0; }
        else if (!strcmp(a, "--print-config")) do_print = 1;
        else if (!strcmp(a, "--no-attach")) cfg.attach = 0;
        else if (!strcmp(a, "--no-bt")) cfg.no_bt = 1;
        else if (!strcmp(a, "--mic")) cfg.mic = 1;
        else if (!strcmp(a, "--no-speaker")) cfg.no_speaker = 1;
        else if (!strcmp(a, "--output")) {
            const char* v = VAL();
            if (!strcmp(v, "auto")) cfg.output = 0;
            else if (!strcmp(v, "speaker")) cfg.output = 1;
            else if (!strcmp(v, "headset")) cfg.output = 2;
            else if (!strcmp(v, "off")) cfg.no_speaker = 1;
            else { fprintf(stderr, "--output takes auto, speaker, headset or off\n"); return 2; }
        }
        else if (!strcmp(a, "--speaker-volume")) { int v = atoi(VAL()); cfg.speaker_volume = MIN(255, MAX(0, v)); }
        else if (!strcmp(a, "--no-haptics")) cfg.no_haptics = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "--port")) cfg.port = atoi(VAL());
        else if (!strcmp(a, "--log")) snprintf(cfg.log_file, sizeof cfg.log_file, "%s", VAL());
        else if (!strcmp(a, "--audio-buffer")) { int v = atoi(VAL()); cfg.audio_buffer = MIN(127, MAX(16, v)); }
        else if (!strcmp(a, "--mic-volume")) { int v = atoi(VAL()); cfg.mic_volume = MIN(64, MAX(0, v)); }
        else if (!strcmp(a, "--mic-gain")) { double v = atof(VAL()); cfg.mic_gain = MAX(0.0, v); }
        else if (!strcmp(a, "--qos-latency")) { int v = atoi(VAL()); cfg.qos_us = MAX(0, v); }
        else if (!strcmp(a, "--idle-off")) { int v = atoi(VAL()); cfg.idle_off_min = MAX(0, v); }
        else if (!strcmp(a, "--grace")) { double v = atof(VAL()); cfg.grace_s = MAX(0.0, v); }
        else if (!strcmp(a, "--idle-off-sec")) { double v = atof(VAL()); cfg.idle_off_sec = MAX(0.0, v); }   // tests
        else if (!strcmp(a, "--hidraw")) { const char* v = VAL(); if (cfg.nhidraw <= MAX_PADS) snprintf(cfg.hidraw[cfg.nhidraw++], sizeof cfg.hidraw[0], "%s", v); }   // tests
        else if (!strcmp(a, "--feature-delay-ms")) { int v = atoi(VAL()); cfg.feature_delay_ms = MAX(0, v); }                                                          // tests
        else if (!strcmp(a, "--fake-vhci")) cfg.fake_vhci = 1;                                         // tests
        else if (!strcmp(a, "--haptics-gain")) { double v = atof(VAL()); cfg.haptics_gain = MIN(2.0, MAX(0.1, v)); }
        else { fprintf(stderr, "unknown option %s (try --help)\n", a); return 2; }
    }
    if (do_print) {
        printf("attach=%d port=%d mic=%d no_speaker=%d no_haptics=%d audio_buffer=%d mic_volume=%d mic_gain=%.2f qos_us=%d idle_off_min=%d grace_s=%.1f haptics_gain=%.2f "
               "output=%d speaker_volume=%d\n",
               cfg.attach, cfg.port, cfg.mic, cfg.no_speaker, cfg.no_haptics, cfg.audio_buffer, cfg.mic_volume, cfg.mic_gain, cfg.qos_us, cfg.idle_off_min, cfg.grace_s,
               cfg.haptics_gain, cfg.output, cfg.speaker_volume);
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    if (cfg.log_file[0]) g_log = fopen(cfg.log_file, "a");
    for (int i = 0; i < MAX_PADS; i++) pad_init(&g_pads[i]);
    loop_init();
    if (cfg.port && !listen_on(cfg.port)) return 1;
    if (!cfg.no_bt) br_watch_cb(NULL);
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT); sigaddset(&sigs, SIGTERM);
    sigprocmask(SIG_BLOCK, &sigs, NULL);
    fd_add(signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC), EPOLLIN, sig_cb, NULL);
    LOGI("ds5bridge started%s%s", cfg.attach ? ": the virtual USB pad appears while a Bluetooth DualSense is connected" : "",
         cfg.port ? " (and a debug TCP listener)" : "");
    if (cfg.port) LOGI("debug TCP listener on 127.0.0.1:%d", cfg.port);
    loop_run();
    for (int i = 0; i < MAX_PADS; i++) vhci_detach(&g_pads[i]);
    LOGI("stopped");
    return 0;
}
