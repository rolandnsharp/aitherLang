/* MIDI's state (midi.h): what the keys and knobs are doing, its writers
 * and the natives a patch reads it through. One writer (the MIDI thread,
 * a script, an exported patch's host), many readers (the audio thread's
 * ticks): every field is atomic and independent, and a reader that
 * catches a note mid-update hears one stale sample at most, since keys
 * move at under 100 Hz and audio reads at 48 kHz. No ALSA here: an
 * exported patch carries this file in its runtime (aither export).
 * The sequencer, which feeds it from ALSA, is midi.c.
 *
 * Each held note carries its own expression: its channel, its pitch bend,
 * its pressure and its own controllers. A keyboard plays every note on one
 * channel, so its notes all read that channel's knobs, as before. An MPE
 * controller (MIDI Polyphonic Expression, MMA RP-053, 2018) gives each
 * note a channel of its own, so the bend, pressure and slide (CC 74) sent
 * on it belong to that note alone; a program can also set a held note's
 * values directly (midi_note_cc), at full resolution. */

#include <math.h>
#include <stdatomic.h>
#include "midi.h"
#include "dsp.h"

typedef struct {
  _Atomic int    note;        /* 0..127, or -1 if never used */
  _Atomic int    ch;          /* the channel it was played on, 0..15 */
  _Atomic double velocity;    /* > 0 held; 0 released or empty */
  _Atomic double struck;      /* the note-on velocity, kept after release */
  _Atomic long   on_at;       /* the note-on counter at allocation */
  _Atomic double bend;        /* its own bend, semitones (its channel's, or set for it) */
  _Atomic double press;       /* its pressure, 0..1 */
  _Atomic double cc[128];     /* its controllers, 0..1 */
} HeldNote;

/* What each channel last said: its controllers (paired into 14 bits as
 * the global table is), its bend and pressure, its bend range, and the
 * registered parameter it is setting. A note takes its channel's values
 * when it starts, so an MPE controller's bend and slide sent just before
 * the note-on are the note's first. */
typedef struct {
  _Atomic double cc[128];
  int            pair[32];
  _Atomic double bend;        /* semitones */
  _Atomic double press;
  double         range;       /* semitones at full bend */
  int            raw_bend;    /* the last bend, 0..16383, to rescale when the range changes */
  int            rpn_msb, rpn_lsb;
} Channel;

/* The polyphonic table: voice n in 1..MIDI_MAX_POLYPHONY is held[n-1].
 * A new note re-triggers its own slot if held (the same key on the same
 * channel), else takes the first empty or released slot (a released one
 * may still be ringing), else steals the oldest (SPEC.md, "Voice
 * stealing"). */
static struct {
  _Atomic double   cc[128];
  _Atomic double   notes[128];
  _Atomic unsigned trigs[128];   /* a monotonic counter per note */
  _Atomic double   last_freq;    /* the latest note's pitch, unbent */
  _Atomic double   last_gate;
  _Atomic int      last_note;    /* which note holds the gate */
  _Atomic int      last_ch;
  _Atomic double   last_vel;     /* the latest note-on velocity, kept */
  HeldNote         held[MIDI_MAX_POLYPHONY];
  _Atomic long     on_counter;
} st = {.last_note = 0, .held = {[0 ... MIDI_MAX_POLYPHONY - 1] = {.note = -1}}};

static Channel chans[16];
static int pair[32];                 /* the global table's pairs */
/* MPE zones (MMA RP-053): the lower zone's master is channel 1 (0 here)
 * and its members the next `lower` channels; the upper zone's master is
 * channel 16 and its members the `upper` below it. 0: no zone. */
static int lower, upper;
static int ranges_set;

static double note_hz(double note) { return 440.0 * pow(2.0, (note - 69.0) / 12.0); }

/* Every channel as MIDI starts it: bends two semitones, centred, no
 * registered parameter chosen (127/127 is RPN null). */
