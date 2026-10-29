/* feat: features of a raw float32 stereo render at rate R, for comparing renders of one patch at different rates.
   Prints: rms_dB peak_Hz then 25 band levels (third octaves 50 Hz..12.5 kHz) relative to their mean, dB. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static void fft(double *re, double *im, int n) {
  for (int i = 1, j = 0; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b;
    if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
  for (int len = 2; len <= n; len <<= 1) { double a = -2 * M_PI / len, wr = cos(a), wi = sin(a);
    for (int i = 0; i < n; i += len) { double cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) { int u = i + k, v = u + len / 2;
        double tr = re[v] * cr - im[v] * ci, ti = re[v] * ci + im[v] * cr;
        re[v] = re[u] - tr; im[v] = im[u] - ti; re[u] += tr; im[u] += ti;
        double nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr; } } }
}
int main(int argc, char **argv) {
  double R = atof(argv[2]);
  FILE *f = fopen(argv[1], "rb"); if (!f) return 1; fseek(f, 0, 2); long n = ftell(f) / 8; rewind(f);
  float *x = malloc(n * 8); if (fread(x, 8, n, f) != (size_t)n) return 1; fclose(f);
  long skip = (long)(0.5 * R); if (n <= skip) return 1;
  double *m = malloc(n * sizeof(double)); double e = 0;
  for (long i = 0; i < n; i++) { m[i] = 0.5 * (x[2*i] + x[2*i+1]); if (i >= skip) e += m[i] * m[i]; }
  int N = 1; while (N < (long)(1.5 * R)) N <<= 1;          /* ~1.5 s frames: about 0.7 Hz bins at any rate */
  double *re = malloc(N * 8), *im = malloc(N * 8), *P = calloc(N / 2, 8); int frames = 0;
  for (long s = skip; s + N <= n; s += N / 2, frames++) {
    for (int i = 0; i < N; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / N); re[i] = m[s + i] * w; im[i] = 0; }
    fft(re, im, N); for (int i = 0; i < N / 2; i++) P[i] += re[i] * re[i] + im[i] * im[i];
  }
  if (!frames) return 1;
  double df = R / N; int best = 0;
  for (int i = (int)(40 / df); i < (int)(16000 / df); i++) if (P[i] > P[best]) best = i;
  double pk = best * df;
  if (best > 0) { double a = log(P[best-1] + 1e-300), b = log(P[best] + 1e-300), c = log(P[best+1] + 1e-300); pk = (best + 0.5 * (a - c) / (a - 2 * b + c)) * df; }
  printf("%.2f %.3f", 10 * log10(e / (n - skip) + 1e-30), pk);
  double L[25], mean = 0;
  for (int b = 0; b < 25; b++) { double fc = 50 * pow(2, b / 3.0), lo = fc / pow(2, 1 / 6.0), hi = fc * pow(2, 1 / 6.0), s = 0;
    for (int i = (int)(lo / df); i < (int)(hi / df); i++) s += P[i];
    L[b] = 10 * log10(s / frames / (double)N + 1e-30); mean += L[b] / 25; }
  for (int b = 0; b < 25; b++) printf(" %.2f", L[b] - mean);
  printf("\n");
}
