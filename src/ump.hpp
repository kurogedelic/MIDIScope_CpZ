// SPDX-License-Identifier: MIT
// Decoding of MIDI 1.0 channel messages and MIDI 2.0 UMP (Universal MIDI Packet) words into short text.
// No ALSA here, so it can be unit-tested anywhere.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace msc {

enum class Kind { Note, CC, Bend, Program, Pressure, Clock, Transport, SysEx, Other };

struct Decoded {
    Kind kind = Kind::Other;
    bool log = true;        // false: counted only (clock ticks, active sensing)
    bool m2 = false;        // a MIDI 2.0 channel voice message (UMP message type 4)
    int sys = 0;            // system real-time status (0xF8 clock, 0xFA start, 0xFB continue, 0xFC stop) or 0
    std::string text;
};

inline std::string noteName(int n) {
    static const char* nm[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (n < 0 || n > 127) return "?";
    char b[8]; snprintf(b, sizeof b, "%s%d", nm[n % 12], n / 12 - 1); return b;   // 60 = C4
}

inline std::string hexBytes(const uint8_t* d, int n, int maxShow) {
    std::string s; char b[4];
    for (int i = 0; i < n && i < maxShow; ++i) { snprintf(b, sizeof b, "%02X", d[i]); if (i) s += ' '; s += b; }
    if (n > maxShow) s += "..";
    return s;
}

// MIDI 1.0 bytes: a channel message (0x80..0xEF) or a system message (0xF1..0xFF)
inline Decoded decodeMidi1(uint8_t st, uint8_t d1, uint8_t d2) {
    Decoded r; char b[96]; int ch = (st & 0x0F) + 1;
    switch (st & 0xF0) {
        case 0x80: r.kind = Kind::Note; snprintf(b, sizeof b, "ch%02d NoteOff %s v%d", ch, noteName(d1).c_str(), d2); break;
        case 0x90: r.kind = Kind::Note;
            if (d2) snprintf(b, sizeof b, "ch%02d NoteOn  %s v%d", ch, noteName(d1).c_str(), d2);
            else snprintf(b, sizeof b, "ch%02d NoteOff %s (v0)", ch, noteName(d1).c_str()); break;
        case 0xA0: r.kind = Kind::Pressure; snprintf(b, sizeof b, "ch%02d PolyAT  %s %d", ch, noteName(d1).c_str(), d2); break;
        case 0xB0: r.kind = Kind::CC; snprintf(b, sizeof b, "ch%02d CC%-3d = %d", ch, d1, d2); break;
        case 0xC0: r.kind = Kind::Program; snprintf(b, sizeof b, "ch%02d Program %d", ch, d1); break;
        case 0xD0: r.kind = Kind::Pressure; snprintf(b, sizeof b, "ch%02d ChanAT  %d", ch, d1); break;
        case 0xE0: r.kind = Kind::Bend; snprintf(b, sizeof b, "ch%02d Bend %+d", ch, ((d2 << 7) | d1) - 8192); break;
        default:
            switch (st) {
                case 0xF1: r.kind = Kind::Transport; snprintf(b, sizeof b, "MTC quarter %02X", d1); break;
                case 0xF2: r.kind = Kind::Transport; snprintf(b, sizeof b, "SongPos %d", (d2 << 7) | d1); break;
                case 0xF3: r.kind = Kind::Transport; snprintf(b, sizeof b, "SongSelect %d", d1); break;
                case 0xF6: r.kind = Kind::Transport; snprintf(b, sizeof b, "TuneRequest"); break;
                case 0xF8: r.kind = Kind::Clock; r.log = false; r.sys = 0xF8; snprintf(b, sizeof b, "Clock"); break;
                case 0xFA: r.kind = Kind::Transport; r.sys = 0xFA; snprintf(b, sizeof b, "Start"); break;
                case 0xFB: r.kind = Kind::Transport; r.sys = 0xFB; snprintf(b, sizeof b, "Continue"); break;
                case 0xFC: r.kind = Kind::Transport; r.sys = 0xFC; snprintf(b, sizeof b, "Stop"); break;
                case 0xFE: r.kind = Kind::Other; r.log = false; snprintf(b, sizeof b, "ActiveSensing"); break;
                case 0xFF: r.kind = Kind::Transport; snprintf(b, sizeof b, "Reset"); break;
                default: snprintf(b, sizeof b, "status %02X %02X %02X", st, d1, d2); break;
            }
    }
    r.text = b; return r;
}

inline int umpWordCount(uint32_t w0) {
    switch (w0 >> 28) {
        case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
        case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;
        case 0xB: case 0xC: return 3;
        default: return 4;
    }
}

// One UMP packet (w[0..3]; only umpWordCount(w[0]) words are read). group is returned in *group when given.
inline Decoded decodeUmp(const uint32_t* w, int* group = nullptr) {
    Decoded r; char b[128];
    const uint32_t w0 = w[0]; const int mt = w0 >> 28, grp = (w0 >> 24) & 0xF;
    if (group) *group = grp;
    const int st = (w0 >> 16) & 0xFF, d1 = (w0 >> 8) & 0xFF, d2 = w0 & 0xFF;
    switch (mt) {
        case 0x0: r.log = false; snprintf(b, sizeof b, "Utility %02X", st); r.text = b; return r;       // NOOP, JR clock/timestamp
        case 0x1: r = decodeMidi1(st, d1, d2); return r;                                                  // system real-time / common
        case 0x2: r = decodeMidi1(st, d1, d2); return r;                                                  // MIDI 1.0 channel voice in UMP
        case 0x3: {                                                                                       // 7-bit SysEx
            static const char* part[] = {"complete", "start", "cont", "end"};
            int s = (w0 >> 20) & 0xF, n = (w0 >> 16) & 0xF; uint8_t d[6] = {(uint8_t)(w0 >> 8), (uint8_t)w0, (uint8_t)(w[1] >> 24), (uint8_t)(w[1] >> 16), (uint8_t)(w[1] >> 8), (uint8_t)w[1]};
            if (n > 6) n = 6;
            r.kind = Kind::SysEx; snprintf(b, sizeof b, "SysEx7 %s %dB %s", s < 4 ? part[s] : "?", n, hexBytes(d, n, 5).c_str()); r.text = b; return r; }
        case 0x4: {                                                                                       // MIDI 2.0 channel voice
            r.m2 = true; const int op = (w0 >> 20) & 0xF, ch = ((w0 >> 16) & 0xF) + 1; const uint32_t v = w[1];
            switch (op) {
                case 0x8: case 0x9: {
                    r.kind = Kind::Note; int at = w0 & 0xFF; unsigned vel = v >> 16, attr = v & 0xFFFF;
                    int n = snprintf(b, sizeof b, "ch%02d %s %s v%u", ch, op == 9 ? "NoteOn " : "NoteOff", noteName(d1).c_str(), vel);
                    if (at) snprintf(b + n, sizeof b - n, " a%d:%04X", at, attr);
                    else if (op == 9 && vel == 0) snprintf(b + n, sizeof b - n, " (v0)");
                    break; }
                case 0xA: r.kind = Kind::Pressure; snprintf(b, sizeof b, "ch%02d PolyAT  %s %08X", ch, noteName(d1).c_str(), v); break;
                case 0xB: r.kind = Kind::CC; snprintf(b, sizeof b, "ch%02d CC%-3d %08X (%u)", ch, d1, v, v >> 25); break;
                case 0xC: {
                    r.kind = Kind::Program; bool bank = w0 & 1;
                    int n = snprintf(b, sizeof b, "ch%02d Program %u", ch, v >> 24);
                    if (bank) snprintf(b + n, sizeof b - n, " bank %u:%u", (v >> 8) & 0x7F, v & 0x7F);
                    break; }
                case 0xD: r.kind = Kind::Pressure; snprintf(b, sizeof b, "ch%02d ChanAT  %08X", ch, v); break;
                case 0xE: r.kind = Kind::Bend; snprintf(b, sizeof b, "ch%02d Bend %08X (%+d)", ch, v, (int)(v >> 18) - 8192); break;
                case 0x6: r.kind = Kind::Bend; snprintf(b, sizeof b, "ch%02d NoteBend %s %08X", ch, noteName(d1).c_str(), v); break;
                case 0x2: case 0x3: case 0x4: case 0x5:
                    r.kind = Kind::CC; snprintf(b, sizeof b, "ch%02d %s%s %u:%u %08X", ch, op >= 4 ? "rel" : "", (op & 1) ? "NRPN" : "RPN", d1, d2, v); break;
                case 0x0: case 0x1: r.kind = Kind::CC; snprintf(b, sizeof b, "ch%02d NoteCtl %s #%d %08X", ch, noteName(d1).c_str(), d2, v); break;
                case 0xF: r.kind = Kind::Other; snprintf(b, sizeof b, "ch%02d NoteMgmt %s opt %02X", ch, noteName(d1).c_str(), d2); break;
                default: snprintf(b, sizeof b, "M2 op%X ch%02d", op, ch); break;
            }
            r.text = b; return r; }
        case 0x5: {                                                                                       // 8-bit SysEx / mixed data set
            r.kind = Kind::SysEx; int s = (w0 >> 20) & 0xF, n = (w0 >> 16) & 0xF; snprintf(b, sizeof b, "SysEx8 st%d %dB", s, n); r.text = b; return r; }
        case 0xD: r.kind = Kind::Other; snprintf(b, sizeof b, "FlexData st%02X", st); r.text = b; return r;
        case 0xF: {                                                                                       // UMP stream (endpoint / function blocks)
            static const struct { int s; const char* n; } names[] = {{0x00, "EndpointDiscovery"}, {0x01, "EndpointInfo"}, {0x02, "DeviceIdentity"}, {0x03, "EndpointName"},
                {0x04, "ProductId"}, {0x05, "StreamConfigReq"}, {0x06, "StreamConfig"}, {0x10, "FBDiscovery"}, {0x11, "FBInfo"}, {0x12, "FBName"}};
            int s = (w0 >> 16) & 0x3FF; const char* nm = nullptr; for (auto& e : names) if (e.s == s) nm = e.n;
            if (nm) snprintf(b, sizeof b, "UMP stream: %s", nm); else snprintf(b, sizeof b, "UMP stream %03X", s);
            r.text = b; return r; }
        default: snprintf(b, sizeof b, "UMP mt%X %08X", mt, w0); r.text = b; return r;
    }
}

// BPM estimate from MIDI clock ticks (24 per quarter note); timestamps in microseconds
struct ClockEst {
    long long last = 0; double ema = 0; int n = 0;
    void tick(long long us) {
        if (last) { double d = (double)(us - last); if (d > 4000 && d < 150000) { ema = n ? ema * 0.92 + d * 0.08 : d; ++n; } else if (d >= 150000) n = 0; }
        last = us;
    }
    double bpm(long long nowUs) const { return (n >= 12 && ema > 0 && nowUs - last < 1000000) ? 60e6 / (ema * 24.0) : 0.0; }
};

}  // namespace msc