static void default_ranges(void) {
  if (ranges_set) return;
  for (int c = 0; c < 16; c++) {
    chans[c].range = 2.0;
    chans[c].raw_bend = 8192;
    chans[c].rpn_msb = chans[c].rpn_lsb = 127;
  }
  ranges_set = 1;
}

/* The zone master whose bend moves channel ch's notes too, or -1. */
static int master_of(int ch) {
  if (lower && ch >= 1 && ch <= lower) return 0;
  if (upper && ch <= 14 && ch >= 15 - upper) return 15;
  return -1;
}

/* A note's whole bend: its own, and its zone master's. */
static double bend_of(int ch, double own) {
  int m = master_of(ch);
  return own + (m >= 0 ? chans[m].bend : 0.0);
}

/* ------------------------------------------------------------- the writer */

static void allocate_slot(int ch, int note, double vel, long on_at) {
  /* Stage a held note. The slot's expression, on_at and velocity go
   * first and its note last, so a reader mid-write sees the old slot
   * whole or the new note coherent. */
  Channel* c = &chans[ch];
  int slot = -1;
  for (int i = 0; i < MIDI_MAX_POLYPHONY && slot < 0; i++)    /* the same note, held: re-trigger */
    if (st.held[i].note == note && st.held[i].ch == ch && st.held[i].velocity > 0.0) slot = i;
  /* The first empty or released slot, by index, so voices stay packed
   * at the low end. */
  for (int i = 0; i < MIDI_MAX_POLYPHONY && slot < 0; i++)
    if (st.held[i].velocity == 0.0) slot = i;
  if (slot < 0) {                                    /* all held: evict the oldest */
    slot = 0;
    long oldest = st.held[0].on_at;
    for (int i = 1; i < MIDI_MAX_POLYPHONY; i++) {
      long t = st.held[i].on_at;
      if (t < oldest) {
        oldest = t;
        slot = i;
      }
    }
  }
  HeldNote* h = &st.held[slot];
  for (int k = 0; k < 128; k++) h->cc[k] = c->cc[k];
  h->bend = c->bend;
  h->press = c->press;
  h->ch = ch;
  h->on_at = on_at;
  h->struck = vel;
  h->velocity = vel;
  h->note = note;
}

void midi_note_on_ch(int ch, int note, int velocity) {
  if (note < 0 || note >= 128 || ch < 0 || ch > 15) return;
  if (velocity <= 0) {
    midi_note_off_ch(ch, note);
    return;
  }
  default_ranges();
  double vel = (double)(velocity > 127 ? 127 : velocity) / 127.0;
  st.notes[note] = vel;
  st.last_freq = note_hz(note);
  st.last_gate = vel;
  st.last_vel = vel;
  st.last_ch = ch;
  st.last_note = note;
  atomic_fetch_add(&st.trigs[note], 1u);
  long on_at = atomic_fetch_add(&st.on_counter, 1) + 1;
  allocate_slot(ch, note, vel, on_at);
}

void midi_note_off_ch(int ch, int note) {
  if (note < 0 || note >= 128 || ch < 0 || ch > 15) return;
  st.notes[note] = 0.0;
  /* Only the latest note-on's own note-off drops the mono gate, so a
   * stale one can't kill a newer held note. */
  if (st.last_note == note && st.last_ch == ch) st.last_gate = 0.0;
  /* The slot keeps its note and its expression, so a release tail still
   * reads its pitch, and is free for the next allocation. */
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    if (st.held[i].note == note && st.held[i].ch == ch && st.held[i].velocity > 0.0) st.held[i].velocity = 0.0;
}

void midi_note_on(int note, int velocity) { midi_note_on_ch(0, note, velocity); }
void midi_note_off(int note) { midi_note_off_ch(0, note); }

