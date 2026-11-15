/* An ALSA sequencer output port of our own, for MIDI generators on MPC OS (MPC ignores a plugin's VST MIDI out).
 * libasound is dlopen()ed (MPC already has it loaded) and the few seq structures are declared here (stable ABI), so the build
 * needs no ALSA headers. Also: an echo filter, since with record arm MPC loops our own port back in as the track's MIDI.
 * Build with -DMIDIPORT_TEST to send to midiport_test_sink() instead (offline tests). */
#ifndef MIDIPORT_H
#define MIDIPORT_H
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

typedef struct { unsigned char client, port; } aseq_addr_t;
typedef struct { unsigned char channel, note, velocity, off_velocity; unsigned int duration; } aseq_note_t;
typedef struct { unsigned char channel, unused[3]; unsigned int param; signed int value; } aseq_ctrl_t;
typedef struct {
    unsigned char type, flags, tag, queue;
    union { unsigned int tick; struct { unsigned int tv_sec, tv_nsec; } time; } time;
    aseq_addr_t source, dest;
    union { aseq_note_t note; aseq_ctrl_t control; unsigned char raw8[12]; unsigned int raw32[3]; } data;
} aseq_event_t;   /* == snd_seq_event_t, 28 bytes on 32- and 64-bit */
typedef char aseq_size_check[sizeof(aseq_event_t) == 28 ? 1 : -1];
enum { ASEQ_NOTEON = 6, ASEQ_NOTEOFF = 7, ASEQ_CONTROLLER = 10, ASEQ_QUEUE_DIRECT = 253, ASEQ_ADDR_SUBSCRIBERS = 254, ASEQ_ADDR_UNKNOWN = 253 };
enum { ASEQ_OPEN_OUTPUT = 1, ASEQ_NONBLOCK = 1 };
#define ASEQ_CAP_READ (1u << 0)
#define ASEQ_CAP_SUBS_READ (1u << 5)
#define ASEQ_TYPE_MIDI_GENERIC (1u << 1)
#define ASEQ_TYPE_APPLICATION (1u << 20)

typedef struct {
    void *lib, *seq;
    int port;
    int (*open)(void **, const char *, int, int);
    int (*set_name)(void *, const char *);
    int (*create_port)(void *, const char *, unsigned, unsigned);
    int (*output_direct)(void *, aseq_event_t *);
    int (*close)(void *);
    struct { uint8_t b[3]; uint32_t blk; } sent[64];   /* echo filter */
    int sent_w;
    uint32_t blk;                                      /* block counter, starts at 1 (0 = empty slot) */
    void *owner;
} midiport_t;

#ifdef MIDIPORT_TEST
void (*midiport_test_sink)(void *owner, int status, int d1, int d2) = NULL;
#endif

static void midiport_open(midiport_t *a, void *owner, const char *client, const char *port) {
    memset(a, 0, sizeof *a);
    a->port = -1;
    a->blk = 1;
    a->owner = owner;
#ifndef MIDIPORT_TEST
    a->lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!a->lib) return;
    a->open = (int (*)(void **, const char *, int, int))dlsym(a->lib, "snd_seq_open");
    a->set_name = (int (*)(void *, const char *))dlsym(a->lib, "snd_seq_set_client_name");
    a->create_port = (int (*)(void *, const char *, unsigned, unsigned))dlsym(a->lib, "snd_seq_create_simple_port");
    a->output_direct = (int (*)(void *, aseq_event_t *))dlsym(a->lib, "snd_seq_event_output_direct");
    a->close = (int (*)(void *))dlsym(a->lib, "snd_seq_close");
    if (!a->open || !a->set_name || !a->create_port || !a->output_direct || !a->close) return;
    if (a->open(&a->seq, "default", ASEQ_OPEN_OUTPUT, ASEQ_NONBLOCK) < 0) { a->seq = NULL; return; }
    a->set_name(a->seq, client);
    a->port = a->create_port(a->seq, port, ASEQ_CAP_READ | ASEQ_CAP_SUBS_READ, ASEQ_TYPE_MIDI_GENERIC | ASEQ_TYPE_APPLICATION);
#else
    (void)client; (void)port;
#endif
}

static void midiport_close(midiport_t *a) {
#ifndef MIDIPORT_TEST
    if (a->seq && a->close) a->close(a->seq);   /* libasound stays loaded: MPC itself uses it */
#endif
    a->seq = NULL;
}

static void midiport_send(midiport_t *a, int st, int d1, int d2) {
    a->sent[a->sent_w].b[0] = (uint8_t)st; a->sent[a->sent_w].b[1] = (uint8_t)d1; a->sent[a->sent_w].b[2] = (uint8_t)d2;
    a->sent[a->sent_w].blk = a->blk;
    a->sent_w = (a->sent_w + 1) & 63;
#ifdef MIDIPORT_TEST
    if (midiport_test_sink) midiport_test_sink(a->owner, st, d1, d2);
#else
    if (!a->seq || a->port < 0) return;
    aseq_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.queue = ASEQ_QUEUE_DIRECT;
    ev.source.port = (unsigned char)a->port;
    ev.dest.client = ASEQ_ADDR_SUBSCRIBERS;
    ev.dest.port = ASEQ_ADDR_UNKNOWN;
    if ((st & 0xf0) == 0xb0) {
        ev.type = ASEQ_CONTROLLER;
        ev.data.control.channel = (unsigned char)(st & 15);
        ev.data.control.param = (unsigned)d1;
        ev.data.control.value = d2;
    } else {
        ev.type = ((st & 0xf0) == 0x90 && d2) ? ASEQ_NOTEON : ASEQ_NOTEOFF;
        ev.data.note.channel = (unsigned char)(st & 15);
        ev.data.note.note = (unsigned char)d1;
        ev.data.note.velocity = (unsigned char)(ev.type == ASEQ_NOTEON ? d2 : 0);
    }
    a->output_direct(a->seq, &ev);
#endif
}

/* a note we sent less than 64 blocks ago, coming back: drop it (byte match; a note-off matches either form) */
static int midiport_was_just_sent(midiport_t *a, const uint8_t *m) {
    for (int i = 0; i < 64; i++) {
        if (a->sent[i].blk == 0 || a->blk - a->sent[i].blk > 64) continue;
        int st = a->sent[i].b[0], d2 = a->sent[i].b[2], mst = m[0], md2 = m[2];
        int off = ((st & 0xf0) == 0x80) || ((st & 0xf0) == 0x90 && d2 == 0);
        int moff = ((mst & 0xf0) == 0x80) || ((mst & 0xf0) == 0x90 && md2 == 0);
        if ((st & 15) != (mst & 15) || a->sent[i].b[1] != m[1]) continue;
        if (off != moff || (!off && d2 != md2)) continue;
        a->sent[i].blk = 0;
        return 1;
    }
    return 0;
}
#endif
