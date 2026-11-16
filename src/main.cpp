// SPDX-License-Identifier: MIT
// MIDI Scope for the Cardputer Zero (320x170 LCD, keyboard only).
// Shows MIDI ports (USB host, USB gadget, virtual, RTP-MIDI/network, MIDI 2.0 UMP) and a live event log, and
// sends a few test messages. Everything is an ALSA sequencer client: subscriptions stay up (snd_seq_connect_from/to),
// hot-plug is followed through the System:Announce port (0:1).
//
//   midi-scope                  run on the LCD (/dev/fb0) with the evdev keyboard
//   midi-scope --ascii          draw one frame as text on stdout and exit (layout check without an image viewer)
//   --demo                      use built-in synthetic ports and events (no ALSA needed)
//   --keys up,enter,tab,...     key presses fed in at start (--ascii and normal mode)
//   --ms N                      with --ascii: how long to listen before printing (default 1500)
//   --ppm FILE                  with --ascii: also write the frame as a PPM image
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "font8x8_basic.h"
#include "ump.hpp"

using namespace msc;

namespace {
constexpr int W = 320, H = 170, COLS = 40, ROWS = 17, CW = 8, CH = 10;
constexpr int PORT_ROWS = 5, LOG_ROWS = 9;

long long usNow() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }

// ---- screen: a 40x17 grid of cells, each 8x10 pixels -----------------------------------------------
enum Col { C_BG, C_FG, C_DIM, C_GRN, C_YEL, C_CYN, C_MAG, C_RED, C_TIT, C_SEL, C_ORG, C_N };
struct Rgb { uint8_t r, g, b; };
const Rgb PAL[C_N] = {{8, 8, 12}, {225, 225, 225}, {125, 125, 150}, {110, 240, 130}, {255, 220, 60}, {90, 210, 255},
                      {240, 120, 255}, {255, 90, 80}, {30, 30, 70}, {55, 55, 120}, {255, 160, 50}};
struct Cell { char ch = ' '; uint8_t fg = C_FG, bg = C_BG; };
struct Screen {
    Cell c[ROWS][COLS];
    void clear() { for (auto& r : c) for (auto& x : r) x = Cell(); }
    void fillRow(int r, int bg) { if (r < 0 || r >= ROWS) return; for (auto& x : c[r]) { x.ch = ' '; x.bg = bg; } }
    void put(int r, int col, const std::string& s, int fg, int bg = -1) {
        if (r < 0 || r >= ROWS) return;
        for (size_t i = 0; i < s.size() && col + (int)i < COLS; ++i) {
            if (col + (int)i < 0) continue; Cell& x = c[r][col + i]; x.ch = (s[i] >= 32 && s[i] < 127) ? s[i] : '?'; x.fg = fg; if (bg >= 0) x.bg = bg; }
    }
    void putR(int r, const std::string& s, int fg, int bg = -1) { put(r, COLS - (int)s.size(), s, fg, bg); }   // right aligned
    void render(uint32_t* px, const uint32_t* pal) const {
        for (int r = 0; r < ROWS; ++r) for (int k = 0; k < COLS; ++k) {
            const Cell& x = c[r][k]; const uint8_t* g = (const uint8_t*)font8x8_basic[(unsigned char)x.ch];
            for (int fy = 0; fy < CH; ++fy) for (int fx = 0; fx < CW; ++fx) {
                bool on = fy >= 1 && fy <= 8 && (g[fy - 1] & (1u << fx));
                px[(r * CH + fy) * W + k * CW + fx] = pal[on ? x.fg : x.bg]; }
        }
    }
    void ascii(FILE* f) const {
        fputs("+", f); for (int i = 0; i < COLS; ++i) fputc('-', f); fputs("+\n", f);
        for (int r = 0; r < ROWS; ++r) { fputc('|', f); for (int k = 0; k < COLS; ++k) fputc(c[r][k].ch, f); fputs("|\n", f); }
        fputs("+", f); for (int i = 0; i < COLS; ++i) fputc('-', f); fputs("+\n", f);
    }
};

// ---- the LCD (see docs/DEVICE-NOTES.md: mmap /dev/fb0, RGB565) ---------------------------------------
struct Lcd {
    int fd = -1; uint8_t* mem = nullptr; size_t size = 0; fb_var_screeninfo v{}; fb_fix_screeninfo f{};
    bool open(const char* path) {
        fd = ::open(path, O_RDWR); if (fd < 0) return false;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) return false;
        if (v.bits_per_pixel != 16 && v.bits_per_pixel != 32) return false;
        size = f.smem_len ? f.smem_len : (size_t)f.line_length * v.yres_virtual;
        void* m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) return false; mem = (uint8_t*)m; return true;
    }
    uint32_t pack(int r, int g, int b) const {
        auto ch = [](const fb_bitfield& bf, int x) -> uint32_t { return bf.length ? ((uint32_t)x >> (8 - std::min<unsigned>(bf.length, 8))) << bf.offset : 0u; };
        return ch(v.red, r) | ch(v.green, g) | ch(v.blue, b);
    }
    void row(int y, const uint32_t* px) {
        size_t bytes = v.bits_per_pixel / 8;
        uint8_t* d = mem + (size_t)(v.yoffset + y) * f.line_length + (size_t)v.xoffset * bytes;
        if (bytes == 2) { auto* o = (uint16_t*)d; for (int x = 0; x < W; ++x) o[x] = (uint16_t)px[x]; }
        else std::memcpy(d, px, W * 4);
    }
};

