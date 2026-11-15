/* PadStep 16 for MPC OS: a 16-step monophonic MIDI step sequencer (the behaviour of Benoit's HTML step sequencer), no audio.
 *
 * Open MPC shared wrapper (wrapper/engine.h, "HAS_SONG_POS"). Notes leave through our own ALSA port "PadStep 16 / MIDI Out"
 * (midiport.h); any MPC track takes that port + the channel as its MIDI input. The MPC pads, on the PadStep track, toggle the
 * 16 steps (or transpose, or pick a scale / mode / step size / channel / slot after SHIFT, CHAN, STORE, RECALL).
 *
 * Timing as docs/MIDI_TIMING.md (open-mpc/mpc-vst-plugins): steps placed from the host song position, each block sends what
 * falls in [sent_to, ppq + blk), the pattern position is a function of the absolute step index, jitter is not a jump, a jump
 * re-anchors and a step the look-ahead already played is not played twice. Swing delays odd steps by 2*d*(swing-50)%. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "engine.h"
#include "midiport.h"

#define NS 16
#define SR 44100.0
#define NOTE_LO 24
#define NOTE_HI 96
#define NUNDO 8
#define NCHAIN 32

enum { DIR_FWD, DIR_REV, DIR_ALT, DIR_RAND };
enum { LEG_OFF, LEG_ON, LEG_RESET };
enum { PAD_STEPS, PAD_TRANSPOSE };
enum { MODE_NONE, MODE_SHIFT, MODE_CHAN, MODE_STORE, MODE_RECALL };

static const int RATE_DIV[] = { 4, 8, 16, 32 };      /* step size 1/4 .. 1/32 */
#define NRATES 4

