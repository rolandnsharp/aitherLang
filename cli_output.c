/* The text (cli_output.h). */

#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli_output.h"

static const char* spark_blocks[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

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
static void bput(Buf* b, const char* s) { bputn(b, s, strlen(s)); }
static void bprintf(Buf* b, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char* s = NULL;
  int n = vasprintf(&s, fmt, ap);
  va_end(ap);
  if (n > 0) bputn(b, s, n);
  free(s);
}
static char* bdone(Buf* b) { return b->s ? b->s : strdup(""); }
/* Lines joined by "\n": a line break before every line but the first. */
static void bline(Buf* b) { if (b->n) bput(b, "\n"); }

static double fmax_nim(double x, double y) { return y <= x ? x : y; }
static double db_of(double v) { return v < 1e-9 ? -180.0 : 20.0 * log10(v); }

/* AITHER_QUIET_DB, -24 unset: a part this far under the loudest is QUIET
 * (taste, so it is settable; 0 turns the flag off). */
static double quiet_part_db(void) {
  static int read;
  static double db;
  if (!read) {
    const char* v = getenv("AITHER_QUIET_DB");
    char* end = NULL;
    double x = v && *v ? strtod(v, &end) : 0.0;
    db = v && *v && !*end ? x : -24.0;
    read = 1;
  }
  return db;
}

void cli_fmt_db(double v, char* out) {
  if (v < 1e-9) strcpy(out, "  -inf");
  else snprintf(out, 16, "%.1f", db_of(v));
}

static const char* db_stripped(double v, char* out) {
  cli_fmt_db(v, out);
  return out[0] == ' ' ? out + 2 : out;      /* only "  -inf" has a margin */
}

static const char* env_bar(float v) {
  double f = v;
  if (f < 1e-5) return spark_blocks[0];
  int idx = (int)((db_of(f) + 60.0) / 60.0 * 8.0 + 0.5);
  return spark_blocks[idx < 0 ? 0 : idx > 8 ? 8 : idx];
}

static const char* voice_state_word(int s) {
  switch (s) {
  case VS_STOPPED:    return "stopped";
  case VS_MUTED:      return "muted";
  case VS_PARKED:     return "parked";
  case VS_FADING_OUT: return "fading-out";
  case VS_FADING_IN:  return "fading-in";
  case VS_PLAYING:    return "playing";
  default:            return "muted: runaway output";
  }
}

static const char* part_state_word(int s) {
  switch (s) {
  case PS_SILENT:     return "silent";
  case PS_FADING_OUT: return "fading-out";
  case PS_FADING_IN:  return "fading-in";
  case PS_PLAYING:    return "playing";
  default:            return "parked";
  }
}

char* cli_meter_line(const char* name, const StatsSnap* s) {
  /* The sparkline shows · for a blank bin. */
  Buf b = {0};
  char r[16], p[16];
  bprintf(&b, "%s  rms=%sdB peak=%sdB clips=%ld env=", name,
          db_stripped(fmax_nim(s->rms_l, s->rms_r), r), db_stripped(fmax_nim(s->peak_l, s->peak_r), p),
          s->clips);
  for (int k = 0; k < ENGINE_ENV_BINS; k++) {
    float v = s->env_ring[(s->env_bin_idx + k) % ENGINE_ENV_BINS];
    bput(&b, (double)v < 1e-5 ? "·" : env_bar(v));
  }
  return bdone(&b);
}

static void stats_block(Buf* b, const char* header, const StatsSnap* s) {
  char a[16], c[16];
  bprintf(b, "%s\n", header);
  cli_fmt_db(s->rms_l, a);
  cli_fmt_db(s->rms_r, c);
  bprintf(b, "  RMS   L %s dB   R %s dB\n", a, c);
  cli_fmt_db(s->peak_l, a);
  cli_fmt_db(s->peak_r, c);
  bprintf(b, "  peak  L %s dB   R %s dB   clips=%ld\n  env   ", a, c, s->clips);
  for (int k = 0; k < ENGINE_ENV_BINS; k++)
    bput(b, env_bar(s->env_ring[(s->env_bin_idx + k) % ENGINE_ENV_BINS]));
}

char* cli_scope(const char* target, int found, const VoiceStatsSnap* s, int n) {
  Buf b = {0};
  if (!found) {
    bprintf(&b, "not found: %s", target);
    return bdone(&b);
  }
  for (int i = 0; i < n; i++) {
    if (i) bput(&b, "\n\n");
    char header[512];
    if (s[i].is_master) snprintf(header, sizeof header, "master");
    else snprintf(header, sizeof header, "%s  %s  gain=%.2f", s[i].name, voice_state_word(s[i].state), s[i].gain);
    stats_block(&b, header, &s[i].stats);
  }
  return bdone(&b);
}

char* cli_parts(const char* voice, int found, const PartSnap* p, int n) {
  Buf b = {0};
  if (!found) bprintf(&b, "not found: %s", voice);
  else if (n == 0) bput(&b, "(no parts)");
  else {
    int w = 0;
    for (int i = 0; i < n; i++) if ((int)strlen(p[i].name) > w) w = (int)strlen(p[i].name);
    bput(&b, voice);
    for (int i = 0; i < n; i++) {
      char r[16], k[16];
      bprintf(&b, "\n  %-*s  [%s]  gain=%.2f  rms=%sdB peak=%sdB%s", w, p[i].name, part_state_word(p[i].state),
              p[i].gain, db_stripped(p[i].rms, r), db_stripped(p[i].peak, k),
              p[i].unheard ? "  NOT IN THE FINAL EXPRESSION" : "");
    }
  }
  return bdone(&b);
}

char* cli_check(const CheckSnap* q) {
  Buf b = {0};
  if (!q->found) {
    bprintf(&b, "not found: %s", q->voice.name);
    return bdone(&b);
  }
  char* meter = cli_meter_line(q->voice.name, &q->voice.stats);
  bprintf(&b, "%s  [%s]", meter, voice_state_word(q->voice.state));
  free(meter);
  if (q->rolled_back)
    bprintf(&b, "\n  ROLLED BACK: %s; the previous version is playing. Fix the patch and send again.",
            q->stuck ? "a recursion didn't return right after the swap" : "it blew up right after the swap");
  if (q->retrying) bput(&b, "\n  RUNAWAY: muted, state wiped, fading back in shortly");
  else if (q->voice.state == VS_RUNAWAY)
    bput(&b, q->stuck ? "\n  STUCK: a recursion didn't return; muted until resent" : "\n  RUNAWAY: muted until resent");
  if (q->nan_events > 0) bprintf(&b, "\n  NaN: %ld time(s) since the send (state reset)", q->nan_events);
  double quiet = quiet_part_db();
  double loudest = 0.0;
  for (int i = 0; i < q->nparts; i++)
    if (q->parts[i].gain > 0.0) loudest = fmax_nim(loudest, q->parts[i].rms);
  Buf ql = {0};
  for (int i = 0; i < q->nparts; i++) {
    const PartSnap* p = &q->parts[i];
    if (p->gain > 0.0 && loudest > 0.0 && quiet < 0.0 && db_of(p->rms) - db_of(loudest) < quiet) {
      char d[16];
      if (ql.n) bput(&ql, ", ");
      bprintf(&ql, "%s %s", p->name, db_stripped(p->rms, d));
    }
  }
  if (ql.n) bprintf(&b, "\n  quiet parts (>%ld dB under the loudest): %s", (long)-quiet, ql.s);
  free(ql.s);
  /* Parts that play and aren't heard. Static first: names the final
   * expression never reaches (a certainty). Then the rest: the loudest
   * heard part is healthy but the voice's output sits far below it, so
   * the final expression is scaling it down (a fade at 0, a stray `* 0`). */
  Buf ul = {0};
  double loudest_heard = 0.0;
  for (int i = 0; i < q->nparts; i++) {
    if (q->parts[i].unheard) {
      if (ul.n) bput(&ul, ", ");
      bput(&ul, q->parts[i].name);
    } else if (q->parts[i].gain > 0.0) {
      loudest_heard = fmax_nim(loudest_heard, q->parts[i].rms);
    }
  }
  if (ul.n) bprintf(&b, "\n  NOT IN THE FINAL EXPRESSION (computed, never heard): %s", ul.s);
  free(ul.s);
  double vrms = fmax_nim(q->voice.stats.rms_l, q->voice.stats.rms_r);
  if (loudest_heard > 1e-3 && (q->voice.state == VS_PLAYING || q->voice.state == VS_FADING_IN) &&
      db_of(vrms) - db_of(loudest_heard) < -30.0) {
    char a[16], c[16];
    bprintf(&b, "\n  QUIET VOICE: the loudest part is at %s dB but the voice puts out %s dB: the final expression is scaling it down",
            db_stripped(loudest_heard, a), db_stripped(vrms, c));
  }
  char a[16], c[16];
  bprintf(&b, "\nmaster  rms=%sdB peak=%sdB", db_stripped(fmax_nim(q->master.rms_l, q->master.rms_r), a),
          db_stripped(fmax_nim(q->master.peak_l, q->master.peak_r), c));
  return bdone(&b);
}

char* cli_midi_header(const char* info, int active) {
  Buf b = {0};
  if (info && *info) bprintf(&b, "MIDI: %s [%s]", info, active ? "active" : "DROPPED");
  return bdone(&b);
}

char* cli_voice_list(const char* midi_info, int midi_active, const VoiceSnap* v, int n) {
  Buf b = {0};
  char* head = cli_midi_header(midi_info, midi_active);
  bput(&b, head);
  free(head);
  for (int i = 0; i < n; i++) {
    bline(&b);
    bprintf(&b, "%s [%s] gain=%.2f", v[i].name, voice_state_word(v[i].state), v[i].gain);
    for (int k = 0; k < v[i].nparts; k++) {
      const PartSnap* p = &v[i].parts[k];
      bprintf(&b, "\n  %-8s gain=%.2f%s", p->name, p->gain,
              p->state == PS_PARKED ? " (parked)" : p->gain <= 0.0 ? " (muted)" : "");
    }
  }
  if (!b.n) bput(&b, "(no voices)");
  return bdone(&b);
}

static void fmt_hz(double v, char* out) {
  if (v >= 1000.0) snprintf(out, 48, "%.0f Hz", v);
  else snprintf(out, 48, "%.1f Hz", v);
}

char* cli_spectrum(const SpectrumSummary* s) {
  Buf b = {0};
  char hz[48];
  bprintf(&b, "  RMS:        %.1f dB    Peak: %.1f dB\n", s->rms_db, s->peak_db);
  if (s->fundamental_hz <= 0.0) bput(&b, "  Fundamental: (no clear pitch — non-tonal)\n");
  else {
    fmt_hz(s->fundamental_hz, hz);
    bprintf(&b, "  Fundamental: %s\n", hz);
  }
  fmt_hz(s->centroid_hz, hz);
  bprintf(&b, "  Centroid:   %s\n", hz);
  bprintf(&b, "  ZCR:        %.0f / sec\n", s->zero_crossing_rate);
  bput(&b, "  Top peaks:");
  for (int i = 0; i < s->npeaks; i++) {
    fmt_hz(s->peaks[i].freq_hz, hz);
    bprintf(&b, "\n    %2d.  %12s  %.1f dB", i + 1, hz, s->peaks[i].mag_db);
  }
  return bdone(&b);
}

char* cli_audit(const char* path, double seconds, double sr, const SpectrumSummary* s) {
  char* spec = cli_spectrum(s);
  Buf b = {0};
  bprintf(&b, "audit: %s (%.1fs @ %.0f Hz)\n%s", path, seconds, sr, spec);
  free(spec);
  return bdone(&b);
}

typedef struct { const PartLevel* p; int order; } Ranked;

static int ranked_cmp(const void* a, const void* b) {
  /* Loudest first; equal ones keep their order. */
  const Ranked* x = a; const Ranked* y = b;
  if (x->p->rms != y->p->rms) return x->p->rms < y->p->rms ? 1 : -1;
  return x->order - y->order;
}

char* cli_part_levels(const PartLevel* parts, int n) {
  if (n == 0) return strdup("");
  Ranked* ps = malloc(n * sizeof *ps);
  for (int i = 0; i < n; i++) ps[i] = (Ranked){&parts[i], i};
  qsort(ps, n, sizeof *ps, ranked_cmp);
  double loudest = 0.0, quiet = quiet_part_db();
  int w = 4;
  for (int i = 0; i < n; i++) {
    if (ps[i].p->gain > 0.0) loudest = fmax_nim(loudest, ps[i].p->rms);
    if ((int)strlen(ps[i].p->name) > w) w = (int)strlen(ps[i].p->name);
  }
  int split = (int)render_low_split_hz();
  Buf b = {0};
  bprintf(&b, "parts (rms / peak / share below %d Hz):", split);
  for (int i = 0; i < n; i++) {
    const PartLevel* p = ps[i].p;
    Buf notes = {0};
#define NOTE(...) do { if (notes.n) bput(&notes, "; "); bprintf(&notes, __VA_ARGS__); } while (0)
    if (p->unheard) NOTE("NOT IN THE FINAL EXPRESSION");
    if (p->gain <= 0.0) NOTE("muted");
    else if (p->rms < 1e-6) NOTE("SILENT");
    else {
      if (loudest > 0.0 && quiet < 0.0 && db_of(p->rms) - db_of(loudest) < quiet)
        NOTE("QUIET: %ld dB under the loudest", (long)(db_of(loudest) - db_of(p->rms)));
      if (p->low > 0.5) NOTE("mostly below %d Hz", split);
    }
#undef NOTE
    char r[16], k[16];
    cli_fmt_db(p->rms, r);
    cli_fmt_db(p->peak, k);
    bprintf(&b, "\n  %-*s  %s dB  %s dB  %3ld%%%s%s", w, p->name, r, k, (long)(p->low * 100.0 + 0.5),
            notes.n ? "  " : "", notes.n ? notes.s : "");
    free(notes.s);
  }
  free(ps);
  return bdone(&b);
}
