// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
/* Windows (WASAPI) test client: plays a known haptic signal on channels 3-4 of the DualSense audio endpoint, the way a game does.
 * Run it under Wine/Proton (tools/wasapi/run.sh). Channels 1-2 (speaker) stay silent.
 * Argument "highband": 4 cycles of [1 s of 160 Hz, 1 s of 2500 Hz, 1 s of silence] on ch3-4. A 2.5 kHz tone is above the 1.5 kHz the haptic stream can
 * carry and must NOT be felt; a filter that lets it alias leaves a ~500 Hz buzz (a hardware check of ds5bridge's haptics filter).
 * Signal (ch3-4): 40, 80, 160, 320 Hz tones (1.5 s each, 0.6 s gaps), then a 100 Hz swell from 0 to loud over 2 s. */
typedef void *HM; typedef long HRESULT; typedef unsigned long DWORD; typedef unsigned int UINT;
typedef unsigned short WCHAR;
typedef struct { unsigned d1; unsigned short d2, d3; unsigned char d4[8]; } GUID;
__declspec(dllimport) void __stdcall Sleep(unsigned long);
__declspec(dllimport) const char *__stdcall GetCommandLineA(void);
int printf(const char *, ...); int fflush(void *); double sin(double);
HRESULT __stdcall CoInitializeEx(void *, DWORD);
HRESULT __stdcall CoCreateInstance(const GUID *, void *, DWORD, const GUID *, void **);
static const GUID CLSID_Enum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_Enum   = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_Client = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID IID_Render = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
typedef struct { GUID fmtid; DWORD pid; } PKEY;
static const PKEY PKEY_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
typedef struct { HRESULT (__stdcall **vt)(); } Obj;
typedef struct { unsigned short tag, ch; unsigned rate, avg; unsigned short align, bits, cb; } WFX;
#define CALL(o, i, T, ...) (((T)(o)->vt[i])(o, __VA_ARGS__))
#define CALL0(o, i) (((HRESULT (__stdcall *)(Obj *))(o)->vt[i])(o))

static int has(const WCHAR *s, const char *sub) {   /* wide-string substring search, ASCII needle */
    for (; *s; s++) { int k = 0; while (sub[k] && s[k] == (WCHAR)sub[k]) k++; if (!sub[k]) return 1; }
    return 0;
}

/* no C runtime start-up here, so argc/argv are not valid: read the real command line */
static int has_arg(const char *needle) {
    const char *c = GetCommandLineA(); int in_exe_name = 1;
    for (; *c; c++) { if (in_exe_name && *c == ' ') in_exe_name = 0; if (!in_exe_name) { int k = 0; while (needle[k] && c[k] == needle[k]) k++; if (!needle[k]) return 1; } }
    return 0;
}

