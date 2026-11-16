// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../src/ump.hpp"
using namespace msc;
static int fails = 0;
#define EXPECT(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); ++fails; } } while (0)
int main() {
    EXPECT(noteName(60) == "C4"); EXPECT(noteName(61) == "C#4"); EXPECT(noteName(0) == "C-1"); EXPECT(noteName(127) == "G9"); EXPECT(noteName(128) == "?");
    { auto d = decodeMidi1(0x90, 60, 100); EXPECT(d.kind == Kind::Note && d.text == "ch01 NoteOn  C4 v100"); }
    { auto d = decodeMidi1(0x93, 60, 0); EXPECT(d.text.find("NoteOff") != std::string::npos); }
    { auto d = decodeMidi1(0xB1, 7, 99); EXPECT(d.kind == Kind::CC && d.text == "ch02 CC7   = 99"); }
    { auto d = decodeMidi1(0xE0, 0, 64); EXPECT(d.text == "ch01 Bend +0"); }
    { auto d = decodeMidi1(0xF8, 0, 0); EXPECT(d.sys == 0xF8 && !d.log); }
    { auto d = decodeMidi1(0xFA, 0, 0); EXPECT(d.sys == 0xFA && d.log && d.text == "Start"); }
    { uint32_t w[4] = {0x40903C00, 0xC0000000, 0, 0}; auto d = decodeUmp(w); EXPECT(d.m2 && d.kind == Kind::Note && d.text == "ch01 NoteOn  C4 v49152"); EXPECT(umpWordCount(w[0]) == 2); }
    { uint32_t w[4] = {0x40913C00 | 1, 0x80000012, 0, 0}; auto d = decodeUmp(w); EXPECT(d.m2 && d.text.find("a1:0012") != std::string::npos); }
    { uint32_t w[4] = {0x40B14A00, 0x80000000, 0, 0}; auto d = decodeUmp(w); EXPECT(d.m2 && d.kind == Kind::CC && d.text == "ch02 CC74  80000000 (64)"); }
    { uint32_t w[4] = {0x40E00000, 0x80000000, 0, 0}; auto d = decodeUmp(w); EXPECT(d.text == "ch01 Bend 80000000 (+0)"); }
    { uint32_t w[4] = {0x40C10001, 0x0C000102, 0, 0}; auto d = decodeUmp(w); EXPECT(d.kind == Kind::Program && d.text == "ch02 Program 12 bank 1:2"); }
    { uint32_t w[4] = {0x20B10764, 0, 0, 0}; auto d = decodeUmp(w); EXPECT(!d.m2 && d.text == "ch02 CC7   = 100"); EXPECT(umpWordCount(w[0]) == 1); }
    { uint32_t w[4] = {0x10F80000, 0, 0, 0}; auto d = decodeUmp(w); EXPECT(d.sys == 0xF8); }
    { uint32_t w[4] = {0x30067E7F, 0x06010000, 0, 0}; auto d = decodeUmp(w); EXPECT(d.kind == Kind::SysEx && d.text == "SysEx7 complete 6B 7E 7F 06 01 00.."); }
    { uint32_t w[4] = {0xF0010101, 0, 0, 0}; auto d = decodeUmp(w); EXPECT(d.text == "UMP stream: EndpointInfo"); EXPECT(umpWordCount(w[0]) == 4); }
    { uint32_t w[4] = {0x71000000, 0, 0, 0}; auto d = decodeUmp(w); EXPECT(d.text.size() > 0); }
    { ClockEst c; long long t = 1000; for (int i = 0; i < 40; ++i) { c.tick(t); t += 20833; } double b = c.bpm(t); EXPECT(b > 119.5 && b < 120.5); EXPECT(c.bpm(t + 2000000) == 0.0); }
    if (!fails) puts("ump_test: all passed");
    return fails ? 1 : 0;
}