/* Controllers 0..31 are 14-bit when a sender pairs them (MIDI 1.0): n
 * carries the coarse 7 bits, n + 32 the fine. The writer keeps each pair
 * whole here. An MSB alone takes itself as its LSB, (m << 7) | m, which
 * is m / 127 exactly (m * 129 / 16383), so a 7-bit knob still spans 0..1
 * and an old patch reads what it always did; MIDI 1.0 says an MSB resets
 * the LSB to 0, which would stop a 7-bit knob at 0.994. A 14-bit sender
 * sends the LSB straight after, and from then the value is exact. Each
 * channel pairs its own, and the global table pairs whatever comes. */
static void paired(_Atomic double* table, int* pairs, int cc, int value) {
  if (cc < 32) {
    pairs[cc] = value << 7 | value;
    table[cc] = pairs[cc] / 16383.0;
    return;
  }
  table[cc] = (double)value / 127.0;          /* a fine half reads on its own too */
  if (cc < 64) {
    pairs[cc - 32] = (pairs[cc - 32] & ~127) | value;
    table[cc - 32] = pairs[cc - 32] / 16383.0;
  }
}

/* Its notes held now take a channel's new value; released ones keep
 * theirs for their tails (the channel may already belong to another). */
static void to_notes_of(int ch, int cc) {
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    if (st.held[i].ch == ch && st.held[i].velocity > 0.0) {
      st.held[i].cc[cc] = chans[ch].cc[cc];
      if (cc < 64 && cc >= 32) st.held[i].cc[cc - 32] = chans[ch].cc[cc - 32];
    }
}

/* A channel's bend range, and its bend rescaled to it, for the channel
 * and the notes it holds. */
static void set_range(int ch, double semitones) {
  chans[ch].range = semitones;
  chans[ch].bend = (chans[ch].raw_bend - 8192) / 8192.0 * semitones;
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    if (st.held[i].ch == ch && st.held[i].velocity > 0.0) st.held[i].bend = chans[ch].bend;
}

/* Registered parameters (CC 101/100 choose one, CC 6/38 set it): 0, the
 * pitch bend range in semitones and cents; 6, MPE's configuration, sent
 * on a zone's master with the count of member channels in CC 6. A new
 * zone's members bend 48 semitones and its master 2, as MPE says. */
static void rpn_data(int ch, int msb_cc, int value) {
  Channel* c = &chans[ch];
  if (c->rpn_msb != 0) return;
  if (c->rpn_lsb == 0) {
    double semis = msb_cc ? value : floor(c->range);
    double cents = msb_cc ? 0 : value;
    set_range(ch, semis + cents / 100.0);
  } else if (c->rpn_lsb == 6 && msb_cc && (ch == 0 || ch == 15)) {
    int m = value > 15 ? 15 : value;
    if (ch == 0) lower = m;
    else upper = m;
    set_range(ch, 2.0);
    for (int k = 1; k <= m; k++) set_range(ch == 0 ? k : 15 - k, 48.0);
  }
}

void midi_cc_ch(int ch, int cc, int value) {
  if (cc < 0 || cc >= 128 || ch < 0 || ch > 15) return;
  default_ranges();
  value = value < 0 ? 0 : value > 127 ? 127 : value;
  paired(st.cc, pair, cc, value);
  Channel* c = &chans[ch];
  paired(c->cc, c->pair, cc, value);
  if (cc == 101) c->rpn_msb = value;
  else if (cc == 100) c->rpn_lsb = value;
  else if (cc == 6 || cc == 38) rpn_data(ch, cc == 6, value);
  to_notes_of(ch, cc);
}

void midi_cc(int cc, int value) { midi_cc_ch(0, cc, value); }

void midi_bend_ch(int ch, int value) {
  if (ch < 0 || ch > 15) return;
  default_ranges();
  value = value < 0 ? 0 : value > 16383 ? 16383 : value;
  chans[ch].raw_bend = value;
  chans[ch].bend = (value - 8192) / 8192.0 * chans[ch].range;
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    if (st.held[i].ch == ch && st.held[i].velocity > 0.0) st.held[i].bend = chans[ch].bend;
}