int main(void) {
    CoInitializeEx(0, 0);
    Obj *en = 0; if (CoCreateInstance(&CLSID_Enum, 0, 23, &IID_Enum, (void **)&en) < 0) { printf("no enumerator\n"); return 1; }
    Obj *col = 0; CALL(en, 3, HRESULT (__stdcall *)(Obj *, int, DWORD, Obj **), 0, 1, &col);   /* render, active */
    UINT n = 0; CALL(col, 3, HRESULT (__stdcall *)(Obj *, UINT *), &n);
    Obj *dev = 0; static WCHAR chosen[256];
    for (UINT i = 0; i < n; i++) {
        Obj *d = 0; CALL(col, 4, HRESULT (__stdcall *)(Obj *, UINT, Obj **), i, &d);
        Obj *ps = 0; CALL(d, 4, HRESULT (__stdcall *)(Obj *, DWORD, Obj **), 0, &ps);
        unsigned char pv[32] = {0}; CALL(ps, 5, HRESULT (__stdcall *)(Obj *, const PKEY *, void *), &PKEY_FriendlyName, pv);
        WCHAR *name = *(WCHAR **)(pv + 8);
        printf("endpoint %u: %ls\n", i, name ? name : (WCHAR *)L"?");
        if (!dev && name && has(name, "Wireless Controller")) { dev = d; for (int k = 0; k < 255 && name[k]; k++) chosen[k] = name[k]; }
    }
    if (!dev) { printf("no DualSense endpoint found\n"); return 2; }
    printf("using: %ls\n", chosen);
    Obj *cl = 0; CALL(dev, 3, HRESULT (__stdcall *)(Obj *, const GUID *, DWORD, void *, void **), &IID_Client, 23, 0, (void **)&cl);
    WFX *fmt = 0; CALL(cl, 8, HRESULT (__stdcall *)(Obj *, WFX **), &fmt);
    printf("mix format: tag %#x, %u ch, %u Hz, %u bit\n", fmt->tag, fmt->ch, fmt->rate, fmt->bits);
    if (fmt->ch < 4 || fmt->bits != 32) { printf("need a 4-channel float endpoint\n"); return 3; }
    HRESULT hr = CALL(cl, 3, HRESULT (__stdcall *)(Obj *, int, DWORD, long long, long long, WFX *, const GUID *), 0, 0, 1000000, 0, fmt, 0);
    printf("Initialize hr=%#lx\n", hr); if (hr < 0) return 4;
    UINT size = 0; CALL(cl, 4, HRESULT (__stdcall *)(Obj *, UINT *), &size);
    Obj *rc = 0; CALL(cl, 14, HRESULT (__stdcall *)(Obj *, const GUID *, void **), &IID_Render, (void **)&rc);
    int speaker = has_arg("speaker"), highband = has_arg("highband"); double T = highband ? 12.0 : 11.0, ph = 0, rate = fmt->rate; unsigned ch = fmt->ch; double t = 0;
    CALL0(cl, 10);
    printf("playing %.0f s on channels 3-4%s%s ...\n", T, highband ? " (highband: 160 Hz / 2500 Hz / silence)" : "", speaker ? " and a tone on 1-2" : ""); fflush(0);
    while (t < T) {
        UINT pad = 0; CALL(cl, 6, HRESULT (__stdcall *)(Obj *, UINT *), &pad);
        UINT frames = size - pad; if (frames > 480) frames = 480; if (!frames) { Sleep(4); continue; }
        float *buf = 0; if (CALL(rc, 3, HRESULT (__stdcall *)(Obj *, UINT, float **), frames, &buf) < 0) break;
        for (UINT i = 0; i < frames; i++, t += 1.0 / rate) {
            double f = 0, a = 0;
            if (highband) { double m = t - 3.0 * (int)(t / 3.0); if (m < 1.0) { f = 160; a = 0.5; } else if (m < 2.0) { f = 2500; a = 0.5; } }
            else if (t < 1.5) { f = 40; a = 0.5; } else if (t >= 2.1 && t < 3.6) { f = 80; a = 0.5; }
            else if (t >= 4.2 && t < 5.7) { f = 160; a = 0.5; } else if (t >= 6.3 && t < 7.8) { f = 320; a = 0.5; }
            else if (t >= 8.4 && t < 10.4) { f = 100; a = 0.55 * (t - 8.4) / 2.0; }
            ph += 6.283185307179586 * f / rate; float v = (float)(a * sin(ph));
            for (unsigned c = 0; c < ch; c++) buf[i * ch + c] = 0;
            buf[i * ch + 2] = v; buf[i * ch + 3] = v;
            if (speaker) { float s = (float)(0.2 * sin(6.283185307179586 * 440.0 * t)); buf[i * ch] = s; buf[i * ch + 1] = s; }
        }
        CALL(rc, 4, HRESULT (__stdcall *)(Obj *, UINT, DWORD), frames, 0);
        Sleep(4);
    }
    Sleep(300); CALL0(cl, 11); printf("done\n"); fflush(0); return 0;
}
