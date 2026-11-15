/* Offline tests for PadStep 16 (timing per docs/MIDI_TIMING.md, pads, tiles, slots, state), built with -DMIDIPORT_TEST:
 *   cc -DMIDIPORT_TEST -I<open-mpc>/wrapper -Isrc -fsanitize=address,undefined test/engine_test.c src/padstep_engine.c -lm -ldl */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "engine.h"

extern void (*midiport_test_sink)(void *owner, int status, int d1, int d2);

static const mpc_engine_t *E;
int cap_notes[256], cap_n;
static int ev_st[512], ev_d1[512], ev_d2[512], ev_n;
void ev_sink(void *o, int st, int d1, int d2) { (void)o; if (ev_n < 512) { ev_st[ev_n] = st; ev_d1[ev_n] = d1; ev_d2[ev_n] = d2; ev_n++; } }
static int ev_find(int from, int st, int d1, int d2) { for (int i = from; i < ev_n; i++) if ((ev_st[i] & 0xf0) == st && ev_d1[i] == d1 && (d2 < 0 || ev_d2[i] == d2)) return i; return -1; }
static int mono_held, mono_max, mono_bad;
void mono_sink(void *o, int st, int d1, int d2) { (void)o; (void)d1; if ((st & 0xf0) == 0xb0) return; if ((st & 0xf0) == 0x90 && d2) { mono_held++; if (mono_held > mono_max) mono_max = mono_held; } else { if (mono_held > 0) mono_held--; else mono_bad++; } }
void capture(void *o, int st, int d1, int d2) { (void)o; if ((st & 0xf0) == 0x90 && d2 && cap_n < 256) cap_notes[cap_n++] = d1; }
static int fails, ons, held[16][128], doubles, phase_err, chan_seen = -1;
static double win_lo, win_hi, frames_now, last_on_frames = -1e12, step_frames = 1e12, swing_d;
static int last_note;

#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* the test pattern: step i plays note 48 + i, so the note tells which step played */
static void sink(void *o, int st, int d1, int d2) {
    (void)o;
    int ch = st & 15;
    if ((st & 0xf0) == 0x90 && d2) {
        ons++; held[ch][d1]++; chan_seen = ch; last_note = d1;
        int step = d1 - 48, match = 0;
        double d = 0.25;
        for (long k = (long)floor(win_lo / d) - 1; k / 4.0 < win_hi; k++) {
            if (k < 0) continue;
            double t = k * d + ((k & 1) ? swing_d : 0);
            if (t >= win_lo && t < win_hi && (int)(k % 16) == step) match = 1;
        }
        if (!match) phase_err++;
        if (frames_now - last_on_frames < 0.25 * step_frames) doubles++;
        last_on_frames = frames_now;
    } else if (held[ch][d1] > 0) held[ch][d1]--;
}
static int held_total(void) { int n = 0; for (int c = 0; c < 16; c++) for (int i = 0; i < 128; i++) n += held[c][i]; return n; }
static void reset(void) { ons = doubles = phase_err = 0; memset(held, 0, sizeof held); last_on_frames = -1e12; frames_now = 0; }

static void set(void *s, const char *k, const char *v) { E->set_param(s, k, v); }
static void seti(void *s, const char *k, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); E->set_param(s, k, b); }
static void get(void *s, const char *k, char *b, int n) { E->get_param(s, k, b, n); }


/* MPC + wrapper tile protocol (wrapper/vst2_wrap.c): MPC's Toggle Switch sends the opposite of what it holds; the wrapper
 * drops a set equal to its cache (MPC echoing a push) and keeps the tapped value; every 10 ms it reads "<key>_on" and, when
 * that differs from its cache, pushes it to MPC (which then holds it and echoes it back, dropped). */
typedef struct { const char *key; int holds, cache; } tilesim_t;
static void tile_poll(void *s, tilesim_t *t) {
    char k[24], b[8]; snprintf(k, sizeof k, "%s_on", t->key); E->get_param(s, k, b, sizeof b);
    int on = atoi(b);
    if (t->cache != on + 1) { t->cache = on + 1; t->holds = on; }
}
static void tile_tap(void *s, tilesim_t *t) {
    int v = !t->holds;
    t->holds = v;
    if (t->cache == v + 1) return;
    t->cache = v + 1;
    E->set_param(s, t->key, v ? "1" : "0");
}

static void *fresh(void) {
    void *s = E->create(NULL);
    set(s, "scale", "0");
    for (int i = 0; i < 16; i++) { char k[16]; snprintf(k, sizeof k, "p_%d", i + 1); seti(s, k, 48 + i); }
    return s;
}

