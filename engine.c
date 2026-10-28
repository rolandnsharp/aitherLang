/* The engine (engine.h): the pump, the voice table, the guards, the
 * background -O2 builds, the rollback doctor and the control socket. */

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "engine.h"

int  aither_realtime(const char* who);   /* realtime.c */
void aither_repin(void);

/* What is still Nim until stage 3 (engine.nim): text, try's render, MIDI.
 * Each returns malloc'd text. */
char* aither_fmt_meter(const char* name, const StatsSnap* s);
char* aither_fmt_list(const VoiceSnap* v, int n);
char* aither_fmt_levels(const char* target, int found, const VoiceStatsSnap* s, int n);
char* aither_fmt_parts(const char* voice, int found, const PartSnap* p, int n);
char* aither_fmt_check(const CheckSnap* q);
char* aither_fmt_spectrum(const double* mono, long n, double sr);
/* `try`'s render of the scratch voice, and the whole reply. */
char* aither_try_report(Voice* scratch, double seconds, double skip, double t0, int sr,
                        const char* head);
char* aither_midi_command(int argc, const char** argv);
char* aither_midi_resubscribe(void);           /* NULL: nothing to report */
void  aither_midi_up(void);

enum { MAX_VOICES = 16 };
#define DEFAULT_FADE_MS 20.0
/* A voice that blows up this soon after a hot-swap is rolled back to
 * the version that was playing before it, instead of going silent. */
#define ROLLBACK_WINDOW 20.0
/* Inside that window the bar is lower than the runaway guard's +18
 * dBFS: new code climbing past +9.5 dBFS goes back before it's loud. */
#define ROLLBACK_LIMIT 3.0
/* A single voice sample beyond ±8 (+18 dBFS) is never music: it's a
 * patch that has blown up. The engine mutes the voice. */
#define RUNAWAY_LIMIT 8.0
/* Runaway outside that window: mute, scrub the state, and fade back in
 * this many seconds later, up to RUNAWAY_RETRIES times per send. */
#define RUNAWAY_RETRY_GAP 2.0
enum { RUNAWAY_RETRIES = 3 };
/* The meters' time constants, per sample: the decaying peak and the
 * smoothed mean square (the generated tick_block meters parts the same). */
#define PEAK_DECAY 0.99993
#define RMS_ALPHA  1.04e-4

/* Per-voice rolling buffer of the most recent ~0.5 s of post-fade stereo,
 * for `spectrum`. float keeps it small (~750 KB a voice). */
enum { MAX_RECENT = 96000 };             /* the array: 0.5 s at 192 kHz */

static const char* stdlib_text;
/* AITHER_SOCK runs a second engine (or talks to one) beside the default. */
static const char* socket_path;
/* Frames per write to the pipe. 512 (10.7 ms at 48 kHz) suits a machine
 * you play on; a streaming engine, where latency is free, can take 2048
 * or more so a slow period doesn't starve the reader. */
static unsigned buffer_frames;
/* Seconds a voice's block may take before a recursion in it gives up
 * (notes/tail-recursion-2026-10-16.md). 0 turns the guard off. */
static double watchdog;
/* The rate. Programs are written in seconds and hertz, never samples, so
 * the same text runs at 48 kHz for sound or 192 kHz for radio; only the
 * reader on the end of the pipe has to agree. */
static unsigned sample_rate;
static int env_bin_samples;              /* 50 ms per sparkline bin */
static int recent_samples;               /* the window of the ring in use */

/* ------------------------------------------------------------ small things */

static char* sdup(const char* s) { return strdup(s ? s : ""); }

typedef struct { char* s; size_t n, cap; } Buf;

static void bputn(Buf* b, const char* s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->s = realloc(b->s, b->cap);
  }
  memcpy(b->s + b->n, s, n);
  b->n += n;
  b->s[b->n] = 0;
}
static void bprintf(Buf* b, const char* fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) { bputn(b, tmp, n); return; }
  char* big = malloc(n + 1);
  va_start(ap, fmt);
  vsnprintf(big, n + 1, fmt, ap);
  va_end(ap);
  bputn(b, big, n);
  free(big);
}
static char* bdone(Buf* b) { return b->s ? b->s : strdup(""); }

static char* fmt(const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  char* s = NULL;
  if (vasprintf(&s, f, ap) < 0) s = NULL;
  va_end(ap);
  return s;
}

/* A number as text, as Nim's $ writes it: the shortest that reads back,
 * and a whole number keeps its ".0" (the log lines read "within 0.5s",
 * "retrying in 2.0s"). */
static void float_str(double x, char* out, int len) {
  for (int p = 1; p <= 17; p++) {
    snprintf(out, len, "%.*g", p, x);
    if (strtod(out, NULL) == x) break;
  }
  if (!strpbrk(out, ".eEn")) strncat(out, ".0", len - strlen(out) - 1);
}

/* A whole token as a number (Nim's parseFloat): no hex, nothing left over. */
static int parse_float(const char* s, double* out) {
  if (!s || !*s || strpbrk(s, "xX")) return 0;
  char* end;
  double x = strtod(s, &end);
  if (*end || end == s) return 0;
  *out = x;
  return 1;
}

static double parse_float_arg(const char* s, double fallback) {
  double x;
  return parse_float(s, &x) ? x : fallback;
}

static double env_float(const char* name, double def) {
  const char* v = getenv(name);
  double x;
  return v && *v && parse_float(v, &x) ? x : def;
}

static double fmax_nim(double x, double y) { return y <= x ? x : y; }
static double fclamp(double x, double a, double b) { return x < a ? a : x > b ? b : x; }

static int is_file(const char* p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static char* read_file(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) return NULL;
  Buf b = {0};
  char chunk[8192];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bputn(&b, chunk, n);
  fclose(f);
  return bdone(&b);
}

static const char* file_tail(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}

/* A patch's voice name: its file name without the extension. */
static char* base_name(const char* p) {
  const char* t = file_tail(p);
  const char* dot = strrchr(t, '.');
  return dot && dot > t ? strndup(t, dot - t) : sdup(t);
}

static int have_exe(const char* name) {
  const char* path = getenv("PATH");
  if (!path) return 0;
  char* copy = strdup(path);
  int found = 0;
  for (char* save = NULL, *d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", d, name);
    found = access(full, X_OK) == 0 && is_file(full);
  }
  free(copy);
  return found;
}

static int optimizer_wanted(void) {
  const char* o = getenv("AITHER_OPT");
  return !(o && strcmp(o, "0") == 0) && have_exe("cc");
}

/* ------------------------------------------------------------------- slots */

typedef struct {
  float l[MAX_RECENT], r[MAX_RECENT];
  int   idx;
} RecentRing;

typedef struct {
  double peak_l, peak_r;
  double rms_sq_l, rms_sq_r;
  long   clips;
  float  env_ring[ENGINE_ENV_BINS];
  int    env_bin_idx;
  float  env_bin_max;
  int    env_count;
} Stats;

typedef struct {
  char*      name;
  Voice*     voice;
  int        active;
  int        muted;
  double     fade_gain, fade_delta;
  Stats      stats;
  RecentRing recent;        /* the rolling 0.5 s for `spectrum` */
  int        nan_logged;    /* one log line per voice per send, then quiet */
  int        runaway;       /* muted by the runaway guard; cleared on resend */
  int        held;          /* muted or parked by you: its fade-out holds at
                               silence instead of freeing the slot */
  int        park_at_zero;  /* parked: once the fade reaches 0, stop running */
  int        parked;        /* parked: skipped, no CPU, state frozen */
  /* Guard bookkeeping, all reset by a send. The audio thread only sets
   * flags and counters; the doctor thread does the rollback. */
  double     swap_at;       /* engine time of the last hot-swap (0: none) */
  int        want_rollback; /* blew up inside ROLLBACK_WINDOW: swap back */
  int        stuck;         /* the last guard to fire was for a recursion
                               that didn't return (for the log and `check`) */
  int        rolled_back;   /* the last send was rolled back */
  long       bad_run;       /* consecutive NaN samples */
  long       nan_events;
  long       runaway_events;
  double     retry_at;      /* a runaway mute lifts at this engine time */
} Slot;

