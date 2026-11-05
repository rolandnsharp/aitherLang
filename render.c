/* Offline render (render.h). */

#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "render.h"
#include "imports.h"
#include "dsp.h"

#define PI 3.141592653589793
/* A render this long (lead-in included) is built with cc -O2 first: the
 * seconds of compiling pay for themselves against TCC's slow code. */
#define OPTIMIZE_OVER_SECONDS 4.0

double render_low_split_hz(void) {
  static double hz = -1.0;
  if (hz < 0.0) {
    const char* v = getenv("AITHER_LOW_HZ");
    char* end;
    double x = v && *v ? strtod(v, &end) : 0.0;
    hz = v && *v && !*end ? x : 200.0;
  }
  return hz;
}

static int have_cc(void) {
  const char* path = getenv("PATH");
  if (!path) return 0;
  char* copy = strdup(path);
  int found = 0;
  for (char* save = NULL, *d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
    char full[1024];
    struct stat st;
    snprintf(full, sizeof full, "%s/cc", d);
    found = access(full, X_OK) == 0 && stat(full, &st) == 0 && S_ISREG(st.st_mode);
  }
  free(copy);
  return found;
}

int render_cc_available(void) {
  const char* o = getenv("AITHER_OPT");
  return !(o && strcmp(o, "0") == 0) && have_cc();
}

void render_optimize(Voice* v) {
  if (!render_cc_available()) return;
  void* lib;
  TickFn fn;
  char err[2048];
  if (voice_compile_optimized(v->cur.csrc, v->cur.state_size, 0, &lib, &fn, err, sizeof err) == 0)
    voice_upgrade(v, v->gen, lib, fn);
}

/* A 4th-order Butterworth lowpass (two biquads) for the share below the
 * split: steep enough that a 330 Hz tone reads ~2%, a 100 Hz one ~100%. */
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } Biquad;

static Biquad lowpass(double fc, double q, int sr) {
  double w = 2.0 * PI * fc / (double)sr;
  double alpha = sin(w) / (2.0 * q);
  double a0 = 1.0 + alpha;
  Biquad f = {0};
  f.b0 = (1.0 - cos(w)) / 2.0 / a0;
  f.b1 = (1.0 - cos(w)) / a0;
  f.b2 = f.b0;
  f.a1 = -2.0 * cos(w) / a0;
  f.a2 = (1.0 - alpha) / a0;
  return f;
}

static double step(Biquad* f, double x) {
  double y = f->b0 * x + f->z1;
  f->z1 = f->b1 * x - f->a1 * y + f->z2;
  f->z2 = f->b2 * x - f->a2 * y;
  return y;
}

static double dmax(double x, double y) { return y <= x ? x : y; }