static void run(void *s, double ppq, double bpm, int blocks, double ls, double le, int jitter, int lurch) {
    unsigned r = 777;
    double blk = 128 * bpm / (60 * 44100.0), prev_end = -1;
    step_frames = 0.25 * 60 * 44100.0 / bpm;
    for (int b = 0; b < blocks; b++) {
        double j = 0;
        if (jitter) { r = r * 1103515245u + 12345u; j = -0.0009 + ((r >> 8) % 1000) / 1000.0 * 0.0011; }
        double rep = ppq + j;
        if (lurch && b % 37 == 20) rep -= 0.12;
        if (lurch && b % 53 == 40) { ppq += 0.4; rep = ppq + j; }
        if (rep < 0) rep = 0;
        win_lo = (prev_end >= 0 && fabs(prev_end - rep) < 1.0 ? fmin(prev_end, rep) : rep) - 0.0025;
        win_hi = rep + blk + 0.0015;
        frames_now += 128;
        E->song_pos(s, rep, bpm, 1, 128);
        prev_end = (prev_end >= 0 && fabs(prev_end - rep) < 1.0) ? fmax(prev_end, rep + blk) : rep + blk;
        ppq += blk;
        if (le > ls && ppq >= le) ppq = ls + (ppq - le);
    }
}