static void stats_update(Stats* s, double gl, double gr) {
  double al = fabs(gl), ar = fabs(gr);
  s->peak_l = fmax_nim(s->peak_l * PEAK_DECAY, al);
  s->peak_r = fmax_nim(s->peak_r * PEAK_DECAY, ar);
  s->rms_sq_l = s->rms_sq_l * (1.0 - RMS_ALPHA) + al * al * RMS_ALPHA;
  s->rms_sq_r = s->rms_sq_r * (1.0 - RMS_ALPHA) + ar * ar * RMS_ALPHA;
  if (al > 1.0 || ar > 1.0) s->clips++;
  float m = (float)fmax_nim(al, ar);
  if (m > s->env_bin_max) s->env_bin_max = m;
  if (++s->env_count >= env_bin_samples) {
    s->env_ring[s->env_bin_idx] = s->env_bin_max;
    s->env_bin_idx = (s->env_bin_idx + 1) % ENGINE_ENV_BINS;
    s->env_bin_max = 0;
    s->env_count = 0;
  }
}

static Slot       slots[MAX_VOICES];
static int        slot_count;
static Stats      master;
static RecentRing master_recent;
static volatile int running;
static int        killed;                /* `kill` ended it, not the reader */
static double     time_sec, time_frac;
static int        input_used;            /* a loaded patch calls input(): offline runs read stdin */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

/* The lock is the audio thread's most of the time: it takes it for every
 * block, lets go, and takes it straight back. Locks aren't fair, so a
 * plain lock from another thread could wait seconds while the audio ran
 * flat out, which is what an overloaded set does, just when a `send` or
 * `park` is wanted. So every other thread takes it through
 * lock_for_command, which says it's waiting, and the audio thread lets it
 * in before its next block. */
static atomic_int commands_waiting;

static void lock_for_command(void) {
  atomic_fetch_add(&commands_waiting, 1);
  pthread_mutex_lock(&mtx);
  atomic_fetch_sub(&commands_waiting, 1);
}
static void unlock(void) { pthread_mutex_unlock(&mtx); }

static double fade_delta_for(double seconds) {
  double s = seconds <= 0.0 ? DEFAULT_FADE_MS / 1000.0 : seconds;
  return 1.0 / (s * (double)sample_rate);
}

static int can_rollback(Voice* v) { return v->prev.state != NULL; }

/* ---------------------------------------------------------------- the block */

/* One block of the mix. Every per-sample step runs in the generated C
 * (tick_block, see codegen's BlockTemplate): the clock, input(), the
 * parts, the voice fade and part-gain ramps, the part meters. What
 * happens here happens once per voice per block: the guards, the voice's
 * stats and ring, the mix. The engine never touches a sample except to
 * add a voice's block into the mix. */
static double *blk_l, *blk_r;            /* one voice's block */
static double *blk_in;                   /* the input block as doubles */

static void mix_block(double* out_l, double* out_r, const float* input, int frames) {
  double dt = 1.0 / (double)sample_rate;
  for (int i = 0; i < frames; i++) out_l[i] = out_r[i] = 0.0;
  /* Wait for the lock, never skip the block. The stream is gapless: every
   * sample written is one tick of t, which is what tv and scope count
   * pixels and points by. A skipped block wrote silence without advancing
   * the clock, so any command that held the lock at the wrong moment
   * sheared a tv picture for the rest of the session. Holds are short (a
   * send's commit is the longest, a few ms) and the pipe ahead of the
   * player holds far more than that. A command waiting goes first. */
  while (atomic_load_explicit(&commands_waiting, memory_order_relaxed) > 0) sched_yield();
  pthread_mutex_lock(&mtx);
  double* in_ptr = NULL;
  if (input) {
    for (int i = 0; i < frames * 2; i++) blk_in[i] = (double)input[i];
    in_ptr = blk_in;
  }
  /* The engine clock at the block's start. Every voice runs the same
   * samples from it, and the engine advances its own copy the same way
   * afterwards, so t has the same bits everywhere. */
  double t0 = time_sec + time_frac;
  for (int v = 0; v < slot_count; v++) {
    Slot* s = &slots[v];
    /* A runaway mute lifts by itself after a gap, with a fade-in, so one
     * bad moment in a long patch doesn't end the voice. */
    if (s->runaway && s->retry_at > 0.0 && t0 >= s->retry_at) {
      s->runaway = 0;
      s->muted = 0;
      s->retry_at = 0.0;
      s->fade_gain = 0.0;
      s->fade_delta = fade_delta_for(1.0);
    }
    if (!s->active || s->muted || s->parked || s->want_rollback) continue;
    Voice* voice = s->voice;
    VoiceHeader* hdr = voice->cur.state;
    hdr->time_sec = time_sec;
    hdr->time_frac = time_frac;
    hdr->in_buf = in_ptr;
    hdr->fade_gain = s->fade_gain;
    hdr->fade_delta = s->fade_delta;
    hdr->deadline = watchdog > 0.0 ? voice_mono_now() + watchdog : 0.0;
    voice_tick_block(voice, frames, blk_l, blk_r);
    s->fade_gain = hdr->fade_gain;
    s->fade_delta = hdr->fade_delta;
    int in_window = s->swap_at > 0.0 && t0 - s->swap_at < ROLLBACK_WINDOW && can_rollback(voice);
    /* A recursion that hadn't returned by the deadline gave up, and its
     * samples came out silent. It would get stuck again every block, so
     * the voice goes: back to the previous version if this follows a
     * send, else muted until it is sent again. */
    if (hdr->stuck) {
      char w[32];
      float_str(watchdog, w, sizeof w);
      if (in_window) {
        fprintf(stderr, "[aither] voice %s: %s didn't return within %ss\n", s->name,
                hdr->stuck_at ? hdr->stuck_at : "", w);
        s->stuck = 1;
        s->want_rollback = 1;
        continue;
      }
      voice_reset_pool(voice);
      voice_scrub_state(voice, 1);
      s->stuck = 1;
      s->muted = 1;
      s->runaway = 1;
      s->retry_at = 0.0;
      fprintf(stderr, "[aither] voice %s: %s didn't return within %ss — muted until resent\n",
              s->name, hdr->stuck_at ? hdr->stuck_at : "", w);
      continue;
    }
    int bad = hdr->bad > 0;
    double peak = hdr->peak_raw;
    /* Blew up right after a hot-swap: the new code is the suspect. Hold
     * the voice silent for the few ms until the doctor thread puts the
     * previous version back. */
    if ((bad || peak > ROLLBACK_LIMIT) && in_window) {
      s->stuck = 0;
      s->want_rollback = 1;
      continue;
    }
    /* NaN/Inf: the bad samples were zeroed in C; reset the pool, clear
     * poisoned `$state`, and log once per voice. If it keeps happening
     * for 100 ms, every def `$state` re-inits. */
    if (bad) {
      int first = s->bad_run == 0;
      s->bad_run += hdr->bad;
      voice_reset_pool(voice);
      voice_scrub_state(voice, s->bad_run > (long)(sample_rate / 10));
      if (first) s->nan_events++;
      if (!s->nan_logged) {
        fprintf(stderr, "[aither] voice %s produced NaN/Inf — pool reset\n", s->name);
        s->nan_logged = 1;
      }
    } else {
      s->bad_run = 0;
    }
    /* Runaway: finite but far too loud (an unstable filter chain climbing
     * toward 1e6). Mute, wipe the state, and retry after a gap; after
     * RUNAWAY_RETRIES it stays muted until resent. */
    if (fabs(peak) > RUNAWAY_LIMIT) {
      voice_reset_pool(voice);
      voice_scrub_state(voice, 1);
      s->stuck = 0;
      s->muted = 1;
      s->runaway = 1;
      s->runaway_events++;
      int retry = s->runaway_events <= RUNAWAY_RETRIES;
      s->retry_at = retry ? t0 + RUNAWAY_RETRY_GAP : 0.0;
      if (retry) {
        char g[32];
        float_str(RUNAWAY_RETRY_GAP, g, sizeof g);
        fprintf(stderr, "[aither] voice %s exceeded +18 dBFS — muted, retrying in %ss\n", s->name, g);
      } else {
        fprintf(stderr, "[aither] voice %s exceeded +18 dBFS — muted until resent\n", s->name);
      }
      continue;
    }
    if (s->fade_gain <= 0.0 && s->fade_delta < 0.0) {
      if (s->held) {                     /* muted or parked: hold at silence */
        s->fade_delta = 0.0;
        if (s->park_at_zero) {
          s->parked = 1;
          s->park_at_zero = 0;
        }
      } else {
        s->active = 0;                   /* fade-out done: the slot is free */
      }
    }
    /* The voice's stats and rolling buffer, and its share of the mix. */
    for (int i = 0; i < frames; i++) {
      double gl = blk_l[i], gr = blk_r[i];
      stats_update(&s->stats, gl, gr);
      s->recent.l[s->recent.idx] = (float)gl;
      s->recent.r[s->recent.idx] = (float)gr;
      s->recent.idx = (s->recent.idx + 1) % recent_samples;
      out_l[i] += gl;
      out_r[i] += gr;
    }
  }
  /* Advance the engine clock the way every voice just did. */
  for (int i = 0; i < frames; i++) {
    time_frac += dt;
    if (time_frac >= 1.0) {
      time_sec += 1.0;
      time_frac -= 1.0;
    }
  }
  for (int i = 0; i < frames; i++) {
    stats_update(&master, out_l[i], out_r[i]);
    master_recent.l[master_recent.idx] = (float)out_l[i];
    master_recent.r[master_recent.idx] = (float)out_r[i];
    master_recent.idx = (master_recent.idx + 1) % recent_samples;
  }
  pthread_mutex_unlock(&mtx);
}