// ---- keyboard (evdev) --------------------------------------------------------------------------------
bool hasKey(int fd, int code) { unsigned char b[KEY_MAX / 8 + 1] = {0}; if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof b), b) < 0) return false; return (b[code / 8] >> (code % 8)) & 1; }
int openKeyboard() {
    int fd = ::open("/dev/input/by-path/platform-3f804000.i2c-event", O_RDONLY | O_NONBLOCK); if (fd >= 0) return fd;
    for (int i = 0; i < 32; ++i) { std::string p = "/dev/input/event" + std::to_string(i); fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
        if (hasKey(fd, KEY_A) && hasKey(fd, KEY_Z) && hasKey(fd, KEY_SPACE)) return fd; ::close(fd); }
    return -1;
}
int keyByName(const std::string& n) {
    static const std::map<std::string, int> m = {{"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"tab", KEY_TAB}, {"enter", KEY_ENTER},
        {"space", KEY_SPACE}, {"pgup", KEY_PAGEUP}, {"pgdn", KEY_PAGEDOWN}, {"plus", KEY_EQUAL}, {"minus", KEY_MINUS},
        {"a", KEY_A}, {"b", KEY_B}, {"c", KEY_C}, {"d", KEY_D}, {"e", KEY_E}, {"f", KEY_F}, {"g", KEY_G}, {"h", KEY_H}, {"i", KEY_I}, {"j", KEY_J}, {"k", KEY_K},
        {"l", KEY_L}, {"m", KEY_M}, {"n", KEY_N}, {"o", KEY_O}, {"p", KEY_P}, {"q", KEY_Q}, {"r", KEY_R}, {"s", KEY_S}, {"t", KEY_T}, {"u", KEY_U}, {"v", KEY_V},
        {"w", KEY_W}, {"x", KEY_X}, {"y", KEY_Y}, {"z", KEY_Z}};
    auto it = m.find(n); return it == m.end() ? -1 : it->second;
}

// ---- model --------------------------------------------------------------------------------------------
enum class PKind { USB, GDG, VRT, NET, APP, HW };
const char* kindTag(PKind k) { switch (k) { case PKind::USB: return "USB"; case PKind::GDG: return "GDG"; case PKind::VRT: return "VRT"; case PKind::NET: return "NET"; case PKind::HW: return "HW "; default: return "APP"; } }
const char* kindLong(PKind k) { switch (k) { case PKind::USB: return "USB host (class-compliant device)"; case PKind::GDG: return "USB gadget (Zero is the device)"; case PKind::VRT: return "virtual (kernel)"; case PKind::NET: return "network (RTP-MIDI)"; case PKind::HW: return "other hardware"; default: return "application"; } }

struct Port {
    int client = 0, port = 0; std::string cname, pname; PKind kind = PKind::APP; int ver = 0; unsigned caps = 0; int card = -1; std::string cardId;
    bool recv = false, send = false, muted = false, seenM1 = false, seenM2 = false; unsigned long long count = 0; long long lastAct = 0; ClockEst clk;
    std::string label() const {
        std::string p = pname; while (!p.empty() && p.back() == ' ') p.pop_back();
        std::string c = cname;
        if (p.compare(0, c.size(), c) == 0) return p;
        return c + " " + p;
    }
    bool same(int c, int p) const { return client == c && port == p; }
};
const char* verTag(int v) { return v >= 2 ? "M2.0" : v == 1 ? "U1.0" : "M1.0"; }
int verCol(int v) { return v >= 2 ? C_MAG : C_CYN; }
const char* verLong(int v) { return v >= 2 ? "MIDI 2.0 (UMP)" : v == 1 ? "UMP, MIDI 1.0 protocol" : "MIDI 1.0"; }

struct LogLine { int client, port; char tag; int tagCol; std::string text; int col; };

struct App {
    // ALSA
    snd_seq_t* seq = nullptr; int me = -1, inPort = -1, outPort = -1; bool umpOk = false; std::string umpNote;
    // model
    std::vector<Port> ports; std::deque<LogLine> log; std::deque<std::string> txlog;
    int sel = 0, view = 0; bool filterSel = false, paused = false; int logOff = 0; unsigned long long total = 0;
    bool needRescan = true, dirty = true, demo = false, running = false; double lastBpm = 0, shownBpm = 0; int bpmPort = -1;
    // sending
    int outSel = 0, txCh = 0, txNote = 60, txCc = 1, txVal = 0; double txBpm = 120; bool txClock = false, txUmp = false; long long nextTick = 0;
    int connClient = -1, connPort = -1; std::vector<std::pair<long long, int>> offs;   // (due us, note) note-offs to send
    std::vector<Port> outs; std::string status;

    // ---------- ALSA setup ----------
    bool openSeq() {
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) { seq = nullptr; return false; }
        snd_seq_set_client_name(seq, "MIDI Scope");
        snd_seq_client_info_t* ci; snd_seq_client_info_alloca(&ci); snd_seq_get_client_info(seq, ci);
        snd_seq_client_info_set_midi_version(ci, SND_SEQ_CLIENT_UMP_MIDI_2_0);   // we speak UMP: MIDI 2.0 events arrive as UMP packets
        snd_seq_client_info_set_ump_conversion(ci, 0);                           // no kernel conversion: every event arrives in its own form
        umpOk = snd_seq_set_client_info(seq, ci) == 0;
        snd_seq_get_client_info(seq, ci); me = snd_seq_client_info_get_client(ci);
        umpOk = umpOk && snd_seq_client_info_get_midi_version(ci) == SND_SEQ_CLIENT_UMP_MIDI_2_0;
        umpNote = umpOk ? "client is UMP MIDI 2.0" : "UMP not available: MIDI 2.0 shown as MIDI 1.0";
        inPort = snd_seq_create_simple_port(seq, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        outPort = snd_seq_create_simple_port(seq, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (inPort < 0 || outPort < 0) return false;
        snd_seq_connect_from(seq, inPort, 0, 1);    // System:Announce -> hot-plug notices
        return true;
    }
    static PKind classify(const snd_seq_client_info_t* ci, int card, std::string& cardId, const std::string& name) {
        auto lower = [](std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; };
        std::string n = lower(name);
        if (n.find("rtp") != std::string::npos || n.find("network") != std::string::npos) return PKind::NET;
        if (snd_seq_client_info_get_type(ci) == SND_SEQ_KERNEL_CLIENT) {
            if (card >= 0) {
                char p[96]; snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
                if (FILE* f = fopen(p, "r")) { char b[64] = {0}; if (fgets(b, sizeof b, f)) { cardId = b; while (!cardId.empty() && (cardId.back() == '\n' || cardId.back() == ' ')) cardId.pop_back(); } fclose(f); }
                if (lower(cardId).compare(0, 6, "f_midi") == 0 || lower(cardId).find("gadget") != std::string::npos) return PKind::GDG;
                snprintf(p, sizeof p, "/proc/asound/card%d/usbid", card);
                return access(p, R_OK) == 0 ? PKind::USB : PKind::HW;
            }
            return PKind::VRT;
        }
        return PKind::APP;
    }
    void rescan() {
        needRescan = false; if (!seq) return;
        std::vector<Port> np; std::vector<Port> no;
        snd_seq_client_info_t* ci; snd_seq_client_info_alloca(&ci); snd_seq_client_info_set_client(ci, -1);
        while (snd_seq_query_next_client(seq, ci) >= 0) {
            int c = snd_seq_client_info_get_client(ci); if (c == 0 || c == me) continue;
            int card = snd_seq_client_info_get_card(ci), ver = snd_seq_client_info_get_midi_version(ci); std::string cname = snd_seq_client_info_get_name(ci), cardId;
            PKind k = classify(ci, card, cardId, cname);
            snd_seq_port_info_t* pi; snd_seq_port_info_alloca(&pi); snd_seq_port_info_set_client(pi, c); snd_seq_port_info_set_port(pi, -1);
            while (snd_seq_query_next_port(seq, pi) >= 0) {
                unsigned cap = snd_seq_port_info_get_capability(pi); if (cap & SND_SEQ_PORT_CAP_NO_EXPORT) continue;
                Port p; p.client = c; p.port = snd_seq_port_info_get_port(pi); p.cname = cname; p.pname = snd_seq_port_info_get_name(pi); p.kind = k; p.ver = ver; p.caps = cap; p.card = card; p.cardId = cardId;
                p.recv = (cap & SND_SEQ_PORT_CAP_READ) && (cap & SND_SEQ_PORT_CAP_SUBS_READ);
                p.send = (cap & SND_SEQ_PORT_CAP_WRITE) && (cap & SND_SEQ_PORT_CAP_SUBS_WRITE);
                if (p.recv) { for (auto& o : ports) if (o.same(c, p.port)) { p.muted = o.muted; p.seenM1 = o.seenM1; p.seenM2 = o.seenM2; p.count = o.count; p.lastAct = o.lastAct; p.clk = o.clk; }
                              np.push_back(p); }
                if (p.send) no.push_back(p);
            }
        }
        // the selected port stays selected when it is still there
        int sc = sel < (int)ports.size() ? ports[sel].client : -1, sp = sel < (int)ports.size() ? ports[sel].port : -1;
        ports.swap(np); outs.swap(no);
        sel = 0; for (size_t i = 0; i < ports.size(); ++i) if (ports[i].same(sc, sp)) sel = (int)i;
        if (outSel >= (int)outs.size()) outSel = std::max(0, (int)outs.size() - 1);
        for (auto& p : ports) if (!p.muted) snd_seq_connect_from(seq, inPort, p.client, p.port);   // already connected: harmless error
        dirty = true;
    }
    Port* find(int c, int p) { for (auto& x : ports) if (x.same(c, p)) return &x; return nullptr; }

    // ---------- events ----------
    void addLog(int c, int p, char tag, int tagCol, const std::string& text, int col) {
        if (paused) return;
        log.push_back({c, p, tag, tagCol, text, col}); if (log.size() > 400) log.pop_front();
        if (logOff > 0) logOff = std::min<int>(logOff + 1, (int)log.size());
        dirty = true;
    }
    static int kindCol(Kind k) { switch (k) { case Kind::Note: return C_GRN; case Kind::CC: return C_YEL; case Kind::Transport: return C_RED; case Kind::SysEx: return C_ORG; case Kind::Other: return C_DIM; default: return C_FG; } }
    // legacy = bytes arrived as a classic sequencer event (MIDI 1.0); otherwise it is a UMP packet
    void handle(int c, int p, const Decoded& d, char tag, int tagCol, bool m2) {
        long long t = usNow(); Port* P = find(c, p); ++total;
        if (P) { ++P->count; P->lastAct = t; if (m2) P->seenM2 = true; else P->seenM1 = true; }
        if (d.sys == 0xF8 && P) {
            P->clk.tick(t); double b = P->clk.bpm(t); bpmPort = (int)(P - &ports[0]);
            if (b > 0 && (lastBpm == 0 || std::abs(b - lastBpm) >= 1.0)) { lastBpm = b; char s[40]; snprintf(s, sizeof s, "Clock %.1f BPM", b); addLog(c, p, tag, tagCol, s, C_RED); }
        }
        if (d.sys == 0xFA || d.sys == 0xFB) running = true; else if (d.sys == 0xFC) running = false;
        dirty = true; if (!d.log) return;
        addLog(c, p, tag, tagCol, d.text, kindCol(d.kind));
    }
    static Decoded decodeLegacy(const snd_seq_ump_event_t* ev) {
        const auto& n = ev->data.note; const auto& k = ev->data.control; Decoded r;
        switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON: return decodeMidi1(0x90 | (n.channel & 15), n.note, n.velocity);
            case SND_SEQ_EVENT_NOTEOFF: return decodeMidi1(0x80 | (n.channel & 15), n.note, n.velocity);
            case SND_SEQ_EVENT_KEYPRESS: return decodeMidi1(0xA0 | (n.channel & 15), n.note, n.velocity);
            case SND_SEQ_EVENT_CONTROLLER: return decodeMidi1(0xB0 | (k.channel & 15), k.param & 127, k.value & 127);
            case SND_SEQ_EVENT_PGMCHANGE: return decodeMidi1(0xC0 | (k.channel & 15), k.value & 127, 0);
            case SND_SEQ_EVENT_CHANPRESS: return decodeMidi1(0xD0 | (k.channel & 15), k.value & 127, 0);
            case SND_SEQ_EVENT_PITCHBEND: { int v = std::max(0, std::min(16383, k.value + 8192)); return decodeMidi1(0xE0 | (k.channel & 15), v & 127, v >> 7); }
            case SND_SEQ_EVENT_CLOCK: return decodeMidi1(0xF8, 0, 0);
            case SND_SEQ_EVENT_START: return decodeMidi1(0xFA, 0, 0);
            case SND_SEQ_EVENT_CONTINUE: return decodeMidi1(0xFB, 0, 0);
            case SND_SEQ_EVENT_STOP: return decodeMidi1(0xFC, 0, 0);
            case SND_SEQ_EVENT_SENSING: return decodeMidi1(0xFE, 0, 0);
            case SND_SEQ_EVENT_RESET: return decodeMidi1(0xFF, 0, 0);
            case SND_SEQ_EVENT_TUNE_REQUEST: return decodeMidi1(0xF6, 0, 0);
            case SND_SEQ_EVENT_SONGPOS: return decodeMidi1(0xF2, k.value & 127, (k.value >> 7) & 127);
            case SND_SEQ_EVENT_SONGSEL: return decodeMidi1(0xF3, k.value & 127, 0);
            case SND_SEQ_EVENT_QFRAME: return decodeMidi1(0xF1, k.value & 127, 0);
            case SND_SEQ_EVENT_SYSEX: {
                int len = (int)ev->data.ext.len; const uint8_t* d = (const uint8_t*)ev->data.ext.ptr; char b[96];
                r.kind = Kind::SysEx; snprintf(b, sizeof b, "SysEx %dB %s", len, d && len > 0 ? hexBytes(d, std::min(len, 8), 7).c_str() : "");
                r.text = b; return r; }
            case SND_SEQ_EVENT_CONTROL14: case SND_SEQ_EVENT_NONREGPARAM: case SND_SEQ_EVENT_REGPARAM: {
                char b[64]; r.kind = Kind::CC; snprintf(b, sizeof b, "ch%02d %s %d = %d", (k.channel & 15) + 1, ev->type == SND_SEQ_EVENT_CONTROL14 ? "CC14" : ev->type == SND_SEQ_EVENT_NONREGPARAM ? "NRPN" : "RPN", k.param, k.value);
                r.text = b; return r; }
            default: { char b[48]; r.kind = Kind::Other; r.log = false; snprintf(b, sizeof b, "event type %d", ev->type); r.text = b; return r; }
        }
    }
    void pump() {
        if (!seq) return; snd_seq_ump_event_t* ev;
        for (;;) {
            int r = snd_seq_ump_event_input(seq, &ev);
            if (r == -ENOSPC) { addLog(0, 0, '!', C_RED, "input overrun: events lost", C_RED); continue; }
            if (r < 0 || !ev) break;
            int c = ev->source.client, p = ev->source.port;
            if (c == 0 && p == 1) { switch (ev->type) { case SND_SEQ_EVENT_PORT_START: case SND_SEQ_EVENT_PORT_EXIT: case SND_SEQ_EVENT_PORT_CHANGE:
                case SND_SEQ_EVENT_CLIENT_START: case SND_SEQ_EVENT_CLIENT_EXIT: case SND_SEQ_EVENT_CLIENT_CHANGE: needRescan = true; break; default: break; } continue; }
            if (c == 0) continue;
            if (snd_seq_ev_is_ump(ev)) {
                int mt = ev->ump[0] >> 28; Decoded d = decodeUmp(ev->ump);
                handle(c, p, d, d.m2 ? '2' : 'U', d.m2 ? C_MAG : C_CYN, mt == 4);
            } else {
                Decoded d = decodeLegacy(ev); handle(c, p, d, '1', C_CYN, false);
            }
        }
    }

    // ---------- test messages ----------
    void connectOut() {
        if (!seq || outs.empty()) return; const Port& o = outs[outSel];
        if (connClient == o.client && connPort == o.port) return;
        if (connClient >= 0) snd_seq_disconnect_to(seq, outPort, connClient, connPort);
        snd_seq_connect_to(seq, outPort, o.client, o.port); connClient = o.client; connPort = o.port;
    }
    void sendLegacy(snd_seq_event_t& ev) { snd_seq_ev_set_source(&ev, outPort); snd_seq_ev_set_subs(&ev); snd_seq_ev_set_direct(&ev); snd_seq_event_output_direct(seq, &ev); }
    void sendUmp(uint32_t w0, uint32_t w1) {
        snd_seq_ump_event_t ev; memset(&ev, 0, sizeof ev); ev.type = SND_SEQ_EVENT_NONE; ev.flags = SND_SEQ_EVENT_UMP;
        ev.ump[0] = w0; ev.ump[1] = w1; ev.source.port = outPort; ev.dest.client = SND_SEQ_ADDRESS_SUBSCRIBERS; ev.dest.port = SND_SEQ_ADDRESS_UNKNOWN; ev.queue = SND_SEQ_QUEUE_DIRECT;
        snd_seq_ump_event_output_direct(seq, &ev);
    }
    void txLine(const std::string& s) { txlog.push_back(s); if (txlog.size() > 8) txlog.pop_front(); dirty = true; }
    bool txReady() { if (demo) { txLine("(demo: nothing sent)"); return false; } if (!seq || outs.empty()) { txLine("no output port"); return false; } connectOut(); return true; }
    void sendNote() {
        if (!txReady()) return; int ch = txCh, n = txNote;
        if (txUmp) { sendUmp(0x40900000u | (ch << 16) | (n << 8), 0xC0000000u); }
        else { snd_seq_event_t ev; snd_seq_ev_clear(&ev); snd_seq_ev_set_noteon(&ev, ch, n, 100); sendLegacy(ev); }
        offs.push_back({usNow() + 200000, n}); char s[48]; snprintf(s, sizeof s, "TX %s NoteOn ch%d %s", txUmp ? "UMP" : "MIDI1", ch + 1, noteName(n).c_str()); txLine(s);
    }
    void sendNoteOff(int n) {
        if (!seq || outs.empty()) return; int ch = txCh;
        if (txUmp) sendUmp(0x40800000u | (ch << 16) | (n << 8), 0);
        else { snd_seq_event_t ev; snd_seq_ev_clear(&ev); snd_seq_ev_set_noteoff(&ev, ch, n, 0); sendLegacy(ev); }
    }
    void sendCc() {
        if (!txReady()) return; txVal = (txVal + 16) & 127; int ch = txCh;
        if (txUmp) sendUmp(0x40B00000u | (ch << 16) | (txCc << 8), (uint32_t)txVal << 25);
        else { snd_seq_event_t ev; snd_seq_ev_clear(&ev); snd_seq_ev_set_controller(&ev, ch, txCc, txVal); sendLegacy(ev); }
        char s[48]; snprintf(s, sizeof s, "TX %s CC%d = %d ch%d", txUmp ? "UMP" : "MIDI1", txCc, txVal, ch + 1); txLine(s);
    }
    void sendClockTick() {
        if (txUmp) sendUmp(0x10F80000u, 0);
        else { snd_seq_event_t ev; snd_seq_ev_clear(&ev); ev.type = SND_SEQ_EVENT_CLOCK; sendLegacy(ev); }
    }
    void sendRealtime(int status) {   // 0xFA start, 0xFC stop
        if (txUmp) sendUmp(0x10000000u | (status << 16), 0);
        else { snd_seq_event_t ev; snd_seq_ev_clear(&ev); ev.type = status == 0xFA ? SND_SEQ_EVENT_START : SND_SEQ_EVENT_STOP; sendLegacy(ev); }
    }
    void toggleClock() {
        if (!txClock) { if (!txReady()) return; txClock = true; sendRealtime(0xFA); nextTick = usNow(); char s[48]; snprintf(s, sizeof s, "TX Start + Clock %.0f BPM", txBpm); txLine(s); }
        else { txClock = false; sendRealtime(0xFC); txLine("TX Stop"); }
    }
    void tick(long long now) {
        for (size_t i = 0; i < offs.size();) { if (now >= offs[i].first) { sendNoteOff(offs[i].second); offs.erase(offs.begin() + i); } else ++i; }
        if (txClock && seq) { long long iv = (long long)(60e6 / (txBpm * 24)); for (int k = 0; k < 4 && now >= nextTick; ++k) { sendClockTick(); nextTick += iv; } if (now >= nextTick) nextTick = now + iv; }
    }

    // ---------- keys ----------
    void key(int code) {
        dirty = true;
        if (code == KEY_TAB) { view = (view + 1) % 3; if (view == 2 && !outs.empty()) connectOut(); return; }
        if (view == 2) {
            switch (code) {
                case KEY_UP: if (outSel > 0) --outSel; break; case KEY_DOWN: if (outSel + 1 < (int)outs.size()) ++outSel; break;
                case KEY_LEFT: txNote = std::max(0, txNote - 1); break; case KEY_RIGHT: txNote = std::min(127, txNote + 1); break;
                case KEY_N: case KEY_ENTER: sendNote(); break; case KEY_C: sendCc(); break; case KEY_L: toggleClock(); break;
                case KEY_U: txUmp = !txUmp; break; case KEY_H: txCh = (txCh + 1) & 15; break;
                case KEY_EQUAL: txBpm = std::min(300.0, txBpm + 5); break; case KEY_MINUS: txBpm = std::max(30.0, txBpm - 5); break;
                case KEY_K: txCc = (txCc + 1) & 127; break;
                default: break;
            }
            if (view == 2 && !outs.empty() && !demo) connectOut();
            return;
        }
        switch (code) {
            case KEY_UP: if (sel > 0) --sel; break; case KEY_DOWN: if (sel + 1 < (int)ports.size()) ++sel; break;
            case KEY_ENTER: if (sel < (int)ports.size() && !demo) { Port& p = ports[sel]; p.muted = !p.muted;
                if (p.muted) snd_seq_disconnect_from(seq, inPort, p.client, p.port); else snd_seq_connect_from(seq, inPort, p.client, p.port); }
                else if (sel < (int)ports.size()) ports[sel].muted = !ports[sel].muted; break;
            case KEY_F: filterSel = !filterSel; logOff = 0; break;
            case KEY_C: log.clear(); logOff = 0; break;
            case KEY_P: paused = !paused; break;
            case KEY_PAGEUP: logOff += 4; break; case KEY_PAGEDOWN: logOff = std::max(0, logOff - 4); break;
            default: break;
        }
    }

    // ---------- drawing ----------
    std::string fit(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + "~"; }
    void draw(Screen& s, long long now) {
        s.clear();
        // title
        s.fillRow(0, C_TIT); s.put(0, 1, "MIDI Scope", C_YEL, C_TIT);
        const char* vn[] = {"MONITOR", "PORT INFO", "SEND TEST"}; s.put(0, 12, std::string("[") + vn[view] + "] Tab", C_FG, C_TIT);
        { double b = 0; if (bpmPort >= 0 && bpmPort < (int)ports.size()) b = ports[bpmPort].clk.bpm(now); if (demo) b = shownBpm;
          char t[32]; if (b > 0) snprintf(t, sizeof t, "%5.1fbpm%s ", b, running ? " >" : ""); else snprintf(t, sizeof t, "--bpm%s ", running ? " >" : "");
          s.putR(0, t, b > 0 ? C_RED : C_DIM, C_TIT); }
        if (view == 0) drawMonitor(s, now); else if (view == 1) drawInfo(s, now); else drawSend(s);
        // footer
        s.fillRow(16, C_TIT);
        const char* foot = view == 0 ? "Up/Dn port Ent mute F filt C clr P hold" : view == 1 ? "Up/Dn port           Tab next view" : "Up/Dn out N note C cc L clk U ump";
        s.put(16, 1, foot, C_DIM, C_TIT);
    }
    void drawPortRow(Screen& s, int r, const Port& p, bool selected, long long now, bool counts) {
        int bg = selected ? C_SEL : C_BG;
        s.fillRow(r, bg);
        s.put(r, 0, selected ? ">" : " ", C_YEL, bg);
        s.put(r, 2, kindTag(p.kind), p.kind == PKind::USB ? C_GRN : p.kind == PKind::NET ? C_ORG : p.kind == PKind::GDG ? C_YEL : C_DIM, bg);
        s.put(r, 6, verTag(p.ver), verCol(p.ver), bg);
        char addr[12]; snprintf(addr, sizeof addr, "%d:%d", p.client, p.port);
        std::string name = fit(p.label(), counts ? 20 : 26);
        s.put(r, 11, name, p.muted ? C_DIM : C_FG, bg);
        if (counts) {
            char c[32]; if (p.count > 99999) snprintf(c, sizeof c, "%lluk", p.count / 1000); else snprintf(c, sizeof c, "%llu", p.count);
            s.putR(r, std::string(c) + (p.muted ? "x" : (now / 1000 - p.lastAct / 1000 < 250 && p.lastAct ? "*" : " ")), p.muted ? C_RED : C_GRN, bg);
        } else s.putR(r, addr, C_DIM, bg);
    }
    void drawMonitor(Screen& s, long long now) {
        if (ports.empty()) { s.put(2, 1, "No MIDI sources yet.", C_YEL); s.put(3, 1, "Plug in a USB MIDI device,", C_DIM); s.put(4, 1, "or connect RTP-MIDI / a gadget.", C_DIM); }
        int first = std::max(0, std::min(sel - PORT_ROWS + 1 > 0 ? sel - PORT_ROWS + 1 : 0, std::max(0, (int)ports.size() - PORT_ROWS)));
        if (sel < first) first = sel;
        for (int i = 0; i < PORT_ROWS && first + i < (int)ports.size(); ++i) drawPortRow(s, 1 + i, ports[first + i], first + i == sel, now, true);
        // log header
        s.fillRow(6, C_TIT);
        char h[48]; snprintf(h, sizeof h, "LOG %s %s%s", filterSel ? "selected" : "all", paused ? "HOLD " : "", logOff ? "scroll" : ""); s.put(6, 1, h, C_FG, C_TIT);
        char n[24]; snprintf(n, sizeof n, "%llu ev ", total); s.putR(6, n, C_DIM, C_TIT);
        std::vector<const LogLine*> v;
        for (auto& l : log) if (!filterSel || (sel < (int)ports.size() && ports[sel].same(l.client, l.port))) v.push_back(&l);
        int off = std::min<int>(logOff, std::max(0, (int)v.size() - 1)); int end = (int)v.size() - off, beg = std::max(0, end - LOG_ROWS);
        for (int i = beg; i < end; ++i) { const LogLine& l = *v[i]; int r = 7 + (i - beg);
            char a[12]; snprintf(a, sizeof a, "%d:%d", l.client, l.port);
            s.put(r, 0, std::string(1, l.tag), l.tagCol); s.put(r, 2, a, C_DIM); s.put(r, 8, fit(l.text, 32), l.col); }
    }
    void drawInfo(Screen& s, long long now) {
        if (sel >= (int)ports.size()) { s.put(2, 1, "No port selected.", C_DIM); return; }
        const Port& p = ports[sel]; char b[96]; int r = 1;
        auto line = [&](const std::string& k, const std::string& v, int col = C_FG) { s.put(r, 0, k, C_DIM); s.put(r, 9, fit(v, 31), col); ++r; };
        line("Name", p.label()); snprintf(b, sizeof b, "%d:%d  (client %s)", p.client, p.port, p.cname.c_str()); line("Address", b);
        line("Source", kindLong(p.kind)); line("Protocol", verLong(p.ver), verCol(p.ver));
        snprintf(b, sizeof b, "%s%s%s%s", p.caps & SND_SEQ_PORT_CAP_READ ? "R " : "", p.caps & SND_SEQ_PORT_CAP_WRITE ? "W " : "", p.caps & SND_SEQ_PORT_CAP_SUBS_READ ? "subsR " : "", p.caps & SND_SEQ_PORT_CAP_SUBS_WRITE ? "subsW" : "");
        line("Caps", b);
        snprintf(b, sizeof b, "%s%s", p.seenM1 ? "MIDI 1.0 bytes " : "", p.seenM2 ? "MIDI 2.0 UMP" : ""); line("Seen", p.seenM1 || p.seenM2 ? b : "(nothing yet)", p.seenM2 ? C_MAG : C_FG);
        snprintf(b, sizeof b, "%llu", p.count); line("Events", b);
        double bpm = p.clk.bpm(now); if (bpm > 0) snprintf(b, sizeof b, "%.1f BPM", bpm); else snprintf(b, sizeof b, "--"); line("Clock", b, bpm > 0 ? C_RED : C_DIM);
        if (p.card >= 0) { snprintf(b, sizeof b, "card %d  %s", p.card, p.cardId.c_str()); line("ALSA", b); }
        line("Input", p.muted ? "muted (disconnected)" : "connected", p.muted ? C_RED : C_GRN);
        ++r; s.put(r++, 0, "This app:", C_DIM); s.put(r++, 1, umpNote, umpOk ? C_GRN : C_YEL);
        s.put(r++, 1, "legacy events are labelled 1, UMP U/2", C_DIM);
        s.put(r++, 1, "tag 2 = MIDI 2.0 channel voice (mt 4)", C_DIM);
    }
    void drawSend(Screen& s) {
        if (outs.empty()) s.put(2, 1, "No output ports.", C_YEL);
        int first = std::max(0, outSel - PORT_ROWS + 1);
        for (int i = 0; i < PORT_ROWS && first + i < (int)outs.size(); ++i) { const Port& p = outs[first + i]; drawPortRow(s, 1 + i, p, first + i == outSel, 0, false); }
        s.fillRow(6, C_TIT); s.put(6, 1, "TEST MESSAGES  (to the output above)", C_FG, C_TIT);
        char b[64];
        snprintf(b, sizeof b, "Format  %s   [U]", txUmp ? "UMP MIDI 2.0 packets" : "MIDI 1.0 events"); s.put(7, 1, b, txUmp ? C_MAG : C_CYN);
        snprintf(b, sizeof b, "Note %s (Left/Right)  ch%d [H]  CC%d [K]", noteName(txNote).c_str(), txCh + 1, txCc); s.put(8, 1, b, C_FG);
        snprintf(b, sizeof b, "Clock  %s  %.0f BPM  [L] on/off  [+/-]", txClock ? "ON " : "off", txBpm); s.put(9, 1, b, txClock ? C_RED : C_FG);
        s.put(10, 1, "N/Enter: note   C: control change", C_DIM);
        for (size_t i = 0; i < txlog.size() && i < 5; ++i) s.put(11 + (int)i, 1, fit(txlog[txlog.size() - 1 - i], 38), C_GRN);
    }

    // ---------- synthetic data (no ALSA) ----------
    void loadDemo() {
        demo = true; umpOk = true; umpNote = "client is UMP MIDI 2.0 (demo)"; auto mk = [&](int c, int p, const char* cn, const char* pn, PKind k, int ver) { Port x; x.client = c; x.port = p; x.cname = cn; x.pname = pn; x.kind = k; x.ver = ver; x.recv = x.send = true;
            x.caps = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE; x.card = k == PKind::USB ? 2 : -1; if (k == PKind::USB) x.cardId = "USB MIDI"; ports.push_back(x); };
        mk(24, 0, "USB MIDI", "USB MIDI Device 1", PKind::USB, 0); mk(129, 0, "Network MIDI", "Network MIDI Session", PKind::NET, 0);
        mk(130, 0, "UMP Test", "UMP Test Group 1", PKind::APP, 2); mk(32, 0, "f_midi", "f_midi MIDI 1", PKind::GDG, 0); mk(14, 0, "Midi Through", "Midi Through Port-0", PKind::VRT, 0);
        outs = ports; sel = 2;
        auto m1 = [&](int pi, uint8_t st, uint8_t a, uint8_t b2) { Decoded d = decodeMidi1(st, a, b2); handle(ports[pi].client, ports[pi].port, d, '1', C_CYN, false); };
        auto u = [&](int pi, uint32_t w0, uint32_t w1) { uint32_t w[4] = {w0, w1, 0, 0}; Decoded d = decodeUmp(w); handle(ports[pi].client, ports[pi].port, d, d.m2 ? '2' : 'U', d.m2 ? C_MAG : C_CYN, (w0 >> 28) == 4); };
        m1(0, 0x90, 60, 100); m1(0, 0xB0, 1, 64); m1(0, 0xE0, 0, 0x50); m1(0, 0x80, 60, 0); m1(1, 0xFA, 0, 0); m1(1, 0xC0, 12, 0);
        u(2, 0x40903C00, 0xC0000000); u(2, 0x40B14A00, 0x80000000); u(2, 0x40E00000, 0xA0000000); u(2, 0x40C10001, 0x0C000102); u(2, 0x30047E7F, 0x06010000); u(2, 0x40803C00, 0x00000000);
        u(2, 0xF0010101, 0); m1(0, 0x90, 64, 90);
        shownBpm = 120.0; running = true; { char s[40]; snprintf(s, sizeof s, "Clock %.1f BPM", 120.0); addLog(129, 0, '1', C_CYN, s, C_RED); }
        ports[1].muted = false; ports[3].muted = true; ports[0].lastAct = usNow();
    }
};

volatile sig_atomic_t gStop = 0;
void onSig(int) { gStop = 1; }
}  // namespace

