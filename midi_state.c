/* MIDI's state (midi.h): what the keys and knobs are doing, its writers
 * and the natives a patch reads it through. One writer (the MIDI thread,
 * a script, an exported patch's host), many readers (the audio thread's
 * ticks): every field is atomic and independent, and a reader that
 * catches a note mid-update hears one stale sample at most, since keys
 * move at under 100 Hz and audio reads at 48 kHz. No ALSA here: an
 * exported patch carries this file in its runtime (aither export).
 * The sequencer, which feeds it from ALSA, is midi.c. */

#include <math.h>
#include <stdatomic.h>
#include "midi.h"
#include "dsp.h"

typedef struct {
  _Atomic int    note;        /* 0..127, or -1 if never used */
  _Atomic double velocity;    /* > 0 held; 0 released or empty */
  _Atomic double struck;      /* the note-on velocity, kept after release */
  _Atomic long   on_at;       /* the note-on counter at allocation */
} HeldNote;

/* The polyphonic table: voice n in 1..MIDI_MAX_POLYPHONY is held[n-1].
 * A new note re-triggers its own slot if held, else takes the first
 * empty or released slot (a released one may still be ringing), else
 * steals the oldest (SPEC.md, "Voice stealing"). */
static struct {
  _Atomic double   cc[128];
  _Atomic double   notes[128];
  _Atomic unsigned trigs[128];   /* a monotonic counter per note */
  _Atomic double   last_freq;
  _Atomic double   last_gate;
  _Atomic int      last_note;    /* which note holds the gate */
  _Atomic double   last_vel;     /* the latest note-on velocity, kept */
  HeldNote         held[MIDI_MAX_POLYPHONY];
  _Atomic long     on_counter;
} st = {.last_note = 0, .held = {[0 ... MIDI_MAX_POLYPHONY - 1] = {.note = -1}}};

static double note_hz(int note) { return 440.0 * pow(2.0, ((double)note - 69.0) / 12.0); }

/* ------------------------------------------------------------- the writer */

static void allocate_slot(int note, double vel, long on_at) {
  /* Stage a held note. The slot's on_at and velocity go first and its
   * note last, so a reader mid-write sees the old slot whole or the new
   * note coherent. */
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)       /* the same note, held: re-trigger */
    if (st.held[i].note == note && st.held[i].velocity > 0.0) {
      st.held[i].on_at = on_at;
      st.held[i].struck = vel;
      st.held[i].velocity = vel;
      return;
    }
  /* The first empty or released slot, by index, so voices stay packed
   * at the low end. */
  int slot = -1;
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
  st.held[slot].on_at = on_at;
  st.held[slot].struck = vel;
  st.held[slot].velocity = vel;
  st.held[slot].note = note;
}

void midi_note_on(int note, int velocity) {
  if (note < 0 || note >= 128) return;
  double vel = (double)velocity / 127.0;
  st.notes[note] = vel;
  st.last_freq = note_hz(note);
  st.last_gate = vel;
  st.last_vel = vel;
  st.last_note = note;
  atomic_fetch_add(&st.trigs[note], 1u);
  long on_at = atomic_fetch_add(&st.on_counter, 1) + 1;
  allocate_slot(note, vel, on_at);
}

void midi_note_off(int note) {
  if (note < 0 || note >= 128) return;
  st.notes[note] = 0.0;
  /* Only the latest note-on's own note-off drops the mono gate, so a
   * stale one can't kill a newer held note. */
  if (st.last_note == note) st.last_gate = 0.0;
  /* The slot keeps its note, so a release tail still reads its pitch,
   * and is free for the next allocation. */
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    if (st.held[i].note == note && st.held[i].velocity > 0.0) st.held[i].velocity = 0.0;
}

/* Controllers 0..31 are 14-bit when a sender pairs them (MIDI 1.0): n
 * carries the coarse 7 bits, n + 32 the fine. The writer keeps each pair
 * whole here. An MSB alone takes itself as its LSB, (m << 7) | m, which
 * is m / 127 exactly (m * 129 / 16383), so a 7-bit knob still spans 0..1
 * and an old patch reads what it always did; MIDI 1.0 says an MSB resets
 * the LSB to 0, which would stop a 7-bit knob at 0.994. A 14-bit sender
 * sends the LSB straight after, and from then the value is exact. */
static int pair[32];

void midi_cc(int cc, int value) {
  if (cc < 0 || cc >= 128) return;
  value = value < 0 ? 0 : value > 127 ? 127 : value;
  if (cc < 32) {
    pair[cc] = value << 7 | value;
    st.cc[cc] = pair[cc] / 16383.0;
    return;
  }
  st.cc[cc] = (double)value / 127.0;          /* a fine half reads on its own too */
  if (cc < 64) {
    pair[cc - 32] = (pair[cc - 32] & ~127) | value;
    st.cc[cc - 32] = pair[cc - 32] / 16383.0;
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
  st.last_vel = 0.0;
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++) {
    st.held[i].note = -1;
    st.held[i].velocity = 0.0;
    st.held[i].struck = 0.0;
    st.held[i].on_at = 0;
  }
  st.on_counter = 0;
}

/* ------------------------------------------------------------- the natives */

double n_midi_cc(int n) { return n < 0 || n >= 128 ? 0.0 : st.cc[n]; }
double n_midi_note(int n) { return n < 0 || n >= 128 ? 0.0 : st.notes[n]; }
double n_midi_freq(void) { return st.last_freq; }

/* 1 while the most recent note is held, 0 after its note-off: a gate,
 * not a level, so `adsr(midi_gate(), ...)` sounds however softly the
 * key was played. The velocity is midi_vel(). */
double n_midi_gate(void) { return st.last_gate > 0.0 ? 1.0 : 0.0; }

/* The latest note-on's velocity, 0..1. It stays after the note-off, so
 * a release tail scaled by it doesn't cut off. */
double n_midi_vel(void) { return st.last_vel; }

/* The nth held voice (1-based, as sum's index): its pitch, 0 if the slot
 * never held a note. A released slot keeps it, for its release tail. */
double n_midi_voice_freq(int n) {
  if (n < 1 || n > MIDI_MAX_POLYPHONY) return 0.0;
  int note = st.held[n - 1].note;
  return note < 0 ? 0.0 : note_hz(note);
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