/* -------------------------------------------------------- voice management */

static int find_slot(const char* name) {
  for (int i = 0; i < slot_count; i++)
    if (strcmp(slots[i].name, name) == 0) return i;
  return -1;
}

/* Compact stopped voices out of the table so they don't count against
 * MAX_VOICES. Under mtx. Inactive slots are the ones whose fade-out
 * completed (the audio thread cleared `active`). Without sweeping, a
 * session that stops 16 voices without re-sending exhausts the table
 * even though no voice is audible. The order of the rest is kept. */
static void sweep_inactive_slots(void) {
  int dst = 0;
  for (int src = 0; src < slot_count; src++) {
    if (!slots[src].active) {
      free(slots[src].name);
      continue;
    }
    if (dst != src) slots[dst] = slots[src];
    dst++;
  }
  for (int i = dst; i < slot_count; i++) memset(&slots[i], 0, sizeof slots[i]);
  slot_count = dst;
}

/* --------------------------------------------------- background -O2 builds */
/* TCC gets a patch playing within milliseconds, but its code is slow (an
 * additive patch can run 5x slower than cc -O2). So each load also
 * queues the same C for this worker, which builds it with cc -O2 and
 * swaps the voice's tick for the fast one: same state, no glitch. A
 * reload while it builds makes the job stale and the result is dropped. */

typedef struct OptJob {
  char*  name;
  Voice* voice;              /* identity only: compared under mtx */
  long   gen;
  char*  csrc;
  long   state_size;
  struct OptJob* next;
} OptJob;

static OptJob* opt_head, *opt_tail;
static pthread_mutex_t opt_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  opt_cond = PTHREAD_COND_INITIALIZER;
static int opt_enabled;

static int current_slot(const OptJob* job) {
  /* The slot still running the job's code, or -1. Under mtx. */
  int i = find_slot(job->name);
  return i >= 0 && slots[i].voice == job->voice && slots[i].voice->gen == job->gen ? i : -1;
}

static void* opt_worker(void* arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&opt_mtx);
    while (!opt_head) pthread_cond_wait(&opt_cond, &opt_mtx);
    OptJob* job = opt_head;
    opt_head = job->next;
    if (!opt_head) opt_tail = NULL;
    pthread_mutex_unlock(&opt_mtx);
    lock_for_command();
    int live = current_slot(job) >= 0;
    unlock();
    if (live) {                          /* else reloaded while queued */
      void* lib;
      TickFn fn;
      char err[2048];
      if (voice_compile_optimized(job->csrc, job->state_size, 1, &lib, &fn, err, sizeof err) == 0) {
        lock_for_command();
        int i = current_slot(job);
        int ok = i >= 0 && voice_upgrade(slots[i].voice, job->gen, lib, fn);
        unlock();
        if (ok) fprintf(stderr, "[aither] %s optimized (cc -O2)\n", job->name);
      } else {
        err[strcspn(err, "\r\n")] = 0;
        fprintf(stderr, "[aither] cc -O2 build of %s failed, staying on TCC: %s\n", job->name, err);
      }
    }
    free(job->name);
    free(job->csrc);
    free(job);
  }
  return NULL;
}

static void queue_optimize(const char* name, Voice* v) {
  /* From the server thread after a commit, without mtx: only this thread
   * commits, so v's generation and C can't change underneath it. */
  if (!opt_enabled) return;
  OptJob* job = calloc(1, sizeof *job);
  *job = (OptJob){sdup(name), v, v->gen, sdup(v->cur.csrc), v->cur.state_size, NULL};
  pthread_mutex_lock(&opt_mtx);
  if (opt_tail) opt_tail->next = job; else opt_head = job;
  opt_tail = job;
  pthread_cond_signal(&opt_cond);
  pthread_mutex_unlock(&opt_mtx);
}

static void start_optimizer(void) {
  if (!optimizer_wanted()) {
    fprintf(stderr, "[aither] cc -O2 rebuilds off (no cc, or AITHER_OPT=0)\n");
    return;
  }
  pthread_t th;
  pthread_create(&th, NULL, opt_worker, NULL);
  pthread_detach(th);
  opt_enabled = 1;
}

static void optimize_now(Voice* v) {
  /* An offline run's -O2 build, up front; on failure it stays on TCC. */
  if (!optimizer_wanted()) return;
  void* lib;
  TickFn fn;
  char err[2048];
  if (voice_compile_optimized(v->cur.csrc, v->cur.state_size, 0, &lib, &fn, err, sizeof err) == 0)
    voice_upgrade(v, v->gen, lib, fn);
}

/* ------------------------------------------------------- the rollback doctor */
/* The audio thread can't roll a voice back itself (that allocates and
 * frees), so it flags the slot and stays silent on it; this thread
 * notices within ~10 ms and swaps the previous version back in. */

static void* doctor(void* arg) {
  (void)arg;
  for (long ticks = 1;; ticks++) {
    usleep(10000);
    if (ticks % 100 == 0) aither_repin();   /* once a second: cores come and go */
    int wanted = 0;                          /* a peek without the lock */
    for (int i = 0; i < slot_count; i++)
      if (slots[i].want_rollback) wanted = 1;
    if (!wanted) continue;
    lock_for_command();
    for (int i = 0; i < slot_count; i++) {
      Slot* s = &slots[i];
      if (!s->want_rollback) continue;
      double after = time_sec + time_frac - s->swap_at;
      if (voice_rollback(s->voice)) {
        s->rolled_back = 1;
        s->bad_run = 0;
        fprintf(stderr, "[aither] %s %s %.1fs after the swap — rolled back to the previous version\n",
                s->name, s->stuck ? "got stuck" : "blew up", after);
      }
      s->want_rollback = 0;
      s->swap_at = 0.0;
    }
    unlock();
  }
  return NULL;
}

