// SPDX-License-Identifier: MIT
// midi-scope-testsrc: a synthetic MIDI source for testing MIDI Scope without hardware.
//   midi-scope-testsrc m1 [seconds]   a classic (MIDI 1.0) sequencer client "Test M1"
//   midi-scope-testsrc m2 [seconds]   a UMP client with midi_version 2 "Test M2": MIDI 2.0 channel voice packets, SysEx7, UMP stream
// It creates one readable port and sends to whoever is subscribed; the subscription is made by the other side
// (MIDI Scope connects by itself when the port appears).
#include <alsa/asoundlib.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static snd_seq_t* seq; static int port;
static void legacy(snd_seq_event_t& ev) { snd_seq_ev_set_source(&ev, port); snd_seq_ev_set_subs(&ev); snd_seq_ev_set_direct(&ev); snd_seq_event_output_direct(seq, &ev); }
static void ump(uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    snd_seq_ump_event_t ev; memset(&ev, 0, sizeof ev); ev.type = SND_SEQ_EVENT_NONE; ev.flags = SND_SEQ_EVENT_UMP;
    ev.ump[0] = a; ev.ump[1] = b; ev.ump[2] = c; ev.ump[3] = d; ev.source.port = port; ev.dest.client = SND_SEQ_ADDRESS_SUBSCRIBERS; ev.dest.port = SND_SEQ_ADDRESS_UNKNOWN; ev.queue = SND_SEQ_QUEUE_DIRECT;
    snd_seq_ump_event_output_direct(seq, &ev);
}
int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "m1"; int secs = argc > 2 ? atoi(argv[2]) : 20;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { fprintf(stderr, "no sequencer\n"); return 1; }
    snd_seq_set_client_name(seq, mode == "m2" ? "Test M2" : "Test M1");
    if (mode == "m2") { snd_seq_client_info_t* ci; snd_seq_client_info_alloca(&ci); snd_seq_get_client_info(seq, ci); snd_seq_client_info_set_midi_version(ci, SND_SEQ_CLIENT_UMP_MIDI_2_0); if (snd_seq_set_client_info(seq, ci) < 0) { fprintf(stderr, "cannot set UMP\n"); return 1; } }
    port = snd_seq_create_simple_port(seq, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    fprintf(stderr, "%s: client %d port %d\n", mode.c_str(), snd_seq_client_id(seq), port);
    sleep(2);   // let the monitor find us and connect
    for (int i = 0; i < secs; ++i) {
        int n = 60 + (i % 12);
        if (mode == "m2") {
            ump(0x40900000u | (n << 8), 0xC0000000u | (i & 0xFFFF));           // Note On ch1, velocity 0xC000
            ump(0x40B00000u | (74 << 8), 0x80000000u + i * 0x01000000u);       // CC74 32 bit
            ump(0x40E00000u, 0xA0000000u);                                      // pitch bend
            if (i % 4 == 0) ump(0x40C10001u, 0x0C000102u);                      // program 12, bank 1:2
            if (i % 5 == 0) ump(0x30067E7Fu, 0x06010000u);                      // SysEx7 complete, 6 bytes
            if (i == 0) ump(0xF0010101u, 0, 0, 0);                              // UMP stream: endpoint info
            ump(0x20B10700u | 100 << 0);                                        // MIDI 1.0 channel voice inside UMP (mt 2): ch2 CC7 = 100
            usleep(100000); ump(0x40800000u | (n << 8), 0);
        } else {
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev); snd_seq_ev_set_noteon(&ev, 0, n, 100); legacy(ev);
            snd_seq_ev_clear(&ev); snd_seq_ev_set_controller(&ev, 0, 1, (i * 8) & 127); legacy(ev);
            snd_seq_ev_clear(&ev); ev.type = SND_SEQ_EVENT_PITCHBEND; ev.data.control.channel = 0; ev.data.control.value = (i % 3 - 1) * 2000; legacy(ev);
            if (i % 4 == 0) { snd_seq_ev_clear(&ev); ev.type = SND_SEQ_EVENT_PGMCHANGE; ev.data.control.channel = 0; ev.data.control.value = i & 127; legacy(ev); }
            if (i % 5 == 0) { static unsigned char sx[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}; snd_seq_ev_clear(&ev); snd_seq_ev_set_sysex(&ev, sizeof sx, sx); legacy(ev); }
            if (i == 0) { snd_seq_ev_clear(&ev); ev.type = SND_SEQ_EVENT_START; legacy(ev); }
            // 120 BPM clock for the remainder of this second (48 ticks at 20.8 ms)
            for (int k = 0; k < 24; ++k) { snd_seq_ev_clear(&ev); ev.type = SND_SEQ_EVENT_CLOCK; legacy(ev); usleep(20833); if (k == 3) { snd_seq_ev_clear(&ev); snd_seq_ev_set_noteoff(&ev, 0, n, 0); legacy(ev); } }
            continue;
        }
        usleep(900000);
    }
    return 0;
}
