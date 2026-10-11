// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// writes a deterministic 950272-byte pseudo firmware image (version bytes 0x1234 at offset 120, product id 0x0CE6 at 0x62) for the ds5fw dry-run test; NOT a real firmware
#include <stdio.h>
#include <stdint.h>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "wb");
    if (!f) return 1;
    uint32_t x = 12345;
    for (int i = 0; i < 950272; i++) { x = x * 1103515245u + 12345u; fputc(i == 120 ? 0x34 : i == 121 ? 0x12 : i == 0x62 ? 0xE6 : i == 0x63 ? 0x0C : (x >> 16) & 0xFF, f); }
    fclose(f);
    return 0;
}
