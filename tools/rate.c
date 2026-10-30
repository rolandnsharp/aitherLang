/* rate: a pair stream from one rate to another.
     AITHER_RATE=2400000 aither rx.aither | tools/rate 2400000 44100 | tools/play hw:rockchipes8388c 20
     aither tx.aither | tools/rate 48000 2400000 | (an SDR's transmitter)
   Reads raw interleaved float32 pairs at FROM Hz from stdin and writes them
   at TO Hz to stdout. The ratio is reduced to L/M (2.4 MHz to 48 kHz is
   1/50; 44.1 to 48 kHz is 160/147): in effect L - 1 zeros between input
   samples, one low-pass, then every Mth sample kept. The low-pass is what
   makes it a rate change and not a fold: keeping every 50th sample of a
   2.4 MHz stream with nothing first folds everything from 24 kHz to
   1.2 MHz into the audio band (notes/radio-2026-10-30.md measured the
   noise 24 dB higher). It is a windowed sinc (Blackman, about 74 dB down
   in its stop band) passing up to 0.4 of the lower rate and stopping by
   0.5 of it, so nothing above the lower rate's Nyquist folds back. Only
   the outputs kept are computed, each from the taps of its own phase
   (polyphase): about 55 times FROM / TO taps an output going down, 55 a
   side going up. Its delay is half the filter. Exits when the pipe
   closes. Build: make tools/rate */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define BLOCK 4096   /* frames read at a time */

static long gcd(long a, long b) { while (b) { long t = a % b; a = b; b = t; } return a; }

int main(int argc, char** argv) {
  if (argc != 3) { fprintf(stderr, "usage: tools/rate FROM TO   (Hz: float32 pairs at FROM in, at TO out)\n"); return 2; }
  long from = atol(argv[1]), to = atol(argv[2]);
  if (from <= 0 || to <= 0) { fprintf(stderr, "rate: the rates are whole numbers of Hz above 0\n"); return 2; }
  long g = gcd(from, to), L = to / g, M = from / g;
  double U = (double)from * L;                        /* the filter's rate: FROM with L - 1 zeros between */
  double low = from < to ? from : to;
  double pass = 0.4 * low, stop = 0.5 * low, fc = (pass + stop) / 2 / U;   /* cycles a sample at U */
  long N = (long)ceil(5.5 * U / (stop - pass)) | 1;                         /* Blackman: transition 5.5 / N */
  long T = (N + L - 1) / L;                                                 /* taps a phase */
  double* h = calloc((size_t)(T * L), sizeof(double));
  if (!h) { fprintf(stderr, "rate: %ld taps don't fit\n", N); return 1; }
  double sum = 0;
  for (long k = 0; k < N; k++) {
    double t = k - (N - 1) / 2.0;
    double sinc = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
    double w = 0.42 - 0.5 * cos(2 * M_PI * k / (N - 1)) + 0.08 * cos(4 * M_PI * k / (N - 1));
    h[k] = sinc * w;
    sum += h[k];
  }
  for (long k = 0; k < N; k++) h[k] *= L / sum;       /* gain 1: the zeros took 1 - 1/L of each sample */
  /* Phase p's taps, in the order they meet the input: q[p][j] = h[p + j L]. */
  double* q = calloc((size_t)(T * L), sizeof(double));
  for (long p = 0; p < L; p++)
    for (long j = 0; j < T; j++) q[p * T + j] = h[p + j * L];
  free(h);

  long cap = 1; while (cap < T + BLOCK) cap <<= 1;   /* the input's history, a ring */
  double* xl = calloc(cap, sizeof(double)), * xr = calloc(cap, sizeof(double));
  float in[2 * BLOCK], out[2 * BLOCK];
  long long got = 0, k = 0;                           /* inputs read; the next output */
  size_t n;
  while ((n = fread(in, 2 * sizeof(float), BLOCK, stdin)) > 0) {
    for (size_t i = 0; i < n; i++) { xl[(got + i) & (cap - 1)] = in[2 * i]; xr[(got + i) & (cap - 1)] = in[2 * i + 1]; }
    got += n;
    long nout = 0;
    for (;;) {
      long long u = k * M, base = u / L;              /* output k is at u in the upsampled stream */
      if (base >= got) break;
      const double* c = q + (u % L) * T;
      double l = 0, r = 0;
      for (long j = 0; j < T && base - j >= 0; j++) {
        long at = (base - j) & (cap - 1);
        l += c[j] * xl[at];
        r += c[j] * xr[at];
      }
      out[2 * nout] = (float)l; out[2 * nout + 1] = (float)r;
      if (++nout == BLOCK) { fwrite(out, 2 * sizeof(float), nout, stdout); nout = 0; }
      k++;
    }
    if (nout) fwrite(out, 2 * sizeof(float), nout, stdout);
  }
  return 0;
}