/* ------------------------------------------------------------ load and try */

/* Find a codegen error's first `(source:line)` and add that line's text,
 * so the offending code is in the reply instead of having to open the
 * file and count. A bare `(line N)` passes through unchanged. */
static char* annotate_err(const char* msg, const char* user_src, const char* user_path) {
  const char* p = msg;
  while (*p) {
    const char* lp = strchr(p, '(');
    if (!lp) break;
    const char* rp = strchr(lp + 1, ')');
    if (!rp) break;
    const char* colon = memchr(lp + 1, ':', rp - lp - 1);
    if (colon && colon > lp + 1) {
      char* src = strndup(lp + 1, colon - lp - 1);
      char* num = strndup(colon + 1, rp - colon - 1);
      char* end;
      long line = strtol(num, &end, 10);
      if (*end || end == num) line = 0;
      free(num);
      if (line > 0) {
        char* owned = NULL;
        const char* body = strcmp(src, "stdlib") == 0 ? stdlib_text
                         : strcmp(src, user_path) == 0 ? user_src
                         : is_file(src) ? (owned = read_file(src)) : NULL;
        char* out = NULL;
        if (body && *body) {
          const char* l = body;
          for (long k = 1; k < line && l; k++) {
            l = strchr(l, '\n');
            if (l) l++;
          }
          if (l) {
            const char* e = strchr(l, '\n');
            if (!e) e = l + strlen(l);
            while (l < e && strchr(" \t\v\r\f", *l)) l++;
            while (e > l && strchr(" \t\v\r\f", e[-1])) e--;
            out = fmt("%s\n  %s:%ld  %.*s", msg, src, line, (int)(e - l), l);
          }
        }
        free(owned);
        free(src);
        return out ? out : sdup(msg);
      }
      free(src);
    }
    p = rp + 1;
  }
  return sdup(msg);
}

/* Parse (stdlib and patch apart, so line numbers are the patch's), splice
 * imports (re-read on every send, so an edited library lands with the
 * next send) and compile. Slow: outside the audio mutex. NULL on an
 * error, with its text (`parse error: ...`) in *err. */
static Prepared* prepare(const char* path, const char* user_src, char** err) {
  const char* h = getenv("AITHER_HOIST");
  int hoist = !(h && strcmp(h, "0") == 0);
  int kind;
  char e[4096] = "";
  Prepared* p = voice_prepare(stdlib_text, user_src, path, path, (double)sample_rate, 1, hoist,
                              &kind, e, sizeof e);
  if (p) return p;
  if (kind == VOICE_IMPORT) {
    *err = fmt("import error: %s", e);
  } else if (kind == VOICE_COMPILE || kind == VOICE_TCC) {
    char* a = annotate_err(e, user_src, path);
    *err = fmt("compile error: %s", a);
    free(a);
  } else {
    *err = fmt("parse error: %s", e);
  }
  return NULL;
}

char* engine_load(const char* path, double fade_in) {
  if (!is_file(path)) return fmt("file not found: %s", path);
  char* user_src = read_file(path);
  char* name = base_name(path);
  fprintf(stderr, "loading %s ... ", file_tail(path));
  char* err = NULL;
  Prepared* prepared = prepare(path, user_src, &err);
  free(user_src);
  if (!prepared) {
    fprintf(stderr, "%s FAIL\n", err[0] == 'i' ? "import" : err[0] == 'c' ? "compile" : "parse");
    free(name);
    return err;
  }
  if (prepared->calls_input) input_used = 1;

  lock_for_command();
  /* Drop slots whose fade-out completed before deciding hot-swap vs new
   * vs voice limit: a session that stopped 16 voices without re-sending
   * still saw "voice limit reached" though every slot was silent. */
  sweep_inactive_slots();
  int idx = find_slot(name);
  double now = time_sec + time_frac;
  Voice* committed;
  if (idx >= 0) {
    Slot* s = &slots[idx];
    committed = s->voice;
    voice_commit(committed, prepared);
    s->nan_logged = 0;                   /* fresh diagnostics for the new code */
    s->swap_at = now;
    s->want_rollback = 0;
    s->stuck = 0;
    s->rolled_back = 0;
    s->bad_run = 0;
    s->nan_events = 0;
    s->runaway_events = 0;
    s->retry_at = 0.0;
    if (s->runaway) {                    /* new code gets a fresh chance */
      s->runaway = 0;
      s->muted = 0;
    }
    /* Retrigger a stopped voice or one fading out; an edit to a muted or
     * parked voice stays so. */
    int retrigger = !s->active ||
                    (!s->held && (s->fade_gain <= 0.0 || s->fade_delta < 0.0));
    if (retrigger) {
      committed->sweep_origin = now;
      s->active = 1;
      s->fade_gain = fade_in > 0.0 ? 0.0 : 1.0;
      s->fade_delta = fade_in > 0.0 ? fade_delta_for(fade_in) : 0.0;
    } else if (fade_in > 0.0) {
      s->fade_delta = fade_delta_for(fade_in);
    }
    unlock();
    fprintf(stderr, "ok (%s %s)\n", retrigger ? "retrigger" : "hot-swap", name);
  } else {
    if (slot_count >= MAX_VOICES) {
      unlock();
      voice_prepared_free(prepared);
      free(name);
      return fmt("voice limit reached (%d)", MAX_VOICES);
    }
    committed = voice_new();
    voice_commit(committed, prepared);
    committed->sweep_origin = now;
    Slot* s = &slots[slot_count];
    memset(s, 0, sizeof *s);
    s->name = sdup(name);
    s->voice = committed;
    s->active = 1;
    s->fade_gain = fade_in > 0.0 ? 0.0 : 1.0;
    s->fade_delta = fade_in > 0.0 ? fade_delta_for(fade_in) : 0.0;
    slot_count++;
    unlock();
    fprintf(stderr, "ok (new %s)\n", name);
  }
  queue_optimize(name, committed);
  free(name);
  /* Resubscribe MIDI if the input thread had silently died. Cheap when
   * nothing dropped; on a recovery, say so: the keyboard is back. */
  char* midi = aither_midi_resubscribe();
  if (midi) {
    fprintf(stderr, "[aither] MIDI %s\n", midi);
    free(midi);
  }
  return NULL;
}

static char* try_patch(const char* path, double seconds, double skip) {
  /* `try`: compile the patch, migrate the live voice's current state into
   * it exactly as a send would, and render it offline from the engine's
   * current time. The live voice plays on untouched. On the server
   * thread: other commands wait for it. */
  if (!is_file(path)) return fmt("ERR file not found: %s", path);
  char* user_src = read_file(path);
  char* err = NULL;
  Prepared* prepared = prepare(path, user_src, &err);
  free(user_src);
  if (!prepared) {
    char* r = fmt("ERR %s", err);
    free(err);
    return r;
  }
  char* name = base_name(path);
  Voice* scratch = voice_new();
  lock_for_command();
  int idx = find_slot(name);
  double now = time_sec + time_frac;
  if (idx >= 0 && slots[idx].voice->cur.state) {
    long size = slots[idx].voice->cur.state_size;
    unlock();                            /* allocate outside the audio lock */
    void* buf = calloc(size, 1);
    lock_for_command();
    Voice* live = slots[idx].voice;
    if (live->cur.state && live->cur.state_size == size) voice_snapshot_into(live, scratch, buf);
    else free(buf);
  }
  unlock();
  char* origin = scratch->cur.state ? fmt("live state of %s", name)
                                    : fmt("fresh state (no live voice %s)", name);
  if (!scratch->cur.state) scratch->sweep_origin = now;
  voice_commit(scratch, prepared);
  char* head = fmt("try: %s from %s, t=%.1fs, %.1fs", name, origin, now + skip, seconds);
  char* r = aither_try_report(scratch, seconds, skip, now, (int)sample_rate, head);
  voice_dispose(scratch);
  free(head);
  free(origin);
  free(name);
  return r;
}

