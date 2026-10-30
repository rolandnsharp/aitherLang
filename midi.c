/* MIDI (midi.h). One writer (the MIDI thread), many readers (the audio
 * thread's ticks): every field is atomic and independent, and a reader
 * that catches a note mid-update hears one stale sample at most, since
 * keys move at under 100 Hz and audio reads at 48 kHz. */

#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "midi.h"
#include "dsp.h"

typedef void (*aither_note_on_fn)(int note, int velocity);   /* alsa_midi.c */
typedef void (*aither_note_off_fn)(int note);
typedef void (*aither_cc_fn)(int cc, int value);
int  aither_alsa_open(const char* client_name);
int  aither_alsa_connect_from(int client, int port);
int  aither_alsa_list_ports(char* buf, int bufsize);
int  aither_alsa_auto_connect(char* outbuf, int outsize);
int  aither_alsa_parse_address(const char* spec, int* client, int* port);
void aither_alsa_run(aither_note_on_fn on, aither_note_off_fn off, aither_cc_fn cc);
void aither_alsa_close(void);

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

void midi_cc(int cc, int value) {
  if (cc < 0 || cc >= 128) return;
  st.cc[cc] = (double)value / 127.0;
}

void midi_reset_all(void) {
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

/* ------------------------------------------------------------ the sequencer */
/* The thread blocks in alsa_midi.c's event loop and calls the writer. The
 * last subscription is remembered, so a loop that exits unexpectedly
 * (BUGS_AND_ISSUES.md 4c: a keyboard that silently stops after a few
 * reloads while `midi list` still shows it) can be resubscribed. */

static pthread_t thread;
static int thread_started;
static atomic_int thread_active;    /* true before the loop runs, false once it ends */
static atomic_int shutting_down;    /* an intended close: no "dropped" line */
static char last_info[512];         /* who, for `list` */
static int last_client = -1, last_port = -1;

static void* run_loop(void* arg) {
  (void)arg;
  aither_alsa_run(midi_note_on, midi_note_off, midi_cc);
  thread_active = 0;
  if (!shutting_down)
    fprintf(stderr, "[aither] MIDI subscription dropped — next `aither send` will attempt auto-resubscribe\n");
  return NULL;
}

int midi_open(const char* client_name) { return aither_alsa_open(client_name) == 0; }

char* midi_list_ports(void) {
  char buf[4096] = "";
  return strdup(aither_alsa_list_ports(buf, sizeof buf) > 0 ? buf : "");
}

char* midi_auto_connect(void) {
  char buf[256] = "";
  if (aither_alsa_auto_connect(buf, sizeof buf) != 0) return strdup("");
  snprintf(last_info, sizeof last_info, "%s", buf);
  /* The "name (client:port)" trailer, so a resubscribe needn't search again. */
  char* lp = strrchr(buf, '(');
  char* rp = strrchr(buf, ')');
  if (lp && lp > buf && rp > lp + 1) {
    char* colon = memchr(lp + 1, ':', rp - lp - 1);
    if (colon && colon > lp + 1) {
      char *e1, *e2;
      long c = strtol(lp + 1, &e1, 10), p = strtol(colon + 1, &e2, 10);
      if (e1 == colon && e2 == rp) {
        last_client = (int)c;
        last_port = (int)p;
      }
    }
  }
  return strdup(buf);
}

int midi_connect(const char* spec) {
  int client, port;
  if (aither_alsa_parse_address(spec, &client, &port) != 0) return 0;
  if (aither_alsa_connect_from(client, port) != 0) return 0;
  last_client = client;
  last_port = port;
  snprintf(last_info, sizeof last_info, "%s (%d:%d)", spec, client, port);
  return 1;
}

void midi_start_thread(void) {
  if (thread_started) return;
  shutting_down = 0;
  thread_active = 1;
  pthread_create(&thread, NULL, run_loop, NULL);
  thread_started = 1;
}

void midi_shutdown(void) {
  /* Close the sequencer, which unblocks the thread, and join it. */
  if (!thread_started) return;
  shutting_down = 1;
  aither_alsa_close();
  pthread_join(thread, NULL);
  thread_started = 0;
}

int midi_subscription_active(void) { return thread_active; }
const char* midi_last_connect_info(void) { return last_info; }

char* midi_resubscribe_if_dropped(void) {
  if (thread_active || last_client < 0) return NULL;
  midi_shutdown();                   /* the thread is gone; the sequencer may be open */
  char* r;
  if (!midi_open("aither")) {
    r = strdup("fail: ALSA seq open");
  } else if (aither_alsa_connect_from(last_client, last_port) != 0) {
    if (asprintf(&r, "fail: subscribe to %d:%d", last_client, last_port) < 0) r = NULL;
  } else {
    midi_start_thread();
    if (asprintf(&r, "ok: resubscribed to %s", last_info) < 0) r = NULL;
  }
  return r;
}
