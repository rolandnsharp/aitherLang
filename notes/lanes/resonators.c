/* A bank of N driven resonators (waterfall's dho, as codegen writes it):
 * A: as today, instance after instance, state [flag, re, im] per partial;
 * B: lanes, each field a row of N, terms into rows, summed in order after.
 * Same arithmetic in the same order per partial; prints time and whether
 * every output sample has the same bits. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { N = 64, SAMPLES = 480000, BLOCK = 128 };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static double sl[N], sr[N], wd[N], wf[N];   /* hoisted per partial, once a block */

__attribute__((noinline)) static void run_a(double* restrict pool, const double* restrict sig, double dt,
                                            double* restrict outl, double* restrict outr, long n) {
  for (long i = 0; i < n; i++) {
    double accl = 0, accr = 0;
    for (int k = 0; k < N; k++) {   /* stands for the unrolled copies */
      double* p = pool + 3 * k;
      if (p[0] == 0.0) { p[1] = 0.0; p[2] = 0.0; p[0] = 1.0; }
      double force = sig[i] * 2.0 * 0.04 * wf[k] * wf[k];
      double a = p[1], b = p[2];
      double nl = (a * sl[k] - b * sr[k]) + 0.0;
      double nr = (a * sr[k] + b * sl[k]) + (wd[k] == 0.0 ? 0.0 : (-force * dt) / wd[k]);
      p[1] = nl; p[2] = nr;
      if (k == 0) { accl = nl; accr = nr; } else { accl += nl; accr += nr; }
    }
    outl[i] = accl; outr[i] = accr;
  }
}

__attribute__((noinline)) static void run_b(double* restrict flag, double* restrict re, double* restrict im,
                                            const double* restrict sig, double dt,
                                            double* restrict outl, double* restrict outr, long n) {
  double tl[N], tr[N];
  for (long i = 0; i < n; i++) {
    const double sg = sig[i];
    for (int k = 0; k < N; k++) {
      double f0 = flag[k];
      double a = f0 == 0.0 ? 0.0 : re[k], b = f0 == 0.0 ? 0.0 : im[k];
      flag[k] = 1.0;
      double force = sg * 2.0 * 0.04 * wf[k] * wf[k];
      double nl = (a * sl[k] - b * sr[k]) + 0.0;
      double nr = (a * sr[k] + b * sl[k]) + (wd[k] == 0.0 ? 0.0 : (-force * dt) / wd[k]);
      re[k] = nl; im[k] = nr;
      tl[k] = nl; tr[k] = nr;
    }
    double accl = tl[0], accr = tr[0];
    for (int k = 1; k < N; k++) { accl += tl[k]; accr += tr[k]; }
    outl[i] = accl; outr[i] = accr;
  }
}

int main(void) {
  double sr_ = 48000, dt = 1 / sr_;
  for (int k = 0; k < N; k++) {
    double f = 50 * pow(2, k / 8.0), w = 2 * M_PI * f, d = 0.04;
    double r = exp(-d * w * dt);
    sl[k] = r * cos(w * dt * sqrt(1 - d * d));
    sr[k] = r * sin(w * dt * sqrt(1 - d * d));
    wd[k] = w * sqrt(1 - d * d);
    wf[k] = w;
  }
  double* sig = malloc(SAMPLES * sizeof *sig);
  unsigned x = 1;
  for (long i = 0; i < SAMPLES; i++) { x = x * 1664525u + 1013904223u; sig[i] = (x >> 8) / 16777216.0 - 0.5; }
  double *al = malloc(SAMPLES * 8), *ar = malloc(SAMPLES * 8), *bl = malloc(SAMPLES * 8), *br = malloc(SAMPLES * 8);
  double* pool = calloc(3 * N, 8);
  double *flag = calloc(N, 8), *re = calloc(N, 8), *im = calloc(N, 8);
  double t0 = now();
  for (long i = 0; i < SAMPLES; i += BLOCK) run_a(pool, sig + i, dt, al + i, ar + i, BLOCK);
  double ta = now() - t0;
  t0 = now();
  for (long i = 0; i < SAMPLES; i += BLOCK) run_b(flag, re, im, sig + i, dt, bl + i, br + i, BLOCK);
  double tb = now() - t0;
  long diff = 0;
  for (long i = 0; i < SAMPLES; i++) diff += memcmp(&al[i], &bl[i], 8) != 0 || memcmp(&ar[i], &br[i], 8) != 0;
  printf("as today %.3f s, lanes %.3f s (%.2fx), samples that differ: %ld\n", ta, tb, ta / tb, diff);
  return 0;
}