/* ------------------------------------------------------------------ profile */

typedef struct { double tm; const char* name; int order; } ProfRow;

static int prof_cmp(const void* a, const void* b) {
  /* Heaviest first; equal times keep their order. */
  const ProfRow* x = a; const ProfRow* y = b;
  if (x->tm != y->tm) return x->tm < y->tm ? 1 : -1;
  return x->order - y->order;
}

static void pct(char* out, double x, double whole) {
  snprintf(out, 32, "%.1f", whole > 0 ? 100 * x / whole : 0.0);
}

static char* profile_voice(const char* name, double seconds) {
  /* `profile`: time the voice's code for `seconds` and report each play
   * and top-level statement's share of one core, heaviest first, the
   * cost of the timing itself taken off. Off the rest of the time: the
   * code checks a flag per statement and reads no clock. */
  double cost = voice_clock_read_cost();
  lock_for_command();
  int idx = find_slot(name);
  if (idx < 0) {
    unlock();
    return fmt("ERR not found: %s", name);
  }
  Voice* v = slots[idx].voice;
  long gen = v->gen;
  for (int i = 0; i < VOICE_MAX_PROF_SLOTS; i++) v->prof[i] = 0.0;
  VoiceHeader* hdr = v->cur.state;
  hdr->prof_ticks = 0;
  hdr->profiling = 1;
  double t0 = voice_mono_now();          /* the window as it really ran, not as asked */
  unlock();
  long ms = (long)(seconds * 1000);
  nanosleep(&(struct timespec){ms / 1000, ms % 1000 * 1000000L}, NULL);
  lock_for_command();
  idx = find_slot(name);
  if (idx < 0 || slots[idx].voice != v || v->gen != gen) {
    if (idx >= 0) ((VoiceHeader*)slots[idx].voice->cur.state)->profiling = 0;
    unlock();
    return fmt("ERR %s was sent again or stopped while profiling", name);
  }
  hdr->profiling = 0;
  double window = voice_mono_now() - t0;
  double ticks = (double)hdr->prof_ticks;
  const char** names = NULL;
  int n = voice_prof_names(v, &names);
  ProfRow* rows = calloc(n ? n : 1, sizeof *rows);
  double total = 0.0;
  for (int i = 0; i < n; i++) {
    double tm = fmax_nim(0.0, v->prof[i] - cost * ticks);
    rows[i] = (ProfRow){tm, names[i], i};
    total += tm;
  }
  unlock();
  qsort(rows, n, sizeof *rows, prof_cmp);
  Buf o = {0};
  char a[32], b[32];
  pct(a, total, window);
  bprintf(&o, "OK %s: %s%% of one core over %.1fs\n", name, a, window);
  double shown = 0.0;
  for (int k = 0; k < n; k++) {
    if (k >= 25 || (k >= 8 && shown >= 0.95 * total)) {
      char more[64];
      snprintf(more, sizeof more, "(%d more)", n - k);
      pct(a, total - shown, window);
      pct(b, total - shown, total);
      bprintf(&o, "  %-34s%6s%%  %5s%% of the voice\n", more, a, b);
      break;
    }
    pct(a, rows[k].tm, window);
    pct(b, rows[k].tm, total);
    bprintf(&o, "  %-34s%6s%%  %5s%% of the voice\n", rows[k].name, a, b);
    shown += rows[k].tm;
  }
  free(rows);
  while (o.n > 0 && strchr(" \t\v\r\n\f", o.s[o.n - 1])) o.s[--o.n] = 0;
  return bdone(&o);
}

/* ------------------------------------------------------ sweep, stop, mute */

static char* sweep_voice(const char* name, double seconds) {
  /* Set a voice's sweep: from the next sample it reads `seconds` and runs
   * on. 0 plays the piece from the top. */
  lock_for_command();
  int idx = find_slot(name);
  if (idx >= 0) slots[idx].voice->sweep_origin = time_sec + time_frac - seconds;
  unlock();
  return idx < 0 ? fmt("not found: %s", name) : NULL;
}

int engine_mark_stopped(const char* name) {
  /* For the tests: mark a voice inactive, as the audio thread does when
   * its fade-out completes, to exercise the sweep without an audio thread. */
  lock_for_command();
  int idx = find_slot(name);
  if (idx >= 0) slots[idx].active = 0;
  unlock();
  return idx >= 0;
}

int engine_active_count(void) {
  int n = 0;
  for (int i = 0; i < slot_count; i++) n += slots[i].active != 0;
  return n;
}

static void stop_slot(Slot* s, double fade) {
  /* Fade a voice out and free its slot; a parked one goes at once. */
  s->held = 0;
  s->park_at_zero = 0;
  if (s->parked) {
    s->parked = 0;
    s->active = 0;
  }
  s->fade_delta = -fade_delta_for(fade);
}

char* engine_stop(const char* name, double fade) {
  lock_for_command();
  int idx = find_slot(name);
  if (idx >= 0) stop_slot(&slots[idx], fade);
  unlock();
  return idx < 0 ? fmt("not found: %s", name) : NULL;
}

static void clear_all(double fade) {
  lock_for_command();
  for (int i = 0; i < slot_count; i++) stop_slot(&slots[i], fade);
  unlock();
}

static void silence(Slot* s, int park, double fade) {
  /* Mute (it runs on, silent) or park (it stops running once silent). */
  if (s->parked) return;
  s->held = 1;
  s->park_at_zero = park;
  if (s->fade_gain <= 0.0) {
    s->fade_delta = 0.0;
    s->parked = park;
    s->park_at_zero = 0;
  } else {
    s->fade_delta = -fade_delta_for(fade);
  }
}

static void wake(Slot* s, double fade) {
  /* Unmute or unpark: run again from where it was and fade in. You take
   * responsibility for a voice the guard had muted. */
  s->held = 0;
  s->park_at_zero = 0;
  s->parked = 0;
  if (s->muted) {
    s->muted = 0;
    s->runaway = 0;
    s->retry_at = 0.0;
  }
  s->fade_delta = s->fade_gain < 1.0 ? fade_delta_for(fade) : 0.0;
}

char* engine_silence(const char* name, int on, int park, double fade) {
  /* `mute`/`park` a voice (on) or `unmute`/`unpark` it. */
  lock_for_command();
  int idx = find_slot(name);
  if (idx >= 0) {
    if (on) silence(&slots[idx], park, fade);
    else wake(&slots[idx], fade);
  }
  unlock();
  return idx < 0 ? fmt("not found: %s", name) : NULL;
}

char* engine_solo(const char* name, double fade) {
  /* The voice plays, every other one is muted: they run on, silent, so
   * `unmute` brings them back in step. */
  lock_for_command();
  int idx = find_slot(name);
  if (idx >= 0) {
    for (int i = 0; i < slot_count; i++) {
      if (!slots[i].active) continue;
      if (i == idx) wake(&slots[i], fade);
      else if (slots[i].held || slots[i].fade_delta >= 0.0)   /* not already stopping */
        silence(&slots[i], 0, fade);
    }
  }
  unlock();
  return idx < 0 ? fmt("not found: %s", name) : NULL;
}

/* -------------------------------------------------------------------- parts */

static int find_part(Voice* v, const char* name) {
  for (int i = 0; i < v->cur.nparts; i++)
    if (strcmp(v->cur.part_names[i], name) == 0) return i;
  return -1;
}