int main(int argc, char** argv) {
    bool ascii = false, demo = false; int ms = 1500; std::string fbPath = "/dev/fb0", keys, ppm;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ascii") ascii = true; else if (a == "--demo") demo = true;
        else if (a == "--ms" && i + 1 < argc) ms = atoi(argv[++i]); else if (a == "--keys" && i + 1 < argc) keys = argv[++i];
        else if (a == "--fb" && i + 1 < argc) fbPath = argv[++i]; else if (a == "--ppm" && i + 1 < argc) ppm = argv[++i];
        else if (a == "--help") { puts("midi-scope [--ascii] [--demo] [--keys up,enter,tab,...] [--ms N] [--ppm FILE] [--fb DEV]\n"
                                       "Monitor: Up/Down port, Enter mute, F filter, C clear, P hold, PgUp/PgDn scroll. Tab: monitor / port info / send test.\n"
                                       "Send: Up/Down output, N note, C cc, K cc number, H channel, U MIDI1/UMP, L clock, +/- bpm. Esc (hold): quit."); return 0; }
        else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    signal(SIGINT, onSig); signal(SIGTERM, onSig); signal(SIGPIPE, SIG_IGN);

    App app;
    if (demo) app.loadDemo();
    else if (!app.openSeq()) fprintf(stderr, "midi-scope: no ALSA sequencer\n");

    Lcd lcd; bool haveLcd = false; int kbd = -1;
    if (!ascii) {
        haveLcd = lcd.open(fbPath.c_str()); if (!haveLcd) { fprintf(stderr, "midi-scope: cannot open %s (use --ascii)\n", fbPath.c_str()); return 1; }
        kbd = openKeyboard(); if (kbd < 0) fprintf(stderr, "midi-scope: no keyboard found\n");
    }
    uint32_t pal[C_N]; for (int i = 0; i < C_N; ++i) pal[i] = haveLcd ? lcd.pack(PAL[i].r, PAL[i].g, PAL[i].b) : (uint32_t)((PAL[i].r << 16) | (PAL[i].g << 8) | PAL[i].b);

    std::vector<int> script; { size_t p = 0; while (p < keys.size()) { size_t q = keys.find(',', p); if (q == std::string::npos) q = keys.size(); int k = keyByName(keys.substr(p, q - p));
        if (k < 0) { fprintf(stderr, "unknown key '%s'\n", keys.substr(p, q - p).c_str()); return 2; } script.push_back(k); p = q + 1; } }

    Screen scr; std::vector<uint32_t> canvas((size_t)W * H), shown((size_t)W * H, ~0u);
    std::vector<pollfd> pfd; if (app.seq) { int n = snd_seq_poll_descriptors_count(app.seq, POLLIN); pfd.resize(n); snd_seq_poll_descriptors(app.seq, pfd.data(), n, POLLIN); }
    if (kbd >= 0) pfd.push_back({kbd, POLLIN, 0});

    const long long t0 = usNow(); long long lastDraw = 0, escT = 0; bool escDown = false; size_t scriptPos = 0;
    for (;;) {
        long long now = usNow();
        if (gStop) break;
        if (ascii && now - t0 >= (long long)ms * 1000 && scriptPos >= script.size()) break;
        // scripted keys: one every 30 ms after the first 300 ms (so ports have been found)
        if (scriptPos < script.size() && now - t0 >= 300000 + (long long)scriptPos * 30000) app.key(script[scriptPos++]);
        poll(pfd.data(), pfd.size(), app.txClock ? 2 : 20);
        app.pump();
        if (kbd >= 0) {
            input_event ev[32];
            for (;;) { ssize_t n = read(kbd, ev, sizeof ev); if (n <= 0) break;
                for (size_t i = 0; i < (size_t)n / sizeof(input_event); ++i) { if (ev[i].type != EV_KEY) continue; int code = ev[i].code, val = ev[i].value;
                    if (code == KEY_ESC) { escDown = val != 0; if (val == 1) escT = now; continue; }
                    if (val == 1 || val == 2) app.key(code); } }
            if (escDown && now - escT >= 800000) break;
        }
        now = usNow(); app.tick(now);
        if (app.needRescan && app.seq) app.rescan();
        if (!app.dirty && now - lastDraw < 120000) continue;
        if (now - lastDraw < 30000 && !ascii) continue;   // at most about 30 frames a second
        lastDraw = now; app.dirty = false; app.draw(scr, now);
        if (haveLcd) { scr.render(canvas.data(), pal);
            for (int y = 0; y < H; ++y) { const uint32_t* r = &canvas[(size_t)y * W]; uint32_t* o = &shown[(size_t)y * W]; if (std::memcmp(r, o, W * 4)) { lcd.row(y, r); std::memcpy(o, r, W * 4); } } }
    }
    if (ascii) {
        app.draw(scr, usNow()); scr.ascii(stdout);
        if (!ppm.empty()) { std::vector<uint32_t> px((size_t)W * H); scr.render(px.data(), pal); FILE* f = fopen(ppm.c_str(), "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", W, H); for (auto p : px) { fputc((p >> 16) & 255, f); fputc((p >> 8) & 255, f); fputc(p & 255, f); } fclose(f); } }
    }
    if (haveLcd) { for (auto& p : canvas) p = lcd.pack(0, 0, 0); for (int y = 0; y < H; ++y) lcd.row(y, &canvas[(size_t)y * W]); }
    if (app.seq) { for (auto& o : app.offs) app.sendNoteOff(o.second); if (app.txClock) app.sendRealtime(0xFC); snd_seq_close(app.seq); }
    return 0;
}
