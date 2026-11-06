/* What tools/fft and tools/ifft share: the frame and a complex FFT.
 *
 * A frame is N samples, N a power of two, one every H = N / 2. The window
 * is a sine on the way in and again on the way out: its square added to
 * itself half a frame on is 1, so frames overlap back to the signal.
 * A frame's H bins go down the pipe as H pairs, one a sample, so the
 * stream keeps the signal's rate: pair k is bin k's [re, im], except
 * that bins 0 and H are real, and share pair 0 as [bin 0, bin H]. Bins
 * are scaled by 2 / N, so a steady sine's stays near its level. */
#ifndef AITHER_TOOLS_FFT_H
#define AITHER_TOOLS_FFT_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* The frame size from the command line, or the tool stops. */
static long frame_size(const char* tool, const char* arg) {
  char* end;
  long n = strtol(arg, &end, 10);
  if (*end || n < 4 || n > (1L << 24) || (n & (n - 1))) {
    fprintf(stderr, "%s: the frame size is a power of two from 4, not %s\n", tool, arg);
    exit(2);
  }
  return n;
}

static double* sine_window(long n) {
  double* w = malloc(n * sizeof *w);
  for (long i = 0; i < n; i++) w[i] = sin(M_PI * (i + 0.5) / n);
  return w;
}

/* The turns of an FFT of n: cos and sin of -2 pi k / n, for k < n / 2. */
static void fft_turns(long n, double** cs, double** sn) {
  *cs = malloc(n / 2 * sizeof **cs);
  *sn = malloc(n / 2 * sizeof **sn);
  for (long k = 0; k < n / 2; k++) {
    (*cs)[k] = cos(-2 * M_PI * k / n);
    (*sn)[k] = sin(-2 * M_PI * k / n);
  }
}

/* In place, unscaled; sign 1 forward, -1 back. */
static void fft(double* re, double* im, long n, const double* cs, const double* sn, int sign) {
  for (long i = 1, j = 0; i < n; i++) {
    long bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      double t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (long size = 2; size <= n; size <<= 1) {
    long half = size >> 1, step = n / size;
    for (long b = 0; b < n; b += size)
      for (long k = 0; k < half; k++) {
        double c = cs[k * step], s = sign * sn[k * step];
        long lo = b + k, hi = lo + half;
        double tr = re[hi] * c - im[hi] * s, ti = re[hi] * s + im[hi] * c;
        re[hi] = re[lo] - tr; im[hi] = im[lo] - ti;
        re[lo] += tr; im[lo] += ti;
      }
  }
}

#endif
