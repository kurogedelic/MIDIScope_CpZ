#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draws the MIDI Scope launcher icons (a dark tile with a green MIDI-note trace) as PNG files, no libraries needed."""
import math, struct, sys, zlib

def png(path, S):
    rows = []
    for y in range(S):
        row = bytearray([0])
        for x in range(S):
            u, v = x / (S - 1), y / (S - 1)
            r, g, b, a = 20, 24, 40, 255
            cx, cy = abs(u - 0.5), abs(v - 0.5)
            if cx > 0.36 and cy > 0.36 and math.hypot(cx - 0.36, cy - 0.36) > 0.14: a = 0
            if abs(v - 0.5) < 0.004 or abs(u - 0.5) < 0.004: r, g, b = 50, 56, 90          # axes
            # a trace: decaying pulse train
            t = u * 6.0; k = math.floor(t); f = t - k
            amp = [0.0, 0.34, 0.2, 0.38, 0.12, 0.26][int(k) % 6] * math.exp(-f * 3.0)
            if f < 0.02 or abs((0.5 - v) - amp) < 0.018 and amp > 0.01: r, g, b = 110, 240, 130
            if 0.0 < 0.5 - v < amp and abs(f - 0.0) < 0.02: r, g, b = 110, 240, 130
            row += bytes([r, g, b, a])
        rows.append(bytes(row))
    raw = b"".join(rows)
    def chunk(t, d): c = struct.pack(">I", len(d)) + t + d; return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

for name, size in (("midi-scope.png", 80), ("midi-scope_80.png", 80), ("midi-scope_100.png", 100)):
    png(sys.argv[1] + "/" + name, size)