void midi_pressure_ch(int ch, int value) {
  if (ch < 0 || ch > 15) return;
  value = value < 0 ? 0 : value > 127 ? 127 : value;
  chans[ch].press = value / 127.0;
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    if (st.held[i].ch == ch && st.held[i].velocity > 0.0) st.held[i].press = chans[ch].press;
}

/* A held note's own values, set directly by a program (the export
 * runtime, a game): every slot holding that key, on any channel. Full
 * resolution: a controller 0..1, a bend in semitones, a pressure 0..1. */
void midi_note_cc(int note, int cc, double value) {
  if (cc < 0 || cc >= 128) return;
  value = value < 0 ? 0 : value > 1 ? 1 : value;
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    if (st.held[i].note == note && st.held[i].velocity > 0.0) st.held[i].cc[cc] = value;
}

void midi_note_bend(int note, double semitones) {
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    if (st.held[i].note == note && st.held[i].velocity > 0.0) st.held[i].bend = semitones;
}

void midi_note_pressure(int note, double value) {
  value = value < 0 ? 0 : value > 1 ? 1 : value;
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    if (st.held[i].note == note && st.held[i].velocity > 0.0) st.held[i].press = value;
}

/* Raw MIDI 1.0 bytes, one message (running status not kept): note on and
 * off, controllers, channel pressure and pitch bend, on their channels.
 * For a host that has MIDI in hand already (a plugin, a game's own
 * device code). */
void midi_message(const unsigned char* m, int len) {
  if (len < 2 || !(m[0] & 0x80)) return;
  int ch = m[0] & 15, a = m[1] & 127, b = len > 2 ? m[2] & 127 : 0;
  switch (m[0] & 0xF0) {
    case 0x80: if (len >= 3) midi_note_off_ch(ch, a); break;
    case 0x90: if (len >= 3) midi_note_on_ch(ch, a, b); break;
    case 0xB0: if (len >= 3) midi_cc_ch(ch, a, b); break;
    case 0xD0: midi_pressure_ch(ch, a); break;
    case 0xE0: if (len >= 3) midi_bend_ch(ch, a | b << 7); break;
    default: break;
  }
}

void midi_reset_all(void) {
  for (int i = 0; i < 32; i++) pair[i] = 0;
  for (int i = 0; i < 128; i++) {
    st.cc[i] = 0.0;
    st.notes[i] = 0.0;
    st.trigs[i] = 0;
  }
  st.last_freq = 0.0;
  st.last_gate = 0.0;
  st.last_note = -1;
  st.last_ch = 0;
  st.last_vel = 0.0;
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++) {
    HeldNote* h = &st.held[i];
    h->note = -1;
    h->ch = 0;
    h->velocity = 0.0;
    h->struck = 0.0;
    h->on_at = 0;
    h->bend = 0.0;
    h->press = 0.0;
    for (int k = 0; k < 128; k++) h->cc[k] = 0.0;
  }
  for (int c = 0; c < 16; c++) {
    Channel* ch = &chans[c];
    for (int k = 0; k < 128; k++) ch->cc[k] = 0.0;
    for (int k = 0; k < 32; k++) ch->pair[k] = 0;
    ch->bend = 0.0;
    ch->press = 0.0;
    ch->raw_bend = 8192;
    ch->rpn_msb = ch->rpn_lsb = 127;
    ch->range = 2.0;
  }
  ranges_set = 1;
  lower = upper = 0;
  st.on_counter = 0;
}

/* ------------------------------------------------------------- the natives */

double n_midi_cc(int n) { return n < 0 || n >= 128 ? 0.0 : st.cc[n]; }
double n_midi_note(int n) { return n < 0 || n >= 128 ? 0.0 : st.notes[n]; }

