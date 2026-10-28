/* Temporal audit — windowed RMS / centroid / peak-frequency analysis
 * of a patch's render. Used to characterise cybernetic patches whose
 * spectrum/RMS evolves over time (a property the default audit's
 * single-FFT-over-the-whole-buffer flattens out).
 *
 * Run via:
 *   tests/temporal_audit <patch> <seconds> [windowSec]
 *
 * Reports per-window RMS dB, centroid Hz, dominant peak (Hz, dB),
 * and aggregate stats: mean and stddev of centroid + RMS across
 * windows. A static patch has near-zero stddev; a cybernetic patch
 * has large stddev. */

#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../render.h"
#include "../analysis.h"

extern const char aither_stdlib[];

static double mean(const double* xs, int n) {
  if (n == 0) return 0.0;
  double s = 0.0;
  for (int i = 0; i < n; i++) s += xs[i];
  return s / n;
}

static double stddev(const double* xs, int n) {
  if (n < 2) return 0.0;
  double m = mean(xs, n), s = 0.0;
  for (int i = 0; i < n; i++) s += (xs[i] - m) * (xs[i] - m);
  return sqrt(s / (n - 1));
}

static double number(const char* s) {
  char* end;
  double x = strtod(s, &end);
  if (!*s || *end) {
    fprintf(stderr, "temporal_audit: not a number: %s\n", s);
    exit(1);
  }
  return x;
}

/* A float as Nim's $ writes it: the shortest that reads back, with ".0"
 * when it would look like an integer. */
static const char* nim_float(double x, char* buf, int len) {
  for (int p = 1; p <= 17; p++) {
    snprintf(buf, len, "%.*g", p, x);
    if (strtod(buf, NULL) == x) break;
  }
  if (!strpbrk(buf, ".eni")) strncat(buf, ".0", len - strlen(buf) - 1);
  return buf;
}

static char* slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 8192, n = 0, k;
  char* s = malloc(cap);
  while ((k = fread(s + n, 1, cap - n - 1, f)) > 0) {
    n += k;
    if (n + 1 >= cap) s = realloc(s, cap *= 2);
  }
  fclose(f);
  s[n] = 0;
  return s;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: temporal_audit <patch> <seconds> [windowSec=1.0]\n");
    return 1;
  }
  const char* patch = argv[1];
  double seconds = number(argv[2]);
  double window_sec = argc >= 4 ? number(argv[3]) : 1.0;

  const int sr = 48000;
  int n_win = (int)(seconds / window_sec);
  if (n_win < 2) {
    fprintf(stderr, "need at least 2 windows: increase seconds or shrink windowSec\n");
    return 1;
  }

  char b1[64], b2[64];
  printf("temporal_audit: %s  total=%ss  window=%ss  N=%d\n", patch, nim_float(seconds, b1, sizeof b1),
         nim_float(window_sec, b2, sizeof b2), n_win);
  printf("  win   t(s)     RMS(dB)   centroid(Hz)   fund(Hz)   peak(Hz @ dB)\n");
  fflush(stdout);

  char* src = slurp(patch);
  if (!src) {
    fprintf(stderr, "temporal_audit: patch not found: %s\n", patch);
    return 1;
  }
  Rendered out;
  char err[4096];
  if (render_source(aither_stdlib, src, patch, seconds, sr, NULL, NULL, 0, 0.0, &out, err, sizeof err)) {
    fprintf(stderr, "temporal_audit: %s\n", err);
    return 1;
  }
  double* mono = mono_mix(out.left, out.right, out.n);

  long win_samples = (long)(window_sec * sr);
  double* rms_s = calloc(n_win, sizeof(double));
  double* centroid_s = calloc(n_win, sizeof(double));
  double* fund_s = calloc(n_win, sizeof(double));
  double* peak_hz_s = calloc(n_win, sizeof(double));
  double* peak_db_s = calloc(n_win, sizeof(double));
  int k = 0;

  for (int i = 0; i < n_win; i++) {
    long start = i * win_samples;
    long stop = start + win_samples < out.n ? start + win_samples : out.n;
    if (stop - start < 1024) break;
    SpectrumSummary s;
    analyze(mono + start, stop - start, sr, 8, &s);
    double pk_hz = s.npeaks > 0 ? s.peaks[0].freq_hz : 0.0;
    double pk_db = s.npeaks > 0 ? s.peaks[0].mag_db : -240.0;
    rms_s[k] = s.rms_db;
    centroid_s[k] = s.centroid_hz;
    fund_s[k] = s.fundamental_hz;
    peak_hz_s[k] = pk_hz;
    peak_db_s[k] = pk_db;
    k++;
    double t = i * window_sec;
    printf("  %3d  %5.2f    %7.2f    %9.1f    %7.1f    %7.1f @ %6.2f\n", i, t, s.rms_db, s.centroid_hz,
           s.fundamental_hz, pk_hz, pk_db);
  }

  printf("\n");
  printf("aggregate over %d windows:\n", n_win);
  printf("  RMS dB           mean=%7.2f   stddev=%6.2f\n", mean(rms_s, k), stddev(rms_s, k));
  printf("  centroid Hz      mean=%7.1f   stddev=%6.1f\n", mean(centroid_s, k), stddev(centroid_s, k));
  printf("  fundamental Hz   mean=%7.1f   stddev=%6.1f\n", mean(fund_s, k), stddev(fund_s, k));
  printf("  dominant peak Hz mean=%7.1f   stddev=%6.1f\n", mean(peak_hz_s, k), stddev(peak_hz_s, k));
  printf("  peak dB          mean=%7.2f   stddev=%6.2f\n", mean(peak_db_s, k), stddev(peak_db_s, k));
  return 0;
}