static void set_part_gain_target(Voice* v, int part, double target, double fade) {
  double c = fclamp(target, 0.0, 1.0);
  v->part_fade_targets[part] = c;
  if (fade <= 0.0) {
    v->part_gains[part] = c;
    v->part_fade_deltas[part] = 0.0;
  } else {
    double steps = fmax_nim(1.0, fade * (double)sample_rate);
    v->part_fade_deltas[part] = (c - v->part_gains[part]) / steps;
  }
}

enum { PART_MUTE, PART_UNMUTE, PART_PARK, PART_UNPARK, PART_SOLO };

static char* part_gesture(const char* voice_name, const char* part_name, int what, double fade) {
  /* mute/unmute, park/unpark or solo one play of a voice. A park fades the
   * part out, then skips its body (no CPU, state frozen); a mute keeps
   * computing, so its LFOs and sequencers stay in step. */
  double f = fade > 0.0 ? fade : DEFAULT_FADE_MS / 1000.0;
  lock_for_command();
  int vi = find_slot(voice_name);
  if (vi < 0) {
    unlock();
    return fmt("voice not found: %s", voice_name);
  }
  Voice* v = slots[vi].voice;
  int p = find_part(v, part_name);
  if (p < 0) {
    unlock();
    return fmt("no play named %s in voice %s", part_name, voice_name);
  }
  switch (what) {
  case PART_MUTE:
    set_part_gain_target(v, p, 0.0, f);
    break;
  case PART_UNMUTE:                      /* wakes a parked part too */
  case PART_UNPARK:
    v->part_parked[p] = 0.0;
    v->part_park_pending[p] = 0;
    set_part_gain_target(v, p, 1.0, f);
    break;
  case PART_PARK:
    if (v->part_gains[p] <= 0.0) {
      v->part_parked[p] = 1.0;
      v->part_park_pending[p] = 0;
    } else {
      v->part_park_pending[p] = 1;
      set_part_gain_target(v, p, 0.0, f);
    }
    break;
  case PART_SOLO:
    for (int i = 0; i < v->cur.nparts; i++) set_part_gain_target(v, i, i == p ? 1.0 : 0.0, f);
    break;
  }
  unlock();
  return NULL;
}

/* ---------------------------------------------------------------- snapshots */
/* Taken under mtx, formatted after it is let go. Side effects that look
 * like part of the text (clips clear on read) happen here. */

static int voice_state_of(const Slot* s) {
  if (!s->active) return VS_STOPPED;
  if (s->runaway) return VS_RUNAWAY;
  if (s->parked || s->park_at_zero) return VS_PARKED;
  if (s->muted || s->held) return VS_MUTED;
  if (s->fade_delta < 0.0) return VS_FADING_OUT;
  if (s->fade_delta > 0.0) return VS_FADING_IN;
  return VS_PLAYING;
}

static int part_state_of(double g, double d, int parked) {
  if (parked) return PS_PARKED;
  if (d > 0.0) return PS_FADING_IN;
  if (d < 0.0) return PS_FADING_OUT;
  if (g <= 0.0) return PS_SILENT;
  return PS_PLAYING;
}

static StatsSnap snapshot_stats(const Stats* s) {
  StatsSnap r = {sqrt(s->rms_sq_l), sqrt(s->rms_sq_r), s->peak_l, s->peak_r, s->clips, {0},
                 s->env_bin_idx};
  memcpy(r.env_ring, s->env_ring, sizeof r.env_ring);
  return r;
}

static int snapshot_parts(Voice* v, PartSnap** out) {
  int n = v->cur.nparts;
  PartSnap* p = calloc(n ? n : 1, sizeof *p);
  for (int i = 0; i < n; i++) {
    const char* name = v->cur.part_names[i];
    int unheard = 0;
    for (int k = 0; k < v->cur.nunheard; k++)
      if (strcmp(v->cur.unheard[k], name) == 0) unheard = 1;
    p[i] = (PartSnap){sdup(name),
                      part_state_of(v->part_gains[i], v->part_fade_deltas[i], v->part_parked[i] != 0.0),
                      v->part_gains[i], sqrt(v->part_rms_sq[i]), v->part_peak[i], unheard};
  }
  *out = p;
  return n;
}

static void free_parts(PartSnap* p, int n) {
  for (int i = 0; i < n; i++) free(p[i].name);
  free(p);
}

int engine_voices(VoiceSnap** out) {
  /* Every voice and its parts: `list`. */
  lock_for_command();
  VoiceSnap* v = calloc(slot_count ? slot_count : 1, sizeof *v);
  for (int i = 0; i < slot_count; i++) {
    v[i].name = sdup(slots[i].name);
    v[i].state = voice_state_of(&slots[i]);
    v[i].gain = slots[i].fade_gain;
    v[i].nparts = snapshot_parts(slots[i].voice, &v[i].parts);
  }
  int n = slot_count;
  unlock();
  *out = v;
  return n;
}

void engine_free_voices(VoiceSnap* v, int n) {
  for (int i = 0; i < n; i++) {
    free(v[i].name);
    free_parts(v[i].parts, v[i].nparts);
  }
  free(v);
}

static VoiceStatsSnap slot_stats(Slot* s) {
  VoiceStatsSnap r = {sdup(s->name), 0, voice_state_of(s), s->fade_gain, snapshot_stats(&s->stats)};
  return r;
}

static char* levels(const char* target) {
  /* "" or "*": the master and every voice; "master": the master; else the
   * named voice. Clears the clips it reports. */
  VoiceStatsSnap* q = calloc(MAX_VOICES + 1, sizeof *q);
  int n = 0, found = 0;
  lock_for_command();
  int all = !*target || strcmp(target, "*") == 0;
  if (all || strcmp(target, "master") == 0) {
    found = 1;
    q[n++] = (VoiceStatsSnap){sdup("master"), 1, 0, 0.0, snapshot_stats(&master)};
    master.clips = 0;
    for (int i = 0; all && i < slot_count; i++) {
      q[n++] = slot_stats(&slots[i]);
      slots[i].stats.clips = 0;
    }
  } else {
    int idx = find_slot(target);
    if (idx >= 0) {
      found = 1;
      q[n++] = slot_stats(&slots[idx]);
      slots[idx].stats.clips = 0;
    }
  }
  unlock();
  char* r = aither_fmt_levels(target, found, q, n);
  for (int i = 0; i < n; i++) free(q[i].name);
  free(q);
  return r;
}

static char* parts_of(const char* name) {
  PartSnap* p = NULL;
  int n = 0;
  lock_for_command();
  int idx = find_slot(name);
  if (idx >= 0) n = snapshot_parts(slots[idx].voice, &p);
  unlock();
  char* r = aither_fmt_parts(name, idx >= 0, p, n);
  free_parts(p, n);
  return r;
}

static double* recent_mono(const RecentRing* r) {
  /* The rolling buffer as mono, oldest first. Under mtx. */
  double* m = malloc(recent_samples * sizeof *m);
  for (int k = 0; k < recent_samples; k++) {
    int i = (r->idx + k) % recent_samples;
    m[k] = ((double)r->l[i] + (double)r->r[i]) * 0.5;
  }
  return m;
}

static char* spectrum(const char* target) {
  /* "" or "master": the master's latest ~0.5 s; else the named voice's. */
  double* m = NULL;
  lock_for_command();
  if (!*target || strcmp(target, "master") == 0) {
    m = recent_mono(&master_recent);
  } else {
    int idx = find_slot(target);
    if (idx >= 0) m = recent_mono(&slots[idx].recent);
  }
  unlock();
  if (!m || recent_samples == 0) {
    free(m);
    return fmt("ERR voice not found: %s", target);
  }
  char* body = aither_fmt_spectrum(m, recent_samples, (double)sample_rate);
  free(m);
  char* r = fmt("OK\n%s", body);
  free(body);
  return r;
}