/* The latest note's pitch, bent by its channel (and zone master). */
double n_midi_freq(void) {
  double f = st.last_freq;
  return f > 0.0 ? f * pow(2.0, bend_of(st.last_ch, chans[st.last_ch].bend) / 12.0) : 0.0;
}

/* 1 while the most recent note is held, 0 after its note-off: a gate,
 * not a level, so `adsr(midi_gate(), ...)` sounds however softly the
 * key was played. The velocity is midi_vel(). */
double n_midi_gate(void) { return st.last_gate > 0.0 ? 1.0 : 0.0; }

/* The latest note-on's velocity, 0..1. It stays after the note-off, so
 * a release tail scaled by it doesn't cut off. */
double n_midi_vel(void) { return st.last_vel; }

/* The nth held voice (1-based, as sum's index): its pitch, bent by its
 * own bend and its zone master's, 0 if the slot never held a note. A
 * released slot keeps it, for its release tail. */
double n_midi_voice_freq(int n) {
  if (n < 1 || n > MIDI_MAX_POLYPHONY) return 0.0;
  HeldNote* h = &st.held[n - 1];
  int note = h->note;
  return note < 0 ? 0.0 : note_hz(note + bend_of(h->ch, h->bend));
}

/* 1 while the nth voice's key is held, 0 after release or when empty. */
double n_midi_voice_gate(int n) {
  if (n < 1 || n > MIDI_MAX_POLYPHONY) return 0.0;
  return st.held[n - 1].velocity > 0.0 ? 1.0 : 0.0;
}

/* The velocity the nth voice was struck at; it stays after release, so
 * `swell(gate, a, r) * vel` keeps its tail. */
double n_midi_voice_vel(int n) {
  if (n < 1 || n > MIDI_MAX_POLYPHONY) return 0.0;
  return st.held[n - 1].struck;
}

/* The nth voice's key, 0..127; it stays after release, 0 if never held. */
double n_midi_voice_key(int n) {
  if (n < 1 || n > MIDI_MAX_POLYPHONY) return 0.0;
  int note = st.held[n - 1].note;
  return note < 0 ? 0.0 : (double)note;
}

/* The nth voice's own controller k, 0..1: its channel's, or set for it. */
double n_midi_voice_cc(int n, int k) {
  if (n < 1 || n > MIDI_MAX_POLYPHONY || k < 0 || k >= 128) return 0.0;
  return st.held[n - 1].cc[k];
}

/* The nth voice's pressure, 0..1 (MPE's third dimension). */
double n_midi_voice_press(int n) {
  if (n < 1 || n > MIDI_MAX_POLYPHONY) return 0.0;
  return st.held[n - 1].press;
}

/* Which note-on the nth voice is: a count that moves on every note-on its
 * slot takes (0: never used), so a voice can tell a new note from the
 * one before, even on the same key (poly's retiring uses it). */
double n_midi_voice_on(int n) {
  if (n < 1 || n > MIDI_MAX_POLYPHONY) return 0.0;
  return (double)st.held[n - 1].on_at;
}

/* The nth voice's bend in semitones, its own and its zone master's. */
double n_midi_voice_bend(int n) {
  if (n < 1 || n > MIDI_MAX_POLYPHONY) return 0.0;
  HeldNote* h = &st.held[n - 1];
  return h->note < 0 ? 0.0 : bend_of(h->ch, h->bend);
}

/* An edge per voice: its pool slot holds the last counter it saw for
 * note n, and the sample the counter moves on is 1. Every reader in the
 * same sample sees the edge. */
double n_midi_trig(void* state, long at, int n) {
  DspState* s = state;
  if (n < 0 || n >= 128) return 0.0;
  if (at + 1 > s->pool_slots - POOL_OVERFLOW_SLOTS) {
    s->overflow = 1;
    return 0.0;
  }
  long i = at;
  double cur = (double)st.trigs[n];
  double prev = s->pool[i];
  s->pool[i] = cur;
  return cur > prev ? 1.0 : 0.0;
}
