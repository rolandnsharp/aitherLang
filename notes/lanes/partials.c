/* A bank of N turned partials (sum(N, k => amp k * sin(TAU * phasor(f k)))):
 * A as codegen writes it (each partial's [re, im, flag] in turn, flag
 * checked each sample, terms added as they come); B lanes (each field a
 * row of N, flags settled once a block, terms into a row, added in order). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
enum { N = 64, SAMPLES = 480000, BLOCK = 128 };
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double cs[N], sn[N], amp[N];
static inline void turn_step(double* p, double c, double s) {
  double r = p[0] * c - p[1] * s, i = p[0] * s + p[1] * c, g = 1.5 - 0.5 * (r * r + i * i);
  p[0] = r * g; p[1] = i * g;
}
__attribute__((noinline)) static void run_a(double* restrict pool, double* restrict out, long n) {
  for (long i = 0; i < n; i++) {
    double acc = 0;
    for (int k = 0; k < N; k++) {
      double* p = pool + 3 * k;
      if (p[2] == 0.0) { p[0] = 1.0; p[1] = 0.0; p[2] = 1.0; }
      turn_step(p, cs[k], sn[k]);
      double t = amp[k] * p[1];
      if (k == 0) acc = t; else acc += t;
    }
    out[i] = acc;
  }
}
__attribute__((noinline)) static void run_c(double* restrict pool, double* restrict out, long n) {
  for (int k = 0; k < N; k++) { double* p = pool + 3 * k; if (p[2] == 0.0) { p[0] = 1.0; p[1] = 0.0; p[2] = 1.0; } }
  for (long i = 0; i < n; i++) {
    double acc = 0;
    for (int k = 0; k < N; k++) {
      double* p = pool + 3 * k;
      turn_step(p, cs[k], sn[k]);
      double t = amp[k] * p[1];
      if (k == 0) acc = t; else acc += t;
    }
    out[i] = acc;
  }
}
__attribute__((noinline)) static void run_b(double* restrict re, double* restrict im, double* restrict flag,
                                            double* restrict out, long n) {
  for (int k = 0; k < N; k++) if (flag[k] == 0.0) { re[k] = 1.0; im[k] = 0.0; flag[k] = 1.0; }
  double t[N];
  for (long i = 0; i < n; i++) {
    for (int k = 0; k < N; k++) {
      double r = re[k] * cs[k] - im[k] * sn[k], q = re[k] * sn[k] + im[k] * cs[k], g = 1.5 - 0.5 * (r * r + q * q);
      re[k] = r * g; im[k] = q * g;
      t[k] = amp[k] * im[k];
    }
    double acc = t[0];
    for (int k = 1; k < N; k++) acc += t[k];
    out[i] = acc;
  }
}
int main(void) {
  for (int k = 0; k < N; k++) { double w = 2 * M_PI * 110 * (k + 1) / 48000; cs[k] = cos(w); sn[k] = sin(w); amp[k] = 1.0 / (k + 1); }
  double *a = malloc(SAMPLES * 8), *b = malloc(SAMPLES * 8), *pool = calloc(3 * N, 8);
  double *re = calloc(N, 8), *im = calloc(N, 8), *fl = calloc(N, 8);
  double t0 = now();
  for (long i = 0; i < SAMPLES; i += BLOCK) run_a(pool, a + i, BLOCK);
  double ta = now() - t0; t0 = now();
  for (long i = 0; i < SAMPLES; i += BLOCK) run_b(re, im, fl, b + i, BLOCK);
  double tb = now() - t0;
  double *c = malloc(SAMPLES * 8), *pool2 = calloc(3 * N, 8);
  t0 = now();
  for (long i = 0; i < SAMPLES; i += BLOCK) run_c(pool2, c + i, BLOCK);
  double tc = now() - t0;
  long dc = 0; for (long i = 0; i < SAMPLES; i++) dc += memcmp(&a[i], &c[i], 8) != 0;
  printf("flag once a block, as today otherwise: %.3f s (%.2fx), differ: %ld\n", tc, ta / tc, dc);
  long d = 0; for (long i = 0; i < SAMPLES; i++) d += memcmp(&a[i], &b[i], 8) != 0;
  printf("as today %.3f s, lanes %.3f s (%.2fx), differ: %ld\n", ta, tb, ta / tb, d);
}