int main(void) {
    E = mpc_engine();
    midiport_test_sink = sink;
    char b[64];

    /* 1. 50 loops of 4 bars, jitter, unreported wrap */
    void *s = fresh(); reset(); swing_d = 0;
    double blk = 128 * 126 / (60 * 44100.0);
    run(s, 0, 126, (int)(50 * 16 / blk), 0, 16, 1, 0);
    CHECK(abs(ons - 3200) <= 1, "50 loops x 64 steps: %d notes", ons);
    CHECK(doubles == 0 && phase_err == 0, "no double (%d), every step in phase (%d off)", doubles, phase_err);
    E->song_pos(s, 0, 126, 0, 128);
    CHECK(held_total() == 0, "stop: no hanging note (%d)", held_total());
    E->destroy(s);

    /* 2. swing 66 and 75, tempo lurches */
    for (int sw = 66; sw <= 75; sw += 9) {
        s = fresh(); reset(); seti(s, "swing", sw); swing_d = 2 * 0.25 * (sw - 50) / 100.0;
        run(s, 0, 133, 20000, 0, 8, 1, 1);
        E->song_pos(s, 0, 133, 0, 128);
        CHECK(doubles == 0 && phase_err == 0 && held_total() == 0 && ons > 500,
              "swing %d + loops + lurches: %d notes, %d doubles, %d off-phase, %d held", sw, ons, doubles, phase_err, held_total());
        E->destroy(s);
    }
    swing_d = 0;

    /* 3. legato on: never two notes at once, and no hang */
    s = fresh(); reset(); set(s, "legato", "1");
    run(s, 0, 120, 3000, 0, 0, 1, 0);
    int maxheld = held_total();
    E->song_pos(s, 0, 120, 0, 128);
    CHECK(maxheld <= 1 && held_total() == 0, "legato: at most one note sounding (%d), none after stop", maxheld);
    E->destroy(s);

    /* 4. pads: pad 3 toggles step 3; SHIFT + pad 3 = Minor; CHAN + pad 5 = channel 5; transpose mode */
    s = fresh(); reset();
    unsigned char m[3] = { 0x90, 26, 100 };   /* pad 3: pad 1 sends 24 on an MPC One */
    E->midi(s, m, 3);
    get(s, "st_3_on", b, sizeof b);
    CHECK(!strcmp(b, "0"), "pad 3 turns step 3 off (%s)", b);
    set(s, "shift", "1"); E->midi(s, m, 3);
    get(s, "scale", b, sizeof b);
    CHECK(!strcmp(b, "2"), "SHIFT + pad 3 = scale Minor (%s)", b);
    get(s, "st_3_on", b, sizeof b);
    CHECK(!strcmp(b, "0"), "... and leaves the step alone (%s)", b);
    get(s, "shift", b, sizeof b);
    CHECK(!strcmp(b, "0"), "... and SHIFT ends (%s)", b);
    set(s, "chanm", "1"); m[1] = 28; E->midi(s, m, 3);
    get(s, "chan", b, sizeof b);
    CHECK(!strcmp(b, "5"), "CHAN + pad 5 = channel 5 (%s)", b);
    set(s, "pad_mode", "1"); m[1] = 67; E->midi(s, m, 3);
    get(s, "transpose", b, sizeof b);
    CHECK(!strcmp(b, "7"), "Transpose pads: G4 = +7 (%s)", b);
    set(s, "pad_mode", "0"); set(s, "transpose", "0");
    run(s, 0, 120, 200, 0, 0, 0, 0);
    CHECK(chan_seen == 4, "notes go out on channel 5 (status channel %d)", chan_seen + 1);
    E->song_pos(s, 0, 120, 0, 128);
    E->destroy(s);

    /* 4b. LEARN: pads sending 50, 52, 54 ... (any layout); pad 3 is then note 54 */
    s = fresh(); reset();
    set(s, "learn", "1");
    for (int i = 0; i < 16; i++) { unsigned char q[3] = { 0x90, (unsigned char)(50 + 2 * i), 101 }; E->midi(s, q, 3); }
    get(s, "learn", b, sizeof b);
    CHECK(!strcmp(b, "0"), "LEARN ends after pad 16 (%s)", b);
    unsigned char q3[3] = { 0x90, 54, 101 };
    E->midi(s, q3, 3);
    get(s, "st_3_on", b, sizeof b);
    CHECK(!strcmp(b, "0"), "learned pad 3 (note 54) toggles step 3 (%s)", b);
    get(s, "pad_info", b, sizeof b);
    CHECK(!strcmp(b, "note 54 = pad 3"), "LAST PAD says which pad (%s)", b);
    char sa[8192], sb[8192];
    get(s, "state", sa, sizeof sa);
    void *sx = E->create(NULL); set(sx, "state", sa); get(sx, "state", sb, sizeof sb);
    CHECK(!strcmp(sa, sb), "the learned pad map survives a reload");
    E->destroy(sx); E->destroy(s);

    /* 5. step tiles through the MPC + wrapper protocol: every tap toggles */
    s = fresh();
    tilesim_t t1 = { "st_1", 0, 0 };
    tile_poll(s, &t1);
    int toggles_ok = 1;
    for (int r = 0; r < 6; r++) {
        get(s, "st_1_on", b, sizeof b); int before = atoi(b);
        tile_tap(s, &t1); tile_poll(s, &t1);
        get(s, "st_1_on", b, sizeof b);
        if (atoi(b) == before || t1.holds != atoi(b)) toggles_ok = 0;
    }
    CHECK(toggles_ok, "6 taps on step tile 1: each one toggles, and MPC shows the right state");
    get(s, "st_5", b, sizeof b);
    CHECK(!strcmp(b, "E3"), "tile 5 shows its note (%s)", b);
    get(s, "p_5_display", b, sizeof b);
    CHECK(!strcmp(b, "E3"), "pitch knob 5 shows the note name (%s)", b);

    char b2[8];
    /* 6. scale-aware knob: Major, from E3 (52) up one lands on F3 (53), up again on G3 (55) */
    set(s, "scale", "1");
    seti(s, "p_5", 53); get(s, "p_5", b, sizeof b);
    seti(s, "p_5", 54); get(s, "p_5", b2, sizeof b2);
    CHECK(!strcmp(b, "53") && !strcmp(b2, "55"), "Major knob steps E3 -> F3 -> G3 (%s, %s)", b, b2);
    seti(s, "p_5", 54); get(s, "p_5", b, sizeof b);
    CHECK(!strcmp(b, "53"), "and down from G3 lands on F3 (%s)", b);

    /* 7. slots: STORE on slot tile 4, change, recall it; undo */
    tilesim_t t4 = { "sl_4", 0, 0 };
    tile_poll(s, &t4);
    set(s, "store", "1"); tile_tap(s, &t4); tile_poll(s, &t4);
    get(s, "sl_4_on", b, sizeof b); get(s, "sl_4", b2, sizeof b2);
    CHECK(!strcmp(b, "1") && !strcmp(b2, "[4]") && t4.holds == 1, "STORE + slot 4 saves it (%s, %s)", b, b2);
    char before[8], after[8];
    int recalls = 0;
    for (int r = 0; r < 6; r++) {                          /* each tap on a stored slot recalls it, tile stays lit */
        set(s, "random", "1");
        tile_tap(s, &t4); tile_poll(s, &t4);
        get(s, "p_1", after, sizeof after);
        get(s, "sl_4_on", b, sizeof b);
        if (!strcmp(after, "48") && !strcmp(b, "1") && t4.holds == 1) recalls++;
    }
    CHECK(recalls == 6, "6 taps on slot 4 recall it 6 times (%d), MPC keeps it lit", recalls);
    set(s, "random", "1"); get(s, "p_1", before, sizeof before);
    set(s, "random", "1"); set(s, "undo", "1");
    get(s, "p_1", after, sizeof after);
    CHECK(!strcmp(after, before), "undo goes back one edit (%s)", after);

    /* 8. randomize stays in scale and in the octave range */
    set(s, "scale", "2"); set(s, "rand_lo", "3"); set(s, "rand_hi", "4");
    int bad = 0;
    for (int r = 0; r < 50; r++) {
        set(s, "random", "1");
        for (int i = 1; i <= 16; i++) {
            char k[12]; snprintf(k, sizeof k, "p_%d", i); get(s, k, b, sizeof b);
            int n = atoi(b), pc = n % 12;
            if (n < 48 || n > 72 || !(pc == 0 || pc == 2 || pc == 3 || pc == 5 || pc == 7 || pc == 8 || pc == 10)) bad++;
        }
    }
    CHECK(bad == 0, "50 randomizes in C minor, octaves 3-4: %d notes out", bad);

    /* 9. state round trip with slots */
    char st1[8192], st2[8192];
    get(s, "state", st1, sizeof st1);
    void *s2 = E->create(NULL);
    set(s2, "state", st1);
    get(s2, "state", st2, sizeof st2);
    CHECK(!strcmp(st1, st2), "state round trip (%zu bytes)", strlen(st1));
    get(s2, "sl_4_on", b, sizeof b);
    CHECK(!strcmp(b, "1"), "slot 4 survives the reload");
    E->destroy(s); E->destroy(s2);

    /* 10. echo filter: our own note looped back is not a pad */
    s = fresh(); reset();
    run(s, 0, 120, 30, 0, 0, 0, 0);
    unsigned char echo[3] = { 0x90, (unsigned char)last_note, 0 };
    echo[2] = 0; (void)echo;
    get(s, "st_1_on", b, sizeof b);
    E->song_pos(s, 0, 120, 0, 128);
    E->destroy(s);


    /* 11. CHAIN: slot 1 = notes 60.. (len 4), slot 2 = notes 72.. (len 2, 1/8); chain 1 2 1 */
    {
        void *c = E->create(NULL);
        set(c, "scale", "0");
        set(c, "len", "4");
        for (int i = 0; i < 4; i++) { char k[12]; snprintf(k, sizeof k, "p_%d", i + 1); seti(c, k, 60 + i); }
        set(c, "store", "1"); set(c, "sl_1", "1");
        set(c, "len", "2"); set(c, "rate", "1");
        for (int i = 0; i < 2; i++) { char k[12]; snprintf(k, sizeof k, "p_%d", i + 1); seti(c, k, 72 + i); }
        set(c, "store", "1"); set(c, "sl_2", "1");
        set(c, "chain_edit", "1");
        unsigned char m1[3] = { 0x90, 24, 101 }, m2[3] = { 0x90, 25, 101 };
        E->midi(c, m1, 3); E->midi(c, m2, 3); E->midi(c, m1, 3);       /* pads 1 2 1 */
        set(c, "chain_edit", "0"); set(c, "chain_on", "1");
        char cv[64]; get(c, "chain_view", cv, sizeof cv);
        CHECK(!strcmp(cv, "3:  1 2 1"), "chain built from pads (%s)", cv);
        /* one pass = 4 x 1/16 (1 beat) + 2 x 1/8 (1 beat) + 1 beat = 3 beats; play 6 beats */
        int seq[64], ns = 0;
        static int got[64]; static int ng; ng = 0;
        reset();
        midiport_test_sink = NULL;
        extern void (*midiport_test_sink)(void *, int, int, int);
        void capture(void *o, int st, int d1, int d2);
        midiport_test_sink = capture;
        double q = 0, blk2 = 128 * 120 / (60 * 44100.0);
        while (q < 6.0 - 1e-9) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        E->song_pos(c, q, 120, 0, 128);
        extern int cap_notes[], cap_n;
        int want[] = { 60, 61, 62, 63, 72, 73, 60, 61, 62, 63, 60, 61, 62, 63, 72, 73, 60, 61, 62, 63 };
        int ok = cap_n >= 20;
        for (int i = 0; i < 20 && ok; i++) if (cap_notes[i] != want[i]) ok = 0;
        CHECK(ok, "chain 1 2 1 plays 60-63, 72 73 (1/8), 60-63, then loops (%d notes)", cap_n);
        (void)seq; (void)ns; (void)got;
        /* locate into the middle: song position 4.0 = second pass, element 2 (slot 2) at its first step */
        cap_n = 0;
        q = 4.0;
        for (int b = 0; b < 30; b++) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        E->song_pos(c, q, 120, 0, 128);
        CHECK(cap_n >= 1 && cap_notes[0] == 72, "start at beat 4 lands on slot 2's first step (note %d)", cap_n ? cap_notes[0] : -1);
        char st1[8192], st2[8192];
        get(c, "state", st1, sizeof st1);
        void *c2 = E->create(NULL); set(c2, "state", st1); get(c2, "state", st2, sizeof st2);
        get(c2, "chain_view", cv, sizeof cv);
        CHECK(!strcmp(st1, st2) && !strcmp(cv, "3:  1 2 1"), "chain survives a reload (%s)", cv);
        set(c2, "chain_back", "1"); get(c2, "chain_view", cv, sizeof cv);
        CHECK(!strcmp(cv, "2:  1 2"), "BACK removes the last element (%s)", cv);
        E->destroy(c); E->destroy(c2);
        midiport_test_sink = sink;
    }

    /* 12. chain under jitter, unreported 8-beat loop wraps and swing: no doubled step, nothing hangs */
    {
        void *c = fresh();
        set(c, "swing", "66");
        set(c, "store", "1"); set(c, "sl_1", "1");
        set(c, "len", "8"); set(c, "dir", "2");
        set(c, "store", "1"); set(c, "sl_2", "1");
        set(c, "chain_edit", "1"); set(c, "sl_1", "1"); set(c, "sl_2", "1"); set(c, "chain_edit", "0");
        set(c, "chain_on", "1");
        reset(); swing_d = 0;
        run(c, 0, 128, 30000, 0, 8, 1, 0);
        E->song_pos(c, 0, 128, 0, 128);
        CHECK(doubles == 0 && held_total() == 0 && ons > 700, "chain + jitter + loops + swing: %d notes, %d doubles, %d held", ons, doubles, held_total());
        E->destroy(c);
    }

    /* 13. velocity master and shapes */
    {
        void *c = E->create(NULL);
        char v1[8], v2[8];
        seti(c, "v_1", 100); seti(c, "v_2", 50);
        seti(c, "vmaster", 50);                                 /* SCALE from 100 to 50: halves */
        get(c, "v_1", v1, sizeof v1); get(c, "v_2", v2, sizeof v2);
        CHECK(!strcmp(v1, "50") && !strcmp(v2, "25"), "SCALE master 100 -> 50 halves the velocities (%s, %s)", v1, v2);
        seti(c, "vmaster", 100);
        get(c, "v_1", v1, sizeof v1); get(c, "v_2", v2, sizeof v2);
        CHECK(!strcmp(v1, "100") && !strcmp(v2, "50"), "back to 100 restores them exactly (%s, %s)", v1, v2);
        seti(c, "vmaster", 10); seti(c, "vmaster", 100);
        get(c, "v_2", v2, sizeof v2);
        CHECK(!strcmp(v2, "50"), "even through a low master, proportions survive (%s)", v2);
        seti(c, "v_2", 80); seti(c, "vmaster", 50);             /* a slider moved: new base */
        get(c, "v_2", v2, sizeof v2);
        CHECK(!strcmp(v2, "40"), "after moving a slider the master scales the new values (%s)", v2);
        set(c, "vm_mode", "1"); seti(c, "vmaster", 77);
        int all = 1; for (int i = 1; i <= 16; i++) { char k[12]; snprintf(k, sizeof k, "v_%d", i); get(c, k, v1, sizeof v1); if (strcmp(v1, "77")) all = 0; }
        CHECK(all, "SET puts all 16 at the master value");
        set(c, "vp_ramp", "1");
        get(c, "v_1", v1, sizeof v1); get(c, "v_16", v2, sizeof v2);
        CHECK(!strcmp(v1, "30") && !strcmp(v2, "127"), "RAMP goes 30 -> 127 (%s, %s)", v1, v2);
        set(c, "vp_saw", "1");
        get(c, "v_1", v1, sizeof v1); get(c, "v_16", v2, sizeof v2);
        CHECK(!strcmp(v1, "127") && !strcmp(v2, "30"), "SAW goes 127 -> 30 (%s, %s)", v1, v2);
        set(c, "undo", "1");
        get(c, "v_1", v1, sizeof v1);
        CHECK(!strcmp(v1, "30"), "UNDO brings back the ramp (%s)", v1);
        set(c, "vp_tri", "1"); set(c, "vp_sine", "1"); set(c, "vp_rand", "1");
        char st1[8192], st2[8192];
        get(c, "state", st1, sizeof st1);
        void *c2 = E->create(NULL); set(c2, "state", st1); get(c2, "state", st2, sizeof st2);
        CHECK(!strcmp(st1, st2), "master and mode survive a reload");
        E->destroy(c); E->destroy(c2);
    }

    /* 14. ratchet, tie, chance, step entry */
    {
        /* timeline capture: every note event with the song position of the block that sent it */
        void *c = E->create(NULL);
        char r1[8];
        set(c, "scale", "0"); set(c, "len", "4");
        for (int i = 0; i < 4; i++) { char k[12]; snprintf(k, sizeof k, "p_%d", i + 1); seti(c, k, 60 + 2 * i); }
        get(c, "rat_1_on", r1, sizeof r1);                       /* the wrapper has polled the tile */
        set(c, "rat_1", "1"); set(c, "rat_1", "0");               /* two taps: x3 */
        get(c, "rat_1", r1, sizeof r1);
        CHECK(!strcmp(r1, "x3"), "two taps on ratchet tile 1 = x3 (%s)", r1);
        cap_n = 0; midiport_test_sink = capture;
        double q = 0, blk2 = 128 * 120 / (60 * 44100.0);
        int onsum = 0;
        while (q < 4.0 - 1e-9) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        E->song_pos(c, q, 120, 0, 128);
        for (int i = 0; i < cap_n; i++) onsum += cap_notes[i] == 60;
        CHECK((onsum == 12 || onsum == 13) && (cap_n == 24 || cap_n == 25), "x3 on step 1: 3 hits a bar on 4 bars (%d hits of 60, %d notes)", onsum, cap_n);
        set(c, "rat_1", "1"); set(c, "rat_1", "0");               /* x3 -> x4 -> x1 */
        get(c, "rat_1", r1, sizeof r1);
        CHECK(!strcmp(r1, "x1"), "ratchet cycles back to x1 (%s)", r1);

        /* tie step 1 into the same note on step 2: one note-on for both */
        seti(c, "p_2", 60);
        get(c, "tie_1_on", r1, sizeof r1);
        set(c, "tie_1", "1");
        cap_n = 0; q = 0;
        while (q < 1.0 - 1e-9) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        E->song_pos(c, q, 120, 0, 128);
        CHECK(cap_n == 4 && cap_notes[0] == 60 && cap_notes[1] == 64, "TIE into the same note: no new note-on (%d notes, %d %d)", cap_n, cap_notes[0], cap_notes[1]);
        seti(c, "p_2", 62);
        cap_n = 0; q = 0;
        while (q < 1.0 - 1e-9) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        E->song_pos(c, q, 120, 0, 128);
        CHECK(cap_n == 5 && cap_notes[1] == 62, "TIE into another note: legato, both play (%d notes)", cap_n);

        /* chance */
        seti(c, "pr_3", 0);
        cap_n = 0; q = 0; int n64 = 0;
        while (q < 8.0 - 1e-9) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        E->song_pos(c, q, 120, 0, 128);
        for (int i = 0; i < cap_n; i++) n64 += cap_notes[i] == 64;
        CHECK(n64 == 0, "chance 0 %%: step 3 never plays (%d)", n64);
        seti(c, "pr_3", 50);
        char pd[8]; get(c, "pr_3_display", pd, sizeof pd);
        CHECK(!strcmp(pd, "50%"), "chance knob shows %% (%s)", pd);
        n64 = 0; int bars = 400;
        for (int b2i = 0; b2i < bars; b2i++) {
            cap_n = 0; q = 0;
            while (q < 1.0 - 1e-9) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
            E->song_pos(c, q, 120, 0, 128);
            for (int i = 0; i < cap_n; i++) n64 += cap_notes[i] == 64;
        }
        CHECK(n64 > bars * 40 / 100 && n64 < bars * 60 / 100, "chance 50 %%: step 3 played %d of %d times", n64, bars);

        /* state with the new fields, and an old (v0.4) state still loads */
        char st1[8192], st2[8192];
        get(c, "state", st1, sizeof st1);
        void *c2 = E->create(NULL); set(c2, "state", st1); get(c2, "state", st2, sizeof st2);
        CHECK(!strcmp(st1, st2), "tie / ratchet / chance survive a reload");
        char old[8192]; strcpy(old, st1);
        char *bar = strchr(old, '|'); char *sp = strchr(bar, ' ');
        memmove(bar, sp, strlen(sp) + 1);                       /* drop the extras: what v0.4 saved */
        void *c3 = E->create(NULL); set(c3, "state", old);
        get(c3, "p_2", pd, sizeof pd); get(c3, "pr_3", r1, sizeof r1);
        CHECK(!strcmp(pd, "62") && !strcmp(r1, "100"), "a v0.4 state loads, chance defaults to 100 (%s, %s)", pd, r1);
        E->destroy(c); E->destroy(c2); E->destroy(c3);

        /* step entry */
        c = E->create(NULL);
        set(c, "entry", "1");
        unsigned char k1[3] = { 0x90, 67, 90 }, k2[3] = { 0x90, 69, 70 };
        E->midi(c, k1, 3); E->midi(c, k2, 3);
        char a[8], b3[8], v[8]; get(c, "p_1", a, sizeof a); get(c, "p_2", b3, sizeof b3); get(c, "v_2", v, sizeof v);
        CHECK(!strcmp(a, "67") && !strcmp(b3, "69") && !strcmp(v, "70"), "STEP ENTRY: notes and velocity go into steps 1, 2 (%s %s %s)", a, b3, v);
        set(c, "entry_rest", "1");
        get(c, "st_3_on", a, sizeof a);
        CHECK(!strcmp(a, "0"), "REST turns step 3 off (%s)", a);
        get(c, "ph_4_on", a, sizeof a);
        CHECK(!strcmp(a, "1"), "the cursor shows on the light row: step 4 (%s)", a);
        set(c, "entry_back", "1"); set(c, "entry_back", "1");
        E->midi(c, k1, 3);
        get(c, "p_2", a, sizeof a);
        CHECK(!strcmp(a, "67"), "BACK twice, then a note rewrites step 2 (%s)", a);
        set(c, "st_10", "0");                                    /* a tap on step tile 10 moves the cursor there */
        unsigned char k3[3] = { 0x90, 72, 100 }; E->midi(c, k3, 3);
        get(c, "p_10", a, sizeof a); get(c, "st_10_on", b3, sizeof b3);
        CHECK(!strcmp(a, "72") && !strcmp(b3, "1"), "tap step 10, play: step 10 = 72, still on (%s, %s)", a, b3);
        set(c, "undo", "1");
        get(c, "p_1", a, sizeof a);
        CHECK(strcmp(a, "67") != 0, "UNDO takes back the whole entry (p_1 = %s)", a);
        E->destroy(c);
        midiport_test_sink = sink;
    }

    /* 15. ratchets, ties and chance everywhere, with jitter, loop wraps and swing: always mono, nothing left on */
    {
        void *c = fresh();
        char k[16];
        for (int i = 1; i <= 16; i++) {
            snprintf(k, sizeof k, "rat_%d", i); for (int t = 0; t < i % 4; t++) set(c, k, t & 1 ? "0" : "1");
            if (i % 3 == 0) { snprintf(k, sizeof k, "tie_%d", i); set(c, k, "1"); }
            snprintf(k, sizeof k, "pr_%d", i); seti(c, k, 40 + 4 * i);
        }
        set(c, "swing", "70");
        for (int b = 0; b < 50; b++) E->song_pos(c, 0, 137, 0, 128);   /* let the knob previews end first */
        mono_held = mono_max = mono_bad = 0;
        midiport_test_sink = mono_sink;
        run(c, 0, 137, 40000, 0, 8, 1, 1);
        E->song_pos(c, 0, 137, 0, 128);
        CHECK(mono_max == 1 && mono_held == 0 && mono_bad == 0, "ratchet/tie/chance stress: max %d note at once, %d left on, %d stray offs", mono_max, mono_held, mono_bad);
        E->destroy(c);
        midiport_test_sink = sink;
    }

    /* 16. slide */
    {
        void *c = E->create(NULL);
        char t[8];
        set(c, "scale", "0"); set(c, "len", "4");
        for (int i = 0; i < 4; i++) { char k[12]; snprintf(k, sizeof k, "p_%d", i + 1); seti(c, k, 60 + 2 * i); }
        for (int b = 0; b < 50; b++) E->song_pos(c, 0, 120, 0, 128);   /* previews end */
        get(c, "sld_1_on", t, sizeof t);
        set(c, "sld_1", "1");
        get(c, "sld_1", t, sizeof t);
        CHECK(!strcmp(t, "SLD"), "slide tile 1 on (%s)", t);
        midiport_test_sink = ev_sink; ev_n = 0;
        double q = 0, blk2 = 128 * 120 / (60 * 44100.0);
        while (q < 0.5 - 1e-9) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        E->song_pos(c, q, 120, 0, 128);
        int on60 = ev_find(0, 0x90, 60, -1), on62 = ev_find(0, 0x90, 62, -1), off60 = ev_find(0, 0x80, 60, -1);
        CHECK(on60 >= 0 && on62 > on60 && off60 > on62, "slide 60 -> 62: 62 starts before 60 ends (on60 %d, on62 %d, off60 %d)", on60, on62, off60);
        CHECK(ev_find(0, 0xb0, 65, -1) < 0, "SLIDE CC65 off: no CC sent");
        /* into the same note: no retrigger */
        seti(c, "p_2", 60);
        ev_n = 0; q = 0;
        while (q < 0.5 - 1e-9) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        E->song_pos(c, q, 120, 0, 128);
        int ons = 0; for (int i = 0; i < ev_n; i++) ons += (ev_st[i] & 0xf0) == 0x90 && ev_d2[i] && ev_d1[i] == 60;
        CHECK(ons == 1, "slide into the same note: one note-on (%d)", ons);
        /* slide into a rest: the note ends at the rest */
        seti(c, "p_2", 62); set(c, "st_2", "0");
        ev_n = 0; q = 0;
        while (q < 0.3) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        int o60 = ev_find(0, 0x80, 60, -1);
        CHECK(o60 >= 0, "slide into a rest: the note ends at the rest");
        E->song_pos(c, q, 120, 0, 128);
        set(c, "st_2", "1");
        /* CC65 around the slide */
        set(c, "slide_cc", "1");
        ev_n = 0; q = 0;
        while (q < 0.5 - 1e-9) { E->song_pos(c, q, 120, 1, 128); q += blk2; }
        E->song_pos(c, q, 120, 0, 128);
        int cOn = ev_find(0, 0xb0, 65, 127), n62 = ev_find(0, 0x90, 62, -1), cOff = ev_find(0, 0xb0, 65, 0);
        CHECK(cOn >= 0 && n62 > cOn && cOff > n62, "SLIDE CC65: 127 before the slide, 0 after the new note (%d, %d, %d)", cOn, n62, cOff);
        /* state: slide kept; a v0.5 state (4 characters a step) still loads */
        char st1[8192], st2[8192];
        get(c, "state", st1, sizeof st1);
        void *c2 = E->create(NULL); set(c2, "state", st1); get(c2, "state", st2, sizeof st2);
        CHECK(!strcmp(st1, st2), "slide and SLIDE CC65 survive a reload");
        char old[8192]; strcpy(old, st1);
        char *bar = strchr(old, '|') + 1;                       /* rewrite the extras as v0.5 did: drop each 5th char */
        char tmp[200]; int w = 0; for (int i = 0; i < 16 * 5; i++) if (i % 5 != 4) tmp[w++] = bar[i];
        memmove(bar + w, bar + 80, strlen(bar + 80) + 1); memcpy(bar, tmp, w);
        void *c3 = E->create(NULL); set(c3, "state", old);
        get(c3, "sld_1", t, sizeof t); char r[8]; get(c3, "pr_1", r, sizeof r);
        CHECK(!strcmp(t, "-") && !strcmp(r, "100"), "a v0.5 state loads (slide off, chance %s)", r);
        E->destroy(c); E->destroy(c2); E->destroy(c3);
        /* stress: slides everywhere with ratchets and ties, jitter and loops: at most 2 notes (the overlap), none left */
        c = fresh();
        for (int i = 1; i <= 16; i++) {
            char k[16];
            if (i % 2) { snprintf(k, sizeof k, "sld_%d", i); set(c, k, "1"); }
            if (i % 5 == 0) { snprintf(k, sizeof k, "rat_%d", i); set(c, k, "1"); }
            if (i % 7 == 0) { snprintf(k, sizeof k, "tie_%d", i); set(c, k, "1"); }
            snprintf(k, sizeof k, "pr_%d", i); seti(c, k, 50 + 3 * i);
        }
        set(c, "slide_cc", "1");
        for (int b = 0; b < 50; b++) E->song_pos(c, 0, 128, 0, 128);
        mono_held = mono_max = mono_bad = 0;
        midiport_test_sink = mono_sink;
        run(c, 0, 128, 40000, 0, 8, 1, 1);
        E->song_pos(c, 0, 128, 0, 128);
        CHECK(mono_max <= 2 && mono_held == 0 && mono_bad == 0, "slide stress: max %d notes at once, %d left on, %d stray offs", mono_max, mono_held, mono_bad);
        E->destroy(c);
        midiport_test_sink = sink;
    }
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