static char* meter_after_send(const char* name) {
  /* The meter on `send`'s OK line. Doesn't clear clips: it's the meter
   * the instant the swap lands, not the official read. */
  StatsSnap st;
  lock_for_command();
  int idx = find_slot(name);
  if (idx >= 0) st = snapshot_stats(&slots[idx].stats);
  unlock();
  return idx >= 0 ? aither_fmt_meter(name, &st) : NULL;
}

static char* check(const char* name) {
  CheckSnap q = {0};
  lock_for_command();
  int idx = find_slot(name);
  if (idx >= 0) {
    Slot* s = &slots[idx];
    q.found = 1;
    q.voice = slot_stats(s);
    q.nparts = snapshot_parts(s->voice, &q.parts);
    q.rolled_back = s->rolled_back;
    q.stuck = s->stuck;
    q.nan_events = s->nan_events;
    q.runaway_events = s->runaway_events;
    q.retrying = s->runaway && s->retry_at > 0.0;
  } else {
    q.voice.name = sdup("");
  }
  q.master = snapshot_stats(&master);
  unlock();
  char* r = aither_fmt_check(&q);
  free(q.voice.name);
  free_parts(q.parts, q.nparts);
  return r;
}

/* ------------------------------------------------------------- the commands */

static char* ok_or_err(char* err) {
  if (!err) return sdup("OK");
  char* r = fmt("ERR %s", err);
  free(err);
  return r;
}

static char* ok_body(char* body) {
  char* r = fmt("OK\n%s", body);
  free(body);
  return r;
}

static char* handle(int n, char** w) {
  if (n == 0) return sdup("ERR empty");
  char verb[32];
  snprintf(verb, sizeof verb, "%s", w[0]);
  for (char* c = verb; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
#define IS(s) (strcmp(verb, s) == 0)
  if (IS("send")) {
    if (n < 2) return sdup("ERR usage: send <file> [fade-seconds]");
    char* err = engine_load(w[1], n >= 3 ? parse_float_arg(w[2], 0.0) : 0.0);
    if (err) return ok_or_err(err);
    char* name = base_name(w[1]);
    char* meter = meter_after_send(name);
    free(name);
    if (!meter) return sdup("OK");
    char* r = fmt("OK %s", meter);
    free(meter);
    return r;
  }
  if (IS("stop")) {
    if (n < 2) return sdup("ERR usage: stop <name> [fade-seconds]");
    return ok_or_err(engine_stop(w[1], n >= 3 ? parse_float_arg(w[2], 0.0) : 0.0));
  }
  if (IS("mute") || IS("unmute") || IS("solo") || IS("park") || IS("unpark")) {
    /* `verb voice [fade]` or `verb voice.play [fade]`. */
    if (n < 2 || n > 3) return fmt("ERR usage: %s <voice>[.<play>] [fade-seconds]", verb);
    if (n == 3 && isnan(parse_float_arg(w[2], NAN)))
      return fmt("ERR the fade is seconds, not %s (a play is named %s.%s)", w[2], w[1], w[2]);
    double fade = n == 3 ? parse_float_arg(w[2], 0.0) : 0.0;
    /* `bass` is a voice, `bass.drone` its play `drone`. A play's name has
     * no dot, so the last one splits them (a voice's file name may have
     * dots: `bass.v2` is a voice if one is called that). */
    char* voice = sdup(w[1]);
    char* part = NULL;
    char* dot = strrchr(voice, '.');
    lock_for_command();
    int whole = find_slot(w[1]) >= 0;
    unlock();
    if (dot && !whole) {
      *dot = 0;
      part = dot + 1;
    }
    char* err;
    if (!part) {
      err = IS("solo") ? engine_solo(voice, fade)
                       : engine_silence(voice, IS("mute") || IS("park"), IS("park") || IS("unpark"), fade);
    } else {
      int what = IS("mute") ? PART_MUTE : IS("unmute") ? PART_UNMUTE : IS("park") ? PART_PARK
               : IS("unpark") ? PART_UNPARK : PART_SOLO;
      err = part_gesture(voice, part, what, fade);
    }
    free(voice);
    return ok_or_err(err);
  }
  if (IS("clear")) {
    clear_all(n >= 2 ? parse_float_arg(w[1], 0.0) : 0.0);
    return sdup("OK");
  }
  if (IS("list")) {
    VoiceSnap* v;
    int nv = engine_voices(&v);
    char* r = ok_body(aither_fmt_list(v, nv));
    engine_free_voices(v, nv);
    return r;
  }
  if (IS("levels")) return ok_body(levels(n >= 2 ? w[1] : ""));
  if (IS("spectrum")) return spectrum(n >= 2 ? w[1] : "");
  if (IS("sweep")) {
    /* `sweep <name>`: where its sweep is; `sweep <name> <seconds>`: set it. */
    if (n < 2) return sdup("ERR usage: sweep <name> [seconds]");
    if (n == 2) {
      lock_for_command();
      int idx = find_slot(w[1]);
      double at = idx >= 0 ? time_sec + time_frac - slots[idx].voice->sweep_origin : 0.0;
      unlock();
      return idx < 0 ? fmt("ERR not found: %s", w[1]) : fmt("OK\n%.1f", at);
    }
    double secs;
    if (!parse_float(w[2], &secs)) return fmt("ERR sweep: seconds must be a number, got %s", w[2]);
    return ok_or_err(sweep_voice(w[1], secs));
  }
  if (IS("check")) {
    if (n < 2) return sdup("ERR usage: check <voice>");
    return ok_body(check(w[1]));
  }
  if (IS("try")) {
    if (n < 2) return sdup("ERR usage: try <file> [seconds] [skip]");
    double secs = n >= 3 ? parse_float_arg(w[2], 2.0) : 2.0;
    double skip = n >= 4 ? parse_float_arg(w[3], 0.0) : 0.0;
    return try_patch(w[1], fmax_nim(0.05, secs), fmax_nim(0.0, skip));
  }
  if (IS("parts")) {
    if (n < 2) return sdup("ERR usage: parts <voice>");
    return ok_body(parts_of(w[1]));
  }
  if (IS("profile")) {
    if (n < 2) return sdup("ERR usage: profile <voice> [seconds]");
    return profile_voice(w[1], fclamp(n >= 3 ? parse_float_arg(w[2], 2.0) : 2.0, 0.2, 30.0));
  }
  if (IS("midi")) {
    if (n < 2) return sdup("ERR usage: midi list|connect <spec>|disconnect");
    return aither_midi_command(n - 1, (const char**)w + 1);
  }
  if (IS("kill")) {
    killed = 1;
    running = 0;
    return sdup("OK bye");
  }
#undef IS
  return fmt("ERR unknown: %s", w[0]);
}

char* engine_command(const char* line) {
  char* copy = sdup(line);
  char** w = calloc(strlen(copy) / 2 + 2, sizeof *w);
  int n = 0;
  for (char* save = NULL, *t = strtok_r(copy, " \t\v\r\n\f", &save); t;
       t = strtok_r(NULL, " \t\v\r\n\f", &save))
    w[n++] = t;
  char* r = handle(n, w);
  free(w);
  free(copy);
  return r;
}

/* --------------------------------------------------------------- the pipe */
/* The engine's one output: raw interleaved float pairs on stdout, a block
 * at a time. Whatever reads the pipe (a player, a scope, an SDR, a file)
 * gives the numbers their meaning and, if it runs in real time, paces the
 * engine by blocking the write. `input()` reads the same format from
 * stdin when stdin is a pipe. A write that fails (the reader went away)
 * ends the engine. Both ends use read(2) and write(2), not stdio: stdio
 * reads ahead and holds writes back, and a program on both ends of the
 * engine (tools/card) needs one block out for each block in. */

static long long pipe_frames;            /* frames written so far */
static long long pipe_limit;             /* stop after this many (0: until killed) */

static void pump_pipe(void) {
  int frames = (int)buffer_frames;
  float* out_buf = calloc(frames * 2, sizeof(float));
  float* in_buf = calloc(frames * 2, sizeof(float));
  double* mix_l = calloc(frames, sizeof(double));
  double* mix_r = calloc(frames, sizeof(double));
  blk_l = calloc(frames, sizeof(double));
  blk_r = calloc(frames, sizeof(double));
  blk_in = calloc(frames * 2, sizeof(double));
  /* Live, the engine is a filter: it reads a pipe on stdin whatever the
   * patch does, one block in for each block out (tools/card relies on
   * that). Offline it reads only for a patch that calls input(), so a
   * render started with an idle pipe on stdin (ssh, a script) can't wait. */
  int have_input = !isatty(0) && (pipe_limit == 0 || input_used);
  int input_open = have_input;
  while (running && (pipe_limit == 0 || pipe_frames < pipe_limit)) {
    int want = pipe_limit > 0 && pipe_limit - pipe_frames < frames ? (int)(pipe_limit - pipe_frames)
                                                                   : frames;
    long bytes = (long)want * 2 * sizeof(float);
    if (input_open) {
      /* A pipe delivers in pieces; silence once the source has ended. */
      long got = 0;
      while (got < bytes) {
        ssize_t k = read(0, (char*)in_buf + got, bytes - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) {
          input_open = 0;
          break;
        }
        got += k;
      }
      for (long i = got / (long)sizeof(float); i < want * 2; i++) in_buf[i] = 0.0f;
    }
    mix_block(mix_l, mix_r, have_input ? in_buf : NULL, want);
    /* The honest mix, nothing applied: a limiter here would be the engine
     * deciding it is feeding a speaker. A player clamps; a patch that
     * wants a safety net ends with `|> tanh`. */
    for (int i = 0; i < want; i++) {
      out_buf[2 * i] = (float)mix_l[i];
      out_buf[2 * i + 1] = (float)mix_r[i];
    }
    long put = 0;
    while (put < bytes) {
      ssize_t k = write(1, (char*)out_buf + put, bytes - put);
      if (k < 0 && errno == EINTR) continue;
      if (k <= 0) break;
      put += k;
    }
    if (put < bytes) break;
    pipe_frames += want;
  }
  running = 0;
}

static void* pipe_worker(void* arg) {
  (void)arg;
  aither_realtime("aither");
  pump_pipe();
  /* The reader closed the pipe: that is the end of the session. After a
   * `kill` the server thread ends it, and says bye. */
  if (!killed) {
    unlink(socket_path);
    exit(0);
  }
  return NULL;
}

/* ---------------------------------------------------------------- the engine */

void engine_init(const char* stdlib_src) {
  stdlib_text = stdlib_src;
  socket_path = getenv("AITHER_SOCK") ? getenv("AITHER_SOCK") : "/tmp/aither.sock";
  buffer_frames = (unsigned)env_float("AITHER_BUFFER", 512.0);
  watchdog = env_float("AITHER_WATCHDOG", 0.5);
  sample_rate = (unsigned)env_float("AITHER_RATE", 48000.0);
  env_bin_samples = (int)(sample_rate / 20);
  recent_samples = (int)(sample_rate / 2) < MAX_RECENT ? (int)(sample_rate / 2) : MAX_RECENT;
}

static char* absolute(const char* p) {
  if (p[0] == '/') return sdup(p);
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) return sdup(p);
  return fmt("%s/%s", cwd, p);
}