typedef struct { int n; int iv[12]; } scale_t;
static const scale_t SCALES[] = {
    { 12, {0,1,2,3,4,5,6,7,8,9,10,11} },   /* Chromatic */
    { 7, {0,2,4,5,7,9,11} },               /* Major */
    { 7, {0,2,3,5,7,8,10} },               /* Minor */
    { 7, {0,2,3,5,7,9,10} },               /* Dorian */
    { 7, {0,2,4,5,7,9,10} },               /* Mixolydian */
    { 7, {0,2,3,5,7,8,11} },               /* Harmonic minor */
    { 6, {0,3,5,6,7,10} },                 /* Blues */
};
#define NSCALES 7
static const char *SCALE_NAMES[NSCALES] = { "Chrom", "Major", "Minor", "Dorian", "Mixo", "HarmMin", "Blues" };
static const char *DIR_NAMES[4] = { "FWD", "REV", "ALT", "RAND" };
static const char *NOTE_NAMES[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

typedef struct { uint8_t note, on, vel, prob, tie, rat, slide; } step_t;   /* prob 0..100 %, tie/slide 0/1, rat(chet) 1..4 hits */

typedef struct {           /* what a slot keeps (no tempo: MPC owns it) */
    step_t st[NS];
    int rate, len, dir, scale, swing, gate, legato, transpose;
} snap_t;

typedef struct {
    snap_t cur;
    int chan, pad_mode, pad_base, rand_lo, rand_hi;
    int mode;                         /* SHIFT / CHAN / STORE / RECALL: the next pad or slot tap uses it */
    snap_t slot[NS];
    uint8_t slot_used[NS];
    int active_slot;                  /* -1 = none */
    snap_t undo[NUNDO];
    int nundo;
    /* transport */
    int playing;
    double sent_to;
    long next_k;
    double since;
    int cur_pos;                      /* step last played, -1 = none */
    int sounding;                     /* note held, -1 = none */
    int snd_ch;
    double off_t;                     /* song position of its note-off (1e300 = until the next step) */
    struct { uint8_t ch, note; int frames; } prev;   /* knob preview note while stopped */
    int last_in;
    uint8_t padmap[NS];               /* the note each MPC pad sends, pad 1..16 = step 1..16 (PAD BASE or LEARN) */
    int learn;                        /* LEARN: the next pad hit is pad number learn (1..16), 0 = off */
    /* chain: slots played one after the other, each for one pass, from the song position (deterministic: a locate or a
     * loop lands on the same element and step a straight run would have reached) */
    uint8_t chain[NCHAIN];
    int chain_n, chain_on, chain_edit;
    int chain_cur;                    /* element playing (-1 = none) */
    double chain_hw;                  /* song position below which chain steps were already sent */
    /* velocity master: SET puts all 16 at its value; SCALE multiplies them by master / (its value when they were taken) */
    int vmaster, vm_mode;
    uint8_t vbase[NS], vlast[NS];     /* velocities taken as the base, and what the master last wrote */
    int vref;                         /* master value when the base was taken */
    /* ratchet: the hits after the first one of a step, sent when their song position comes */
    int rat_left, rat_note, rat_vel;
    double rat_t, rat_dt, rat_gate;
    int prev_tied;                    /* the note sounding was tied into the next step */
    int prev_slide;                   /* ... or slides into it: the next note starts before this one ends */
    int slide_cc, porta_on;           /* SLIDE CC65: portamento on around each slide (porta_on: CC65 127 sent) */
    int entry, cursor;                /* STEP ENTRY: a played note goes into step cursor, which then moves on */
    /* tiles: the wrapper drops MPC's echo of its own "_on" push (same value as its cache), so every set that reaches us
     * is a tap. Only before the first "_on" read (a host restoring values) a set equal to what we show is ignored. */
    signed char tile_rep[NS], slot_rep[NS], tie_rep[NS], rat_rep[NS], sld_rep[NS];   /* last "_on" told to the wrapper (-1 = never) */
    midiport_t out;
    uint32_t rng;
    unsigned rev;
} ps_t;

static uint32_t rnd(ps_t *s) { s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5; return s->rng; }
static int rndn(ps_t *s, int n) { return n > 0 ? (int)(rnd(s) % (unsigned)n) : 0; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int in_scale(int scale, int m) {
    const scale_t *sc = &SCALES[scale];
    for (int i = 0; i < sc->n; i++) if (sc->iv[i] == ((m % 12) + 12) % 12) return 1;
    return 0;
}
/* nearest note of the scale, as the HTML's qScale */
static int qscale(int m, int scale) {
    const scale_t *sc = &SCALES[scale];
    int b = m % 12, o = m / 12, best = b, min = 99;
    for (int i = 0; i < sc->n; i++) { int x = abs(b - sc->iv[i]); if (x < min) { min = x; best = sc->iv[i]; } }
    return o * 12 + best;
}
/* a knob turned up or down lands on the next scale note that way, so it never sticks */
static int qscale_dir(int m, int from, int scale) {
    if (in_scale(scale, m)) return m;
    if (m > from) { while (m < NOTE_HI && !in_scale(scale, m)) m++; return m; }
    if (m < from) { while (m > NOTE_LO && !in_scale(scale, m)) m--; return m; }
    return qscale(m, scale);
}
static void note_name(int m, char *b, int n) { snprintf(b, n, "%s%d", NOTE_NAMES[m % 12], m / 12 - 1); }

/* ------------------------------------------------------------------ output */
static void end_note(ps_t *s) {
    if (s->sounding >= 0) midiport_send(&s->out, 0x80 | s->snd_ch, s->sounding, 0);
    s->sounding = -1;
}

static double step_ppq(const ps_t *s) { return 4.0 / RATE_DIV[clampi(s->cur.rate, 0, NRATES - 1)]; }

static double step_time(const ps_t *s, long k, double d) {
    double sw = (k & 1) ? 2.0 * d * (clampi(s->cur.swing, 50, 75) - 50) / 100.0 : 0;
    return k * d + sw;
}

static int pattern_pos(ps_t *s, long k) {
    int L = clampi(s->cur.len, 1, NS);
    switch (s->cur.dir) {
    case DIR_REV: return L - 1 - (int)(k % L);
    case DIR_ALT: { if (L == 1) return 0; int P = 2 * L - 2, q = (int)(k % P); return q < L ? q : P - q; }
    case DIR_RAND: return rndn(s, L);
    default: return (int)(k % L);
    }
}

static void porta_end(ps_t *s) {
    if (s->porta_on) { midiport_send(&s->out, 0xb0 | s->snd_ch, 65, 0); s->porta_on = 0; }
}

static void play_pos(ps_t *s, int p, double t, double d) {
    s->cur_pos = p;
    const step_t *st = &s->cur.st[p];
    int tied = s->prev_tied, slid = s->prev_slide;
    s->prev_tied = s->prev_slide = 0;
    s->rat_left = 0;
    if (!st->on || (st->prob < 100 && rndn(s, 100) >= st->prob)) { end_note(s); porta_end(s); return; }   /* a rest, or chance said no */
    int note = clampi(st->note + s->cur.transpose, 0, 127), n = clampi(st->rat, 1, 4);
    double gate = d * clampi(s->cur.gate, 50, 99) / 100.0;
    if ((tied || slid) && s->sounding == note && n == 1) {
        /* a tie or slide into the same note just carries on */
    } else if (slid && s->sounding >= 0) {           /* slide: the new note starts, then the old one ends (overlap) */
        int old = s->sounding, och = s->snd_ch;
        s->snd_ch = s->chan & 15;
        midiport_send(&s->out, 0x90 | s->snd_ch, note, clampi(st->vel, 1, 127));
        midiport_send(&s->out, 0x80 | och, old, 0);
        s->sounding = note;
    } else {
        end_note(s);                                 /* mono: the previous note ends here at the latest */
        s->snd_ch = s->chan & 15;
        midiport_send(&s->out, 0x90 | s->snd_ch, note, clampi(st->vel, 1, 127));
        s->sounding = note;
    }
    porta_end(s);                                    /* the glide into this note has started: portamento back off */
    if (n > 1) {                                     /* ratchet: n hits in the step, each with the gate of its share */
        s->rat_dt = d / n;
        s->rat_gate = gate / n;
        s->off_t = t + s->rat_gate;
        s->rat_left = n - 1;
        s->rat_t = t + s->rat_dt;
        s->rat_note = note;
        s->rat_vel = clampi(st->vel, 1, 127);
    } else if (st->slide) {                          /* held to the next step, which slides in */
        s->off_t = 1e300;
        s->prev_slide = 1;
        if (s->slide_cc) { midiport_send(&s->out, 0xb0 | s->snd_ch, 65, 127); s->porta_on = 1; }
    } else if (st->tie || s->cur.legato != LEG_OFF) {
        s->off_t = 1e300;                            /* held to the next step */
        s->prev_tied = st->tie;
    } else {
        s->off_t = t + gate;
    }
}

/* send the note-offs and ratchet hits due before song position T, in time order */
static void flush_until(ps_t *s, double T) {
    for (;;) {
        double toff = s->sounding >= 0 ? s->off_t : 1e300, trat = s->rat_left > 0 ? s->rat_t : 1e300;
        if (toff >= T && trat >= T) return;
        if (toff <= trat) { end_note(s); continue; }
        end_note(s);
        midiport_send(&s->out, 0x90 | s->snd_ch, s->rat_note, s->rat_vel);
        s->sounding = s->rat_note;
        s->off_t = s->rat_t + s->rat_gate;
        s->rat_t += s->rat_dt;
        s->rat_left--;
    }
}

static void fire(ps_t *s, long k, double t, double d) { play_pos(s, pattern_pos(s, k), t, d); }

/* ---- chain timeline: element e lasts steps_e * d_e (one pass; ALT: there and back), the whole chain D, from song
 * position 0. A boundary is (element, local step j) at base + S_e + j d_e (+ that element's swing on odd j). */
static int chain_steps(const snap_t *c) {
    int L = clampi(c->len, 1, NS);
    return (c->dir == DIR_ALT && L > 1) ? 2 * L - 2 : L;
}
static double snap_d(const snap_t *c) { return 4.0 / RATE_DIV[clampi(c->rate, 0, NRATES - 1)]; }
static double chain_dur(const ps_t *s, int e) {
    int sl = s->chain[e];
    return s->slot_used[sl] ? chain_steps(&s->slot[sl]) * snap_d(&s->slot[sl]) : 0;
}
static double chain_total(const ps_t *s) { double D = 0; for (int e = 0; e < s->chain_n; e++) D += chain_dur(s, e); return D; }
static double chain_btime(const ps_t *s, double base_e, int e, int j) {
    const snap_t *c = &s->slot[s->chain[e]];
    double d = snap_d(c);
    return base_e + j * d + ((j & 1) ? 2.0 * d * (clampi(c->swing, 50, 75) - 50) / 100.0 : 0);
}
/* the first chain boundary at or after t */
static int chain_next(const ps_t *s, double t, double D, double *bt, int *be, int *bj) {
    if (D <= 0) return 0;
    double base = floor(t / D) * D;
    for (int lap = 0; lap < 3; lap++, base += D) {
        double S = base;
        for (int e = 0; e < s->chain_n; e++) {
            double dur = chain_dur(s, e);
            if (dur <= 0) continue;
            if (S + dur > t - 1e-9) {
                const snap_t *c = &s->slot[s->chain[e]];
                int n = chain_steps(c);
                int j = (int)floor((t - S) / snap_d(c) - 1e-9);
                if (j < 0) j = 0;
                for (; j < n; j++) {
                    double b = chain_btime(s, S, e, j);
                    if (b >= t - 1e-9) { *bt = b; *be = e; *bj = j; return 1; }
                }
            }
            S += dur;
        }
    }
    return 0;
}
static void chain_fire(ps_t *s, int e, int j, double t) {
    if (e != s->chain_cur) {                         /* next element: load its slot (keep the live transpose) */
        int tr = s->cur.transpose;
        s->cur = s->slot[s->chain[e]];
        s->cur.transpose = tr;
        s->active_slot = s->chain[e];
        s->chain_cur = e;
        s->rev++;
    }
    int L = clampi(s->cur.len, 1, NS), p;
    switch (s->cur.dir) {
    case DIR_REV: p = L - 1 - j; break;
    case DIR_ALT: p = j < L ? j : 2 * L - 2 - j; break;
    case DIR_RAND: p = rndn(s, L); break;
    default: p = j;
    }
    play_pos(s, clampi(p, 0, L - 1), t, snap_d(&s->cur));
}
static int chain_active(const ps_t *s) { return s->chain_on && s->chain_n > 0 && chain_total(s) > 0; }

static long first_k(const ps_t *s, double start, double d) {
    long k = (long)ceil(start / d - 1e-9) - 1;
    if (k < 0) k = 0;
    while (step_time(s, k, d) < start - 1e-9) k++;
    return k;
}

static void song_pos(void *inst, double ppq, double bpm, int playing, int frames) {
    ps_t *s = inst;
    s->out.blk++;
    s->since += frames;
    if (s->prev.frames > 0 && (s->prev.frames -= frames) <= 0) midiport_send(&s->out, 0x80 | s->prev.ch, s->prev.note, 0);
    if (!playing || ppq < 0 || bpm <= 0) {
        if (s->playing) { end_note(s); porta_end(s); s->rat_left = 0; s->prev_tied = s->prev_slide = 0; s->playing = 0; s->cur_pos = -1; s->rev++; }
        return;
    }
    double d = step_ppq(s), blk = frames * bpm / (60.0 * SR), start, end = ppq + blk;
    if (!s->playing) {
        s->playing = 1;
        start = s->sent_to = ppq;
        s->next_k = first_k(s, start, d);
        s->since = 1e12;
        s->chain_hw = start;
        s->chain_cur = -1;
        s->rev++;
    } else if (fabs(ppq - s->sent_to) < 1.0) {
        start = s->sent_to;
    } else {                                         /* loop wrap or locate */
        start = ppq - 0.002;
        if (start < 0) start = 0;
        if (s->off_t < 1e299) s->off_t += start - s->sent_to;
        s->rat_t += start - s->sent_to;
        s->next_k = first_k(s, start, d);
        s->chain_hw = start;
    }
    if (end <= start) return;
    if (chain_active(s)) {
        double D = chain_total(s), bt;
        int e, j;
        if (start < s->chain_hw - 1.0 || start > s->chain_hw + 1.0) s->chain_hw = start;   /* start, jump, chain just on */
        double from = s->chain_hw > start ? s->chain_hw : start;
        while (chain_next(s, from, D, &bt, &e, &j) && bt < end) {
            double sf = snap_d(&s->slot[s->chain[e]]) * 60.0 * SR / bpm;
            if (s->since >= 0.25 * sf) {
                flush_until(s, bt);
                chain_fire(s, e, j, bt);
                s->since = 0;
            }
            from = bt + 1e-6;
            s->chain_hw = from;
        }
        flush_until(s, end);
        s->sent_to = end;
        s->next_k = first_k(s, end, d);              /* ready if CHAIN PLAY goes off */
        return;
    }
    double step_frames = d * 60.0 * SR / bpm;
    for (long k = s->next_k; step_time(s, k, d) < end; k++) {
        if (s->since >= 0.25 * step_frames) {        /* else the look-ahead already played it (swing 75 keeps 0.5 d) */
            flush_until(s, step_time(s, k, d));
            fire(s, k, step_time(s, k, d), d);
            s->since = 0;
        }
        s->next_k = k + 1;
    }
    flush_until(s, end);
    s->sent_to = end;
}

static void preview(ps_t *s, int note) {
    if (s->playing) return;
    if (s->prev.frames > 0) midiport_send(&s->out, 0x80 | s->prev.ch, s->prev.note, 0);
    s->prev.ch = (uint8_t)(s->chan & 15);
    s->prev.note = (uint8_t)clampi(note + s->cur.transpose, 0, 127);
    s->prev.frames = (int)(0.1 * SR);
    midiport_send(&s->out, 0x90 | s->prev.ch, s->prev.note, 100);
}

/* ------------------------------------------------------------------ edits (the HTML's tools) */
static void push_undo(ps_t *s) {
    if (s->nundo == NUNDO) { memmove(&s->undo[0], &s->undo[1], sizeof(snap_t) * (NUNDO - 1)); s->nundo--; }
    s->undo[s->nundo++] = s->cur;
}
static void do_undo(ps_t *s) { if (s->nundo) { s->cur = s->undo[--s->nundo]; s->rev++; } }

static void randomize(ps_t *s) {
    push_undo(s);
    int lo = (s->rand_lo + 1) * 12, hi = (s->rand_hi + 2) * 12, pool[128], n = 0;
    for (int m = lo; m <= hi && m < 128; m++) if (in_scale(s->cur.scale, m)) pool[n++] = m;
    if (!n) return;
    int prev = rndn(s, n);
    for (int i = 0; i < clampi(s->cur.len, 1, NS); i++) {
        int r = rndn(s, 100), idx;
        if (r < 55) idx = prev + rndn(s, 5) - 2;                                   /* stepwise */
        else if (r < 85) idx = prev + (rndn(s, 2) ? 1 : -1) * (3 + rndn(s, 4));    /* small leap */
        else idx = rndn(s, n);                                                    /* anywhere */
        idx = clampi(idx, 0, n - 1);
        s->cur.st[i].note = (uint8_t)pool[idx];
        s->cur.st[i].on = rndn(s, 100) >= 12;
        s->cur.st[i].vel = (uint8_t)(60 + rndn(s, 68));
        prev = idx;
    }
    s->rev++;
}

static void mutate(ps_t *s) {
    push_undo(s);
    int pool[64], n = 0;
    for (int m = 36; m <= 84; m++) if (in_scale(s->cur.scale, m)) pool[n++] = m;
    if (!n) return;
    int L = clampi(s->cur.len, 1, NS), count = 2 + rndn(s, 4);
    for (int c = 0; c < count; c++) {
        step_t *st = &s->cur.st[rndn(s, L)];
        int ci = -1;
        for (int j = 0; j < n; j++) if (pool[j] == st->note) ci = j;
        if (ci >= 0) st->note = (uint8_t)pool[clampi(ci + (rndn(s, 2) ? 1 : -1) * (1 + rndn(s, 3)), 0, n - 1)];
        else st->note = (uint8_t)clampi(qscale(st->note + (rndn(s, 2) ? -2 : 2), s->cur.scale), 36, 84);
        if (rndn(s, 100) < 15) st->on = !st->on;
    }
    s->rev++;
}

static void shift_pattern(ps_t *s, int dir) {
    push_undo(s);
    int L = clampi(s->cur.len, 1, NS);
    step_t t;
    if (dir > 0) { t = s->cur.st[L - 1]; memmove(&s->cur.st[1], &s->cur.st[0], sizeof(step_t) * (L - 1)); s->cur.st[0] = t; }
    else { t = s->cur.st[0]; memmove(&s->cur.st[0], &s->cur.st[1], sizeof(step_t) * (L - 1)); s->cur.st[L - 1] = t; }
    s->rev++;
}

static void reverse_pattern(ps_t *s) {
    push_undo(s);
    int L = clampi(s->cur.len, 1, NS);
    for (int i = 0; i < L / 2; i++) { step_t t = s->cur.st[i]; s->cur.st[i] = s->cur.st[L - 1 - i]; s->cur.st[L - 1 - i] = t; }
    s->rev++;
}

static void clear_pattern(ps_t *s) {
    push_undo(s);
    for (int i = 0; i < NS; i++) {
        step_t *st = &s->cur.st[i];
        st->note = 60; st->on = 1; st->vel = 100; st->prob = 100; st->tie = 0; st->rat = 1; st->slide = 0;
    }
    s->rev++;
}

static void set_master(ps_t *s, int m) {
    m = clampi(m, 0, 127);
    int changed = 0;
    for (int i = 0; i < NS; i++) if (s->cur.st[i].vel != s->vlast[i]) changed = 1;
    if (changed || s->vref < 1) {                    /* the velocities moved since the master last wrote them: new base */
        for (int i = 0; i < NS; i++) s->vbase[i] = s->cur.st[i].vel;
        s->vref = s->vmaster > 0 ? s->vmaster : 1;
    }
    for (int i = 0; i < NS; i++) {
        int v = s->vm_mode ? m : (int)lround(s->vbase[i] * (double)m / s->vref);
        s->cur.st[i].vel = (uint8_t)clampi(v, 1, 127);
        s->vlast[i] = s->cur.st[i].vel;
    }
    s->vmaster = m;
    s->rev++;
}

enum { VP_SAW, VP_RAMP, VP_SINE, VP_TRI, VP_RAND };
static void vel_preset(ps_t *s, int kind) {
    push_undo(s);
    int L = clampi(s->cur.len, 1, NS);
    const double lo = 30, hi = 127;
    for (int i = 0; i < NS; i++) {
        int k = i % L;
        double x = L > 1 ? (double)k / (L - 1) : 0, y;
        switch (kind) {
        case VP_SAW: y = 1 - x; break;
        case VP_RAMP: y = x; break;
        case VP_SINE: y = 0.5 + 0.5 * sin(2 * M_PI * k / L); break;
        case VP_TRI: y = 1 - fabs(2.0 * k / L - 1); break;
        default: y = rndn(s, 1000) / 999.0;
        }
        s->cur.st[i].vel = (uint8_t)clampi((int)lround(lo + (hi - lo) * y), 1, 127);
    }
    s->rev++;
}

static void store_slot(ps_t *s, int i) { s->slot[i] = s->cur; s->slot_used[i] = 1; s->active_slot = i; s->rev++; }
static void recall_slot(ps_t *s, int i) {
    if (!s->slot_used[i]) return;
    push_undo(s);
    double d_old = step_ppq(s);
    s->cur = s->slot[i];
    s->active_slot = i;
    if (s->playing && step_ppq(s) != d_old) s->next_k = first_k(s, s->sent_to, step_ppq(s));
    s->rev++;
}

static void set_rate(ps_t *s, int r) {
    r = clampi(r, 0, NRATES - 1);
    if (r == s->cur.rate) return;
    s->cur.rate = r;
    if (s->playing) s->next_k = first_k(s, s->sent_to, step_ppq(s));
    s->rev++;
}

static void chain_add(ps_t *s, int slot) {
    if (s->chain_n < NCHAIN) { s->chain[s->chain_n++] = (uint8_t)slot; s->rev++; }
}

/* a pad (or a step tile tap) number i, the HTML's handlePadClick */
static void pad(ps_t *s, int i) {
    switch (s->mode) {
    case MODE_SHIFT:
        if (i < 7) s->cur.scale = i;
        else if (i >= 8 && i <= 11) s->cur.dir = i - 8;
        else if (i >= 12) set_rate(s, i - 12);
        break;
    case MODE_CHAN: end_note(s); s->chan = i; break;
    case MODE_STORE: store_slot(s, i); break;
    case MODE_RECALL: recall_slot(s, i); break;
    default: s->cur.st[i].on = !s->cur.st[i].on; break;
    }
    s->mode = MODE_NONE;
    s->rev++;
}

static void midi(void *inst, const uint8_t *m, int n) {
    ps_t *s = inst;
    if (n < 3) return;
    int st = m[0] & 0xf0;
    if (st != 0x90 && st != 0x80) return;
    if (midiport_was_just_sent(&s->out, m)) return;
    if (st != 0x90 || m[2] == 0) return;
    int note = m[1], i = -1;
    s->last_in = note;
    s->rev++;
    if (s->learn) {                                  /* hit the pads 1..16 in order */
        s->padmap[s->learn - 1] = (uint8_t)note;
        s->learn = s->learn == NS ? 0 : s->learn + 1;
        return;
    }
    if (s->entry && s->mode == MODE_NONE && !s->chain_edit) {   /* STEP ENTRY: any note, from pads or a keyboard */
        step_t *st = &s->cur.st[s->cursor];
        st->note = (uint8_t)clampi(note, NOTE_LO, NOTE_HI);
        st->on = 1;
        st->vel = m[2];
        preview(s, st->note);
        s->cursor = (s->cursor + 1) % clampi(s->cur.len, 1, NS);
        return;
    }
    for (int j = 0; j < NS; j++) if (s->padmap[j] == note) { i = j; break; }
    if (s->chain_edit && s->mode == MODE_NONE) { if (i >= 0 && i < NS) chain_add(s, i); return; }   /* pads = slots */
    if (s->mode != MODE_NONE) { if (i >= 0 && i < NS) pad(s, i); return; }
    if (s->pad_mode == PAD_TRANSPOSE) { s->cur.transpose = clampi(note - 60, -24, 24); return; }
    if (i >= 0 && i < NS) pad(s, i);
}

/* ------------------------------------------------------------------ parameters */
static int idx_key(const char *k, const char *prefix, int *is_on, const char **suffix) {
    size_t n = strlen(prefix);
    if (strncmp(k, prefix, n)) return -1;
    char *end;
    long i = strtol(k + n, &end, 10);
    if (end == k + n || i < 1 || i > NS) return -1;
    *is_on = !strcmp(end, "_on");
    if (suffix) *suffix = end;
    if (*end && !*is_on && !(suffix && !strcmp(end, "_display"))) return -1;
    return (int)i - 1;
}

static int tile_lit(const ps_t *s, int i) { return s->cur.st[i].on; }
static void load_state(ps_t *s, const char *str);

static void set_param(void *inst, const char *k, const char *val) {
    ps_t *s = inst;
    int iv = atoi(val), on, i;
    double fv = atof(val);
    if (!strcmp(k, "state")) { load_state(s, val); return; }
    if ((i = idx_key(k, "st_", &on, NULL)) >= 0 && !on) {          /* step tile tap (see Tenori-Grid: echo vs tap) */
        int want = fv > 0.5;
        if (s->tile_rep[i] < 0 && want == tile_lit(s, i)) return;
        if (s->entry && s->mode == MODE_NONE) { s->cursor = i; s->rev++; return; }   /* STEP ENTRY: pick the step */
        pad(s, i);
        return;
    }
    if ((i = idx_key(k, "sl_", &on, NULL)) >= 0 && !on) {          /* slot tile tap: recall, or store in STORE mode */
        int want = fv > 0.5;
        if (s->slot_rep[i] < 0 && want == s->slot_used[i]) return;
        if (s->mode == MODE_STORE) store_slot(s, i);
        else if (s->chain_edit) chain_add(s, i);
        else recall_slot(s, i);
        s->mode = MODE_NONE;
        s->rev++;
        return;
    }
    if ((i = idx_key(k, "ph_", &on, NULL)) >= 0) return;
    if ((i = idx_key(k, "tie_", &on, NULL)) >= 0 && !on) {         /* tile taps, as st_ */
        if (s->tie_rep[i] < 0 && (fv > 0.5) == s->cur.st[i].tie) return;
        s->cur.st[i].tie = !s->cur.st[i].tie; s->rev++; return;
    }
    if ((i = idx_key(k, "sld_", &on, NULL)) >= 0 && !on) {
        if (s->sld_rep[i] < 0 && (fv > 0.5) == s->cur.st[i].slide) return;
        s->cur.st[i].slide = !s->cur.st[i].slide; s->rev++; return;
    }
    if ((i = idx_key(k, "rat_", &on, NULL)) >= 0 && !on) {         /* each tap: 1 -> 2 -> 3 -> 4 -> 1 hits */
        if (s->rat_rep[i] < 0 && (fv > 0.5) == (s->cur.st[i].rat > 1)) return;
        s->cur.st[i].rat = (uint8_t)(s->cur.st[i].rat % 4 + 1); s->rev++; return;
    }
    if ((i = idx_key(k, "pr_", &on, NULL)) >= 0 && !on) { s->cur.st[i].prob = (uint8_t)clampi(iv, 0, 100); s->rev++; return; }
    if ((i = idx_key(k, "p_", &on, NULL)) >= 0 && !on) {
        int from = s->cur.st[i].note, m = qscale_dir(clampi(iv, NOTE_LO, NOTE_HI), from, s->cur.scale);
        if (m != from) { s->cur.st[i].note = (uint8_t)m; preview(s, m); s->rev++; }
        return;
    }
    if ((i = idx_key(k, "v_", &on, NULL)) >= 0 && !on) { s->cur.st[i].vel = (uint8_t)clampi(iv, 1, 127); return; }
    int was = s->mode;
#define MODE_TOGGLE(key, M) if (!strcmp(k, key)) { s->mode = fv > 0.5 ? M : (was == M ? MODE_NONE : was); s->rev++; return; }
    MODE_TOGGLE("shift", MODE_SHIFT) MODE_TOGGLE("chanm", MODE_CHAN) MODE_TOGGLE("store", MODE_STORE) MODE_TOGGLE("recall", MODE_RECALL)
    if (!strcmp(k, "dir")) s->cur.dir = clampi(iv, 0, 3);
    else if (!strcmp(k, "scale")) s->cur.scale = clampi(iv, 0, NSCALES - 1);
    else if (!strcmp(k, "rate")) set_rate(s, iv);
    else if (!strcmp(k, "len")) s->cur.len = clampi(iv, 1, NS);
    else if (!strcmp(k, "swing")) s->cur.swing = clampi(iv, 50, 75);
    else if (!strcmp(k, "gate")) s->cur.gate = clampi(iv, 50, 99);
    else if (!strcmp(k, "legato")) s->cur.legato = clampi(iv, 0, 2);
    else if (!strcmp(k, "transpose")) s->cur.transpose = clampi(iv, -24, 24);
    else if (!strcmp(k, "chan")) { if (clampi(iv - 1, 0, 15) != s->chan) { end_note(s); s->chan = clampi(iv - 1, 0, 15); } }
    else if (!strcmp(k, "pad_mode")) s->pad_mode = clampi(iv, 0, 1);
    else if (!strcmp(k, "pad_base")) {
        s->pad_base = clampi(iv, 0, 112);
        for (int j = 0; j < NS; j++) s->padmap[j] = (uint8_t)(s->pad_base + j);
    }
    else if (!strcmp(k, "learn")) s->learn = fv > 0.5 ? (s->learn ? s->learn : 1) : 0;
    else if (!strcmp(k, "chain_on")) {
        int on2 = fv > 0.5;
        if (on2 != s->chain_on) { s->chain_on = on2; s->chain_cur = -1; s->chain_hw = s->sent_to; }
    }
    else if (!strcmp(k, "chain_edit")) s->chain_edit = fv > 0.5;
    else if (!strcmp(k, "slide_cc")) { if (!(fv > 0.5)) porta_end(s); s->slide_cc = fv > 0.5; }
    else if (!strcmp(k, "entry")) {
        int e2 = fv > 0.5;
        if (e2 && !s->entry) { push_undo(s); s->cursor = 0; }
        s->entry = e2;
    }
    else if (!strcmp(k, "vmaster")) { if (clampi(iv, 0, 127) != s->vmaster) set_master(s, iv); return; }
    else if (!strcmp(k, "vm_mode")) s->vm_mode = clampi(iv, 0, 1);
    else if (!strcmp(k, "rand_lo")) s->rand_lo = clampi(iv, 2, 4);
    else if (!strcmp(k, "rand_hi")) s->rand_hi = clampi(iv, 3, 5);
    else if (fv > 0.5) {                                            /* trigger buttons */
        if (!strcmp(k, "random")) randomize(s);
        else if (!strcmp(k, "mutate")) mutate(s);
        else if (!strcmp(k, "shl")) shift_pattern(s, -1);
        else if (!strcmp(k, "shr")) shift_pattern(s, 1);
        else if (!strcmp(k, "reverse")) reverse_pattern(s);
        else if (!strcmp(k, "clear")) clear_pattern(s);
        else if (!strcmp(k, "undo")) do_undo(s);
        else if (!strcmp(k, "chain_back")) { if (s->chain_n) { s->chain_n--; s->chain_cur = -1; s->rev++; } }
        else if (!strcmp(k, "chain_clear")) { s->chain_n = 0; s->chain_cur = -1; s->rev++; }
        else if (!strcmp(k, "entry_rest")) {                         /* STEP ENTRY: a rest, then the next step */
            s->cur.st[s->cursor].on = 0;
            s->cursor = (s->cursor + 1) % clampi(s->cur.len, 1, NS);
            s->rev++;
        }
        else if (!strcmp(k, "entry_back")) { s->cursor = (s->cursor + clampi(s->cur.len, 1, NS) - 1) % clampi(s->cur.len, 1, NS); s->rev++; }
        else if (!strcmp(k, "vp_saw")) vel_preset(s, VP_SAW);
        else if (!strcmp(k, "vp_ramp")) vel_preset(s, VP_RAMP);
        else if (!strcmp(k, "vp_sine")) vel_preset(s, VP_SINE);
        else if (!strcmp(k, "vp_tri")) vel_preset(s, VP_TRI);
        else if (!strcmp(k, "vp_rand")) vel_preset(s, VP_RAND);
        return;
    } else return;
    s->rev++;
}

static void save_state(const ps_t *s, char *b, int n);

static int get_param(void *inst, const char *k, char *b, int n) {
    ps_t *s = inst;
    int on, i;
    const char *suf;
    char nn[8];
    if (!strcmp(k, "state")) { save_state(s, b, n); return (int)strlen(b) + 1; }
    if (!strcmp(k, "display_rev")) return snprintf(b, n, "%u", s->rev);
    if ((i = idx_key(k, "st_", &on, NULL)) >= 0) {
        if (on) {
            int lit = tile_lit(s, i);
            s->tile_rep[i] = (signed char)lit;
            return snprintf(b, n, "%d", lit);
        }
        note_name(s->cur.st[i].note, nn, sizeof nn);
        return snprintf(b, n, "%s", nn);
    }
    if ((i = idx_key(k, "sl_", &on, NULL)) >= 0) {
        if (on) {
            int lit = s->slot_used[i];
            s->slot_rep[i] = (signed char)lit;
            return snprintf(b, n, "%d", lit);
        }
        return snprintf(b, n, i == s->active_slot ? "[%d]" : "%d", i + 1);
    }
    if ((i = idx_key(k, "ph_", &on, NULL)) >= 0) {   /* the step playing; stopped in STEP ENTRY: the entry cursor */
        if (on) return snprintf(b, n, "%d", s->playing ? s->cur_pos == i : (s->entry && s->cursor == i));
        return snprintf(b, n, " ");
    }
    if ((i = idx_key(k, "tie_", &on, NULL)) >= 0) {
        if (on) { s->tie_rep[i] = (signed char)s->cur.st[i].tie; return snprintf(b, n, "%d", s->cur.st[i].tie); }
        return snprintf(b, n, "%s", s->cur.st[i].tie ? "TIE" : "-");
    }
    if ((i = idx_key(k, "sld_", &on, NULL)) >= 0) {
        if (on) { s->sld_rep[i] = (signed char)s->cur.st[i].slide; return snprintf(b, n, "%d", s->cur.st[i].slide); }
        return snprintf(b, n, "%s", s->cur.st[i].slide ? "SLD" : "-");
    }
    if ((i = idx_key(k, "rat_", &on, NULL)) >= 0) {
        int r = s->cur.st[i].rat > 1;
        if (on) { s->rat_rep[i] = (signed char)r; return snprintf(b, n, "%d", r); }
        return snprintf(b, n, "x%d", s->cur.st[i].rat);
    }
    if ((i = idx_key(k, "pr_", &on, &suf)) >= 0) {
        if (on) return 0;
        if (!strcmp(suf, "_display")) return snprintf(b, n, "%d%%", s->cur.st[i].prob);
        return snprintf(b, n, "%d", s->cur.st[i].prob);
    }
    if (!strcmp(k, "entry")) return snprintf(b, n, "%d", s->entry);
    if (!strcmp(k, "slide_cc")) return snprintf(b, n, "%d", s->slide_cc);
    if (!strcmp(k, "entry_rest") || !strcmp(k, "entry_back")) return snprintf(b, n, "0");
    if ((i = idx_key(k, "p_", &on, &suf)) >= 0) {
        if (!strcmp(suf, "_display")) { note_name(s->cur.st[i].note, nn, sizeof nn); return snprintf(b, n, "%s", nn); }
        if (on) return 0;
        return snprintf(b, n, "%d", s->cur.st[i].note);
    }
    if ((i = idx_key(k, "v_", &on, NULL)) >= 0) return on ? 0 : snprintf(b, n, "%d", s->cur.st[i].vel);
    if (!strcmp(k, "shift")) return snprintf(b, n, "%d", s->mode == MODE_SHIFT);
    if (!strcmp(k, "chanm")) return snprintf(b, n, "%d", s->mode == MODE_CHAN);
    if (!strcmp(k, "store")) return snprintf(b, n, "%d", s->mode == MODE_STORE);
    if (!strcmp(k, "recall")) return snprintf(b, n, "%d", s->mode == MODE_RECALL);
    if (!strcmp(k, "dir")) return snprintf(b, n, "%d", s->cur.dir);
    if (!strcmp(k, "scale")) return snprintf(b, n, "%d", s->cur.scale);
    if (!strcmp(k, "rate")) return snprintf(b, n, "%d", s->cur.rate);
    if (!strcmp(k, "len")) return snprintf(b, n, "%d", s->cur.len);
    if (!strcmp(k, "swing")) return snprintf(b, n, "%d", s->cur.swing);
    if (!strcmp(k, "gate")) return snprintf(b, n, "%d", s->cur.gate);
    if (!strcmp(k, "legato")) return snprintf(b, n, "%d", s->cur.legato);
    if (!strcmp(k, "transpose")) return snprintf(b, n, "%d", s->cur.transpose);
    if (!strcmp(k, "chan")) return snprintf(b, n, "%d", s->chan + 1);
    if (!strcmp(k, "pad_mode")) return snprintf(b, n, "%d", s->pad_mode);
    if (!strcmp(k, "pad_base")) return snprintf(b, n, "%d", s->pad_base);
    if (!strcmp(k, "learn")) return snprintf(b, n, "%d", s->learn > 0);
    if (!strcmp(k, "vmaster")) return snprintf(b, n, "%d", s->vmaster);
    if (!strcmp(k, "vm_mode")) return snprintf(b, n, "%d", s->vm_mode);
    if (!strncmp(k, "vp_", 3)) return snprintf(b, n, "0");
    if (!strcmp(k, "chain_on")) return snprintf(b, n, "%d", s->chain_on);
    if (!strcmp(k, "chain_edit")) return snprintf(b, n, "%d", s->chain_edit);
    if (!strcmp(k, "chain_back") || !strcmp(k, "chain_clear")) return snprintf(b, n, "0");
    if (!strcmp(k, "chain_view")) {   /* "3/8  1 2 [1] 3 ..": a window of up to 10 elements around the one playing */
        if (!s->chain_n) return snprintf(b, n, s->chain_edit ? "tap slots or pads to add" : "empty");
        int cur = s->chain_cur >= 0 && s->chain_on && s->playing ? s->chain_cur : -1;
        int first = cur > 4 ? cur - 4 : 0;
        if (first + 10 > s->chain_n) first = s->chain_n > 10 ? s->chain_n - 10 : 0;
        int j = cur >= 0 ? snprintf(b, n, "%d/%d ", cur + 1, s->chain_n) : snprintf(b, n, "%d: ", s->chain_n);
        if (first > 0 && j < n) j += snprintf(b + j, n - j, "..");
        for (int e = first; e < s->chain_n && e < first + 10 && j < n; e++)
            j += snprintf(b + j, n - j, e == cur ? " [%d]" : " %d", s->chain[e] + 1);
        if (first + 10 < s->chain_n && j < n) snprintf(b + j, n - j, " ..");
        return (int)strlen(b) + 1;
    }
    if (!strcmp(k, "rand_lo")) return snprintf(b, n, "%d", s->rand_lo);
    if (!strcmp(k, "rand_hi")) return snprintf(b, n, "%d", s->rand_hi);
    if (!strcmp(k, "random") || !strcmp(k, "mutate") || !strcmp(k, "shl") || !strcmp(k, "shr") || !strcmp(k, "reverse") ||
        !strcmp(k, "clear") || !strcmp(k, "undo")) return snprintf(b, n, "0");
    if (!strcmp(k, "info")) {           /* the BeatStep-style display line */
        static const char *MODE_HINT[] = { "", "SHIFT: pad = scale/mode/step", "CHAN: pad = channel", "STORE: pad = slot",
                                           "RECALL: pad = slot" };
        if (s->learn) return snprintf(b, n, "LEARN: hit pad %d", s->learn);
        if (s->chain_edit && !s->mode) return snprintf(b, n, "CHAIN EDIT: pad/slot = add");
        if (s->entry && !s->mode) return snprintf(b, n, "STEP ENTRY: step %d/%d, play a note", s->cursor + 1, clampi(s->cur.len, 1, NS));
        if (s->mode) return snprintf(b, n, "%s", MODE_HINT[s->mode]);
        return snprintf(b, n, "%s %s 1/%d Ch%d Tr%+d", SCALE_NAMES[s->cur.scale], DIR_NAMES[s->cur.dir],
                        RATE_DIV[s->cur.rate], s->chan + 1, s->cur.transpose);
    }
    if (!strcmp(k, "pad_info")) {
        if (s->learn) return snprintf(b, n, "LEARN pad %d", s->learn);
        int pi = -1;
        for (int j = 0; j < NS; j++) if (s->padmap[j] == s->last_in) { pi = j; break; }
        if (s->last_in < 0) return snprintf(b, n, "note -");
        if (pi < 0) return snprintf(b, n, "note %d", s->last_in);
        return snprintf(b, n, "note %d = pad %d", s->last_in, pi + 1);
    }
    return 0;
}

/* ------------------------------------------------------------------ state chunk */
static int put_snap(char *b, int n, const snap_t *c) {
    int j = snprintf(b, n, "%d,%d,%d,%d,%d,%d,%d,%d,", c->rate, c->len, c->dir, c->scale, c->swing, c->gate, c->legato, c->transpose);
    for (int i = 0; i < NS && j < n; i++) j += snprintf(b + j, n - j, "%02x%x%02x", c->st[i].note, c->st[i].on, c->st[i].vel);
    if (j < n) j += snprintf(b + j, n - j, "|");     /* per-step chance, tie, ratchet (older states: 100 %, no, 1) */
    for (int i = 0; i < NS && j < n; i++) j += snprintf(b + j, n - j, "%02x%x%x%x", c->st[i].prob, c->st[i].tie, c->st[i].rat, c->st[i].slide);
    return j;
}

static int hexn(const char *p, int digits, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < digits; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v * 16 + (unsigned)d;
    }
    *out = v;
    return 1;
}

static const char *get_snap(const char *p, snap_t *c) {
    int f[8], used = 0;
    if (sscanf(p, "%d,%d,%d,%d,%d,%d,%d,%d,%n", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &used) != 8) return NULL;
    snap_t t;
    t.rate = clampi(f[0], 0, NRATES - 1); t.len = clampi(f[1], 1, NS); t.dir = clampi(f[2], 0, 3); t.scale = clampi(f[3], 0, NSCALES - 1);
    t.swing = clampi(f[4], 50, 75); t.gate = clampi(f[5], 50, 99); t.legato = clampi(f[6], 0, 2); t.transpose = clampi(f[7], -24, 24);
    p += used;
    for (int i = 0; i < NS; i++, p += 5) {
        unsigned a, o, v;
        if (!hexn(p, 2, &a) || !hexn(p + 2, 1, &o) || !hexn(p + 3, 2, &v)) return NULL;
        t.st[i].note = (uint8_t)clampi((int)a, NOTE_LO, NOTE_HI); t.st[i].on = o ? 1 : 0; t.st[i].vel = (uint8_t)clampi((int)v, 1, 127);
        t.st[i].prob = 100; t.st[i].tie = 0; t.st[i].rat = 1; t.st[i].slide = 0;
    }
    if (*p == '|') {                                 /* 4 characters a step (v0.5) or 5 (with slide) */
        p++;
        int len = 0;
        while (p[len] && p[len] != ' ') len++;
        int w = len >= NS * 5 ? 5 : 4;
        for (int i = 0; i < NS; i++, p += w) {
            unsigned pr, ti, ra, sl = 0;
            if (!hexn(p, 2, &pr) || !hexn(p + 2, 1, &ti) || !hexn(p + 3, 1, &ra) || (w == 5 && !hexn(p + 4, 1, &sl))) break;
            t.st[i].prob = (uint8_t)clampi((int)pr, 0, 100); t.st[i].tie = ti ? 1 : 0; t.st[i].rat = (uint8_t)clampi((int)ra, 1, 4);
            t.st[i].slide = sl ? 1 : 0;
        }
    }
    *c = t;
    return p;
}

static void save_state(const ps_t *s, char *b, int n) {
    int j = snprintf(b, n, "PS16v1 ch=%d pm=%d pb=%d rl=%d rh=%d as=%d cur=", s->chan, s->pad_mode, s->pad_base, s->rand_lo,
                     s->rand_hi, s->active_slot);
    j += put_snap(b + j, n - j, &s->cur);
    if (j < n) j += snprintf(b + j, n - j, " vm=%d,%d sc=%d", s->vmaster, s->vm_mode, s->slide_cc);
    if (j < n) j += snprintf(b + j, n - j, " chain=%d,", s->chain_on);
    for (int i = 0; i < s->chain_n && j < n; i++) j += snprintf(b + j, n - j, "%x", s->chain[i]);
    if (j < n) j += snprintf(b + j, n - j, " map=");
    for (int i = 0; i < NS && j < n; i++) j += snprintf(b + j, n - j, "%02x", s->padmap[i]);
    for (int i = 0; i < NS && j < n; i++)
        if (s->slot_used[i]) { j += snprintf(b + j, n - j, " s%d=", i); if (j < n) j += put_snap(b + j, n - j, &s->slot[i]); }
}

static void load_state(ps_t *s, const char *str) {
    if (strncmp(str, "PS16v1 ", 7)) return;
    int ch, pm, pb, rl, rh, as, used = 0;
    if (sscanf(str, "PS16v1 ch=%d pm=%d pb=%d rl=%d rh=%d as=%d cur=%n", &ch, &pm, &pb, &rl, &rh, &as, &used) != 6 || !used) return;
    snap_t c;
    if (!get_snap(str + used, &c)) return;
    end_note(s);
    s->rat_left = 0; s->prev_tied = s->prev_slide = 0; porta_end(s);
    s->chan = clampi(ch, 0, 15); s->pad_mode = clampi(pm, 0, 1); s->pad_base = clampi(pb, 0, 112);
    s->rand_lo = clampi(rl, 2, 4); s->rand_hi = clampi(rh, 3, 5); s->active_slot = clampi(as, -1, NS - 1);
    s->cur = c;
    memset(s->slot_used, 0, sizeof s->slot_used);
    for (int i = 0; i < NS; i++) {
        char tag[12];
        snprintf(tag, sizeof tag, " s%d=", i);
        const char *p = strstr(str, tag);
        if (p && get_snap(p + strlen(tag), &s->slot[i])) s->slot_used[i] = 1;
    }
    const char *vp = strstr(str, " vm=");
    int vm = 100, vmm = 0;
    if (vp) sscanf(vp, " vm=%d,%d", &vm, &vmm);
    s->vmaster = clampi(vm, 0, 127); s->vm_mode = clampi(vmm, 0, 1); s->vref = 0;
    const char *scp = strstr(str, " sc=");
    s->slide_cc = scp ? atoi(scp + 4) != 0 : 0;
    const char *cp = strstr(str, " chain=");
    s->chain_n = 0; s->chain_on = 0; s->chain_cur = -1;
    if (cp) {
        int con = 0, used2 = 0;
        if (sscanf(cp, " chain=%d,%n", &con, &used2) == 1 && used2) {
            s->chain_on = con ? 1 : 0;
            for (const char *q = cp + used2; s->chain_n < NCHAIN; q++) {
                unsigned u2;
                if (!hexn(q, 1, &u2)) break;
                s->chain[s->chain_n++] = (uint8_t)u2;
            }
        }
    }
    const char *mp = strstr(str, " map=");
    unsigned u;
    if (mp) for (int i = 0; i < NS && hexn(mp + 5 + 2 * i, 2, &u); i++) s->padmap[i] = (uint8_t)(u & 127);
    else for (int i = 0; i < NS; i++) s->padmap[i] = (uint8_t)(s->pad_base + i);
    s->nundo = 0;
    s->rev++;
}

/* ------------------------------------------------------------------ life cycle */
static void *create(const char *dir) {
    (void)dir;
    ps_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    static const uint8_t demo[NS] = { 57, 60, 64, 57, 67, 64, 60, 62, 57, 60, 64, 69, 67, 64, 62, 60 };   /* A minor-ish */
    for (int i = 0; i < NS; i++) {
        s->cur.st[i].note = demo[i]; s->cur.st[i].on = 1; s->cur.st[i].vel = (i & 3) ? 90 : 120;
        s->cur.st[i].prob = 100; s->cur.st[i].rat = 1;
    }
    s->cur.rate = 2; s->cur.len = NS; s->cur.dir = DIR_FWD; s->cur.scale = 0; s->cur.swing = 50; s->cur.gate = 50;
    s->pad_base = 24; s->rand_lo = 3; s->rand_hi = 4; s->active_slot = -1;
    s->cur_pos = -1; s->sounding = -1; s->last_in = -1; s->chain_cur = -1;
    s->vmaster = 100;
    for (int i = 0; i < NS; i++) s->padmap[i] = (uint8_t)(24 + i);   /* MPC One: pad 1 sends 24 */
    memset(s->tile_rep, -1, sizeof s->tile_rep);
    memset(s->slot_rep, -1, sizeof s->slot_rep);
    memset(s->tie_rep, -1, sizeof s->tie_rep);
    memset(s->rat_rep, -1, sizeof s->rat_rep);
    memset(s->sld_rep, -1, sizeof s->sld_rep);
    s->rng = 0x2545f491u;
    s->rev = 1;
    midiport_open(&s->out, s, "PadStep 16", "MIDI Out");
    return s;
}

static void destroy(void *inst) {
    ps_t *s = inst;
    end_note(s);
    if (s->prev.frames > 0) midiport_send(&s->out, 0x80 | s->prev.ch, s->prev.note, 0);
    midiport_close(&s->out);
    free(s);
}

static void render(void *inst, int16_t *out, int frames) { (void)inst; memset(out, 0, (size_t)frames * 4); }

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, song_pos };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