int render_voice(Voice* v, double seconds, int sr, const double* in_l, const double* in_r,
                 long n_in, double skip, double t0, int feed_input, Rendered* out,
                 char* err, int errlen) {
  long n_skip = (long)(skip * (double)sr);
  long n = (long)(seconds * (double)sr);
  if (n_skip < 0) n_skip = 0;
  if (n < 0) n = 0;
  if ((double)(n_skip + n) / (double)sr >= OPTIMIZE_OVER_SECONDS) render_optimize(v);
  memset(out, 0, sizeof *out);
  out->seconds = (double)n / (double)sr;
  out->skipped = (double)n_skip / (double)sr;
  out->n = n;
  out->left = calloc(n ? n : 1, sizeof(double));
  out->right = calloc(n ? n : 1, sizeof(double));
  int np = v->cur.nparts;
  double* sum_sq = calloc(np + 1, sizeof(double));
  double* low_sq = calloc(np + 1, sizeof(double));
  double* peak = calloc(np + 1, sizeof(double));
  Biquad* lp1 = calloc(np + 1, sizeof(Biquad));
  Biquad* lp2 = calloc(np + 1, sizeof(Biquad));
  double split = render_low_split_hz();
  for (int p = 0; p < np; p++) {
    lp1[p] = lowpass(split, 0.5412, sr);
    lp2[p] = lowpass(split, 1.3066, sr);
  }
  /* A block at a time through tick_block, as the engine runs it, from
   * t0's step (sample i at step t0 sr + i), every sample as the voice made
   * it (no guard, no fade) and every part's sound each sample. A gain
   * ramp in progress holds, as it did a sample at a time. */
  enum { BLK = 512 };
  VoiceHeader* h = v->cur.state;
  double* raw = malloc(2 * BLK * sizeof(double));
  double* inb = malloc(2 * BLK * sizeof(double));
  double* pb = malloc((size_t)(np > 0 ? 2 * np : 2) * BLK * sizeof(double));
  double* scratch = malloc(2 * BLK * sizeof(double));
  double* held = calloc(np + 1, sizeof(double));
  for (int p = 0; p < np; p++) { held[p] = h->part_fade_delta[p]; h->part_fade_delta[p] = 0.0; }
  double* was_in = h->in_buf;
  double was_fade = h->fade_gain, was_delta = h->fade_delta;
  h->fade_gain = 1.0; h->fade_delta = 0.0;
  h->step = nearbyint(t0 * (double)sr);
  h->raw_out = raw; h->part_buf = pb; h->in_buf = inb;
  int rc = 0;
  long total = n_skip + n;
  for (long i0 = 0; i0 < total && rc == 0; i0 += BLK) {
    long m = total - i0 < BLK ? total - i0 : BLK;
    for (long k = 0; k < m; k++) {
      long i = i0 + k;
      if (feed_input) {             /* the recording a frame at a time; silence past its end */
        inb[2 * k] = i < n_in ? in_l[i] : 0.0;
        inb[2 * k + 1] = i < n_in ? in_r[i] : 0.0;
      } else {
        inb[2 * k] = dsp_input_l;
        inb[2 * k + 1] = dsp_input_r;
      }
    }
    voice_tick_block(v, m, scratch, scratch + BLK);
    for (long k = 0; k < m; k++) {
      long i = i0 + k;
      double l = raw[2 * k], r = raw[2 * k + 1];
      if (l != l || r != r) {
        snprintf(err, errlen, "patch produced NaN at %.3fs (sample %ld of %ld)",
                 (double)i / (double)sr, i, total);
        rc = -1;
        break;
      }
      if (i < n_skip) continue;
      out->left[i - n_skip] = l;
      out->right[i - n_skip] = r;
      for (int p = 0; p < np; p++) {
        double pl = pb[k * 2 * np + 2 * p], pr = pb[k * 2 * np + 2 * p + 1];
        double mm = (pl + pr) * 0.5;
        double low = step(&lp2[p], step(&lp1[p], mm));
        sum_sq[p] += mm * mm;
        low_sq[p] += low * low;
        peak[p] = dmax(peak[p], dmax(fabs(pl), fabs(pr)));
      }
    }
  }
  h->raw_out = NULL; h->part_buf = NULL; h->in_buf = was_in;
  h->fade_gain = was_fade; h->fade_delta = was_delta;
  for (int p = 0; p < np; p++) h->part_fade_delta[p] = held[p];
  free(raw); free(inb); free(pb); free(scratch); free(held);
  if (rc == 0) {
    out->parts = calloc(np > 0 ? (size_t)np : 1, sizeof(PartLevel));
    out->nparts = np;
    for (int p = 0; p < np; p++) {
      const char* name = v->cur.part_names[p];
      int unheard = 0;
      for (int k = 0; k < v->cur.nunheard; k++)
        if (strcmp(v->cur.unheard[k], name) == 0) unheard = 1;
      double low = sum_sq[p] > 0.0 ? low_sq[p] / sum_sq[p] : 0.0;
      out->parts[p] = (PartLevel){strdup(name), n > 0 ? sqrt(sum_sq[p] / (double)n) : 0.0, peak[p],
                                  low < 1.0 ? low : 1.0, v->part_gains[p], unheard};
    }
  } else {
    rendered_free(out);
  }
  free(sum_sq); free(low_sq); free(peak); free(lp1); free(lp2);
  return rc;
}

void rendered_free(Rendered* r) {
  free(r->left);
  free(r->right);
  for (int i = 0; i < r->nparts; i++) free(r->parts[i].name);
  free(r->parts);
  memset(r, 0, sizeof *r);
}

static int hoist_on(void) {
  const char* h = getenv("AITHER_HOIST");
  return !(h && strcmp(h, "0") == 0);
}

int render_source(const char* stdlib_src, const char* src, const char* tag, double seconds,
                  int sr, const double* in_l, const double* in_r, long n_in, double skip,
                  Rendered* out, char* err, int errlen) {
  int kind;
  Prepared* p = voice_prepare(stdlib_src, src, tag, "", (double)sr, 0, hoist_on(), &kind, err, errlen);
  if (!p) return kind;
  Voice* v = voice_new();
  voice_commit(v, p);
  int rc = render_voice(v, seconds, sr, in_l, in_r, n_in, skip, 0.0, 1, out, err, errlen);
  voice_dispose(v);
  return rc == 0 ? 0 : RENDER_NAN;
}

int render_calls_input(const char* stdlib_src, const char* src, const char* tag,
                       char* err, int errlen) {
  Arena* a = arena_new();
  int kind;
  Node* prog = build_program(a, stdlib_src, src, tag, 0, &kind, err, errlen);
  int r = prog ? calls_input(prog) != 0 : -1;
  arena_free(a);
  return r;
}