static char* read_line(int fd) {
  /* One line from a client, without its newline; "" if it hung up first. */
  Buf b = {0};
  char c;
  for (;;) {
    ssize_t k = read(fd, &c, 1);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) {
      free(b.s);
      return sdup("");
    }
    if (c == '\n') break;
    bputn(&b, &c, 1);
  }
  if (b.n > 0 && b.s[b.n - 1] == '\r') b.s[--b.n] = 0;
  return bdone(&b);
}

void engine_run(char** files, int nfiles, double seconds) {
  /* The voices in `files` (and any sent later), mixed to stdout as float
   * pairs. With `seconds`, run that long from t=0 and exit (offline, as
   * fast as the reader takes it, no socket, no MIDI); without, run until
   * `kill`, listening on the socket for sends and gestures. */
  if (isatty(1)) {
    fprintf(stderr, "aither: stdout is a terminal. Pipe me into a player:\n");
    fprintf(stderr, "  aither | aplay -f FLOAT_LE -c 2 -r %u\n", sample_rate);
    fprintf(stderr, "  aither | tools/play\n");
    fprintf(stderr, "or run offline:  aither set.aither 30 > take.raw\n");
    exit(1);
  }
  signal(SIGPIPE, SIG_IGN);
  if (seconds <= 0.0) start_optimizer();
  for (int i = 0; i < nfiles; i++) {
    char* path = absolute(files[i]);
    char* err = engine_load(path, 0.0);
    free(path);
    if (err) {
      fprintf(stderr, "aither: %s\n", err);
      exit(1);
    }
  }
  if (seconds > 0.0) {
    /* Offline: one deterministic pass, cc -O2 built up front when it pays. */
    if (seconds >= 4.0)
      for (int i = 0; i < slot_count; i++) optimize_now(slots[i].voice);
    pipe_limit = (long long)(seconds * (double)sample_rate);
    running = 1;
    pump_pipe();
    exit(0);
  }

  fprintf(stderr, "aither \xC2\xB7 %u Hz \xC2\xB7 %s%s\n", sample_rate, socket_path,
          !isatty(0) ? " \xC2\xB7 input() reads stdin" : "");
  pthread_t th;
  pthread_create(&th, NULL, doctor, NULL);
  pthread_detach(th);
  /* MIDI input. Without a sequencer (a container, a chroot) it says so and
   * carries on: midi_* read zero, patches still run. */
  aither_midi_up();

  /* A socket left by an engine that was killed blocks the bind. If an
   * engine answers on it, it's live: say so and stop. Otherwise it's
   * stale; remove it. */
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", socket_path);
  int probe = socket(AF_UNIX, SOCK_STREAM, 0);
  if (connect(probe, (struct sockaddr*)&addr, sizeof addr) == 0) {
    close(probe);
    fprintf(stderr, "aither: an engine is already running on %s"
                    " (aither kill stops it; AITHER_SOCK runs a second one)\n", socket_path);
    exit(1);
  }
  close(probe);
  unlink(socket_path);
  int server = socket(AF_UNIX, SOCK_STREAM, 0);
  if (bind(server, (struct sockaddr*)&addr, sizeof addr) != 0 || listen(server, SOMAXCONN) != 0) {
    fprintf(stderr, "aither: can't listen on %s: %s\n", socket_path, strerror(errno));
    exit(1);
  }

  running = 1;
  pthread_create(&th, NULL, pipe_worker, NULL);
  pthread_detach(th);
  while (running) {
    int client = accept(server, NULL, NULL);
    if (client < 0) continue;
    /* A client that hung up without a command (a probe, a killed `aither
     * list`) gets no reply, and one that hangs up before reading it costs
     * nothing: the failed send is dropped. */
    char* cmd = read_line(client);
    if (*cmd) {
      char* reply = engine_command(cmd);
      size_t len = strlen(reply);
      reply = realloc(reply, len + 2);
      reply[len] = '\n';
      for (size_t put = 0; put <= len;) {
        ssize_t k = send(client, reply + put, len + 1 - put, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        put += k;
      }
      free(reply);
    }
    free(cmd);
    close(client);
  }
  /* The OS reaps the threads, the sockets and every fd at exit; only the
   * socket file outlives the process. */
  unlink(socket_path);
  fprintf(stderr, "[aither] bye\n");
  exit(0);
}
