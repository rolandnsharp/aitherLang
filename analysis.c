/* Spectral features (analysis.h). A radix-2 Cooley-Tukey FFT with no
 * dependencies, a Hann window, and normalised autocorrelation for the
 * fundamental. */

#include <math.h>
#include <stdlib.h>
#include "analysis.h"

#define PI 3.141592653589793

static double db_of(double linear) { return linear < 1e-12 ? -240.0 : 20.0 * log10(linear); }

static long next_pow2(long n) {
  long r = 1;
  while (r < n) r *= 2;
  return r;
}

/* Iterative radix-2 FFT on parallel (re, im) arrays, in place: bit-reverse
 * permute, then log2(n) butterfly passes. n is a power of two. */
static void fft_in_place(double* re, double* im, long n) {
  if (n <= 1) return;
  long j = 0;
  for (long i = 1; i < n - 1; i++) {
    long bit = n >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j |= bit;
    if (i < j) {
      double t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (long size = 2; size <= n; size <<= 1) {
    long half = size >> 1;
    double theta = -2.0 * PI / (double)size;
    double w_re_step = cos(theta), w_im_step = sin(theta);
    for (long k = 0; k < n; k += size) {
      double w_re = 1.0, w_im = 0.0;
      for (long jj = 0; jj < half; jj++) {
        long ti = k + jj + half, bi = k + jj;
        double t_re = w_re * re[ti] - w_im * im[ti];
        double t_im = w_re * im[ti] + w_im * re[ti];
        re[ti] = re[bi] - t_re;
        im[ti] = im[bi] - t_im;
        re[bi] = re[bi] + t_re;
        im[bi] = im[bi] + t_im;
        double nw_re = w_re * w_re_step - w_im * w_im_step;
        double nw_im = w_re * w_im_step + w_im * w_re_step;
        w_re = nw_re;
        w_im = nw_im;
      }
    }
  }
}

/* The Hann-windowed magnitude spectrum: next_pow2(n)/2 + 1 bins. */
static double* fft_mag(const double* x, long n, long* out_len) {
  *out_len = 0;
  if (n == 0) return NULL;
  long n_fft = next_pow2(n);
  double* re = calloc(n_fft, sizeof *re);
  double* im = calloc(n_fft, sizeof *im);
  if (n == 1) re[0] = x[0];
  else {
    double denom = (double)(n - 1);
    for (long i = 0; i < n; i++) re[i] = x[i] * (0.5 * (1.0 - cos(2.0 * PI * (double)i / denom)));
  }
  fft_in_place(re, im, n_fft);
  long len = n_fft / 2 + 1;
  double* mag = malloc(len * sizeof *mag);
  for (long k = 0; k < len; k++) mag[k] = sqrt(re[k] * re[k] + im[k] * im[k]);
  free(re);
  free(im);
  *out_len = len;
  return mag;
}

static double centroid_hz(const double* mag, long len, double sr) {
  /* The magnitude-weighted mean frequency. */
  if (len < 2) return 0.0;
  long n_fft = (len - 1) * 2;
  double num = 0.0, den = 0.0;
  for (long k = 0; k < len; k++) {
    double freq = (double)k * sr / (double)n_fft;
    num += freq * mag[k];
    den += mag[k];
  }
  return den < 1e-12 ? 0.0 : num / den;
}

typedef struct { long bin; double mag; } Cand;

static int cand_cmp(const void* a, const void* b) {
  /* Loudest first; equal ones keep their order (a stable sort). */
  const Cand* x = a; const Cand* y = b;
  if (x->mag > y->mag) return -1;
  if (x->mag < y->mag) return 1;
  return x->bin < y->bin ? -1 : x->bin > y->bin;
}

static int top_peaks(const double* mag, long len, double sr, int n, SpectralPeak* out) {
  /* The top n local maxima above a -60 dB floor, none within ±3 bins of
   * one already found, each placed between bins by a parabola. */
  if (len < 3 || n <= 0) return 0;
  long n_fft = (len - 1) * 2;
  double max_mag = 0.0;
  for (long k = 0; k < len; k++)
    if (mag[k] > max_mag) max_mag = mag[k];
  if (max_mag < 1e-9) return 0;
  double floor_mag = max_mag * pow(10.0, -60.0 / 20.0);
  Cand* c = malloc(len * sizeof *c);
  long nc = 0;
  for (long k = 1; k < len - 1; k++)
    if (mag[k] >= mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] >= floor_mag)
      c[nc++] = (Cand){k, mag[k]};
  qsort(c, nc, sizeof *c, cand_cmp);
  int found = 0;
  for (long i = 0; i < nc && found < n; i++) {
    int clash = 0;
    for (int p = 0; p < found && !clash; p++) {
      long p_bin = (long)(out[p].freq_hz * (double)n_fft / sr + 0.5);
      if (labs(c[i].bin - p_bin) < 3) clash = 1;
    }
    if (clash) continue;
    double y0 = mag[c[i].bin - 1], y1 = mag[c[i].bin], y2 = mag[c[i].bin + 1];
    double denom = y0 - 2.0 * y1 + y2;
    double offset = fabs(denom) < 1e-12 ? 0.0 : 0.5 * (y0 - y2) / denom;
    double interp = (double)c[i].bin + offset;
    out[found++] = (SpectralPeak){interp * sr / (double)n_fft, db_of(y1 / max_mag)};
  }
  free(c);
  return found;
}

static double rms_db(const double* x, long n) {
  if (n == 0) return -240.0;
  double sum_sq = 0.0;
  for (long i = 0; i < n; i++) sum_sq += x[i] * x[i];
  return db_of(sqrt(sum_sq / (double)n));
}

static double peak_db(const double* x, long n) {
  if (n == 0) return -240.0;
  double p = 0.0;
  for (long i = 0; i < n; i++)
    if (fabs(x[i]) > p) p = fabs(x[i]);
  return db_of(p);
}

static double zero_crossing_rate(const double* x, long n, double sr) {
  if (n < 2) return 0.0;
  double prev = x[0];
  long crossings = 0;
  for (long i = 1; i < n; i++) {
    double cur = x[i];
    if ((prev <= 0.0 && cur > 0.0) || (prev >= 0.0 && cur < 0.0)) crossings++;
    prev = cur;
  }
  return (double)crossings * sr / (double)n;
}

static double autocorr(const double* x, long n, long lag) {
  long n_lag = n - lag;
  if (n_lag <= 0) return 0.0;
  double s = 0.0;
  for (long i = 0; i < n_lag; i++) s += x[i] * x[i + lag];
  return s / (double)n_lag;
}

static double estimate_fundamental(const double* x, long n, double sr) {
  /* Normalised autocorrelation over lags for 40 Hz to 4 kHz: the first
   * lag that is a local maximum and reaches 0.4 of the zero-lag energy
   * (the period itself; later peaks are its multiples). The local-max
   * test keeps broadband content, which always correlates with itself
   * one sample on, from winning at the smallest lag. 0 when no lag
   * stands out: drums and noise shouldn't masquerade as pitched. */
  if (n < 64) return 0.0;
  long min_lag = (long)(sr / 4000.0);
  if (min_lag < 2) min_lag = 2;
  long max_lag = (long)(sr / 40.0);
  if (n / 2 < max_lag) max_lag = n / 2;
  if (min_lag >= max_lag - 1) return 0.0;
  double r0 = autocorr(x, n, 0);
  if (r0 < 1e-12) return 0.0;
  double threshold = 0.4 * r0;
  double prev = autocorr(x, n, min_lag);
  double cur = autocorr(x, n, min_lag + 1);
  for (long lag = min_lag + 1; lag < max_lag; lag++) {
    double nxt = autocorr(x, n, lag + 1);
    if (cur > prev && cur > nxt && cur >= threshold) {
      double denom = prev - 2.0 * cur + nxt;
      double offset = fabs(denom) < 1e-12 ? 0.0 : 0.5 * (prev - nxt) / denom;
      return sr / ((double)lag + offset);
    }
    prev = cur;
    cur = nxt;
  }
  return 0.0;
}

void analyze(const double* x, long n, double sr, int npeaks, SpectrumSummary* out) {
  if (npeaks > ANALYSIS_MAX_PEAKS) npeaks = ANALYSIS_MAX_PEAKS;
  long len;
  double* mag = fft_mag(x, n, &len);
  out->sr = sr;
  out->npeaks = top_peaks(mag, len, sr, npeaks, out->peaks);
  out->centroid_hz = centroid_hz(mag, len, sr);
  out->rms_db = rms_db(x, n);
  out->peak_db = peak_db(x, n);
  out->zero_crossing_rate = zero_crossing_rate(x, n, sr);
  out->fundamental_hz = estimate_fundamental(x, n, sr);
  free(mag);
}

double* mono_mix(const double* l, const double* r, long n) {
  double* m = malloc((n ? n : 1) * sizeof *m);
  for (long i = 0; i < n; i++) m[i] = (l[i] + r[i]) * 0.5;
  return m;
}

double analysis_rms_db(const double* x, long n) { return rms_db(x, n); }
double analysis_peak_db(const double* x, long n) { return peak_db(x, n); }
double analysis_zero_crossing_rate(const double* x, long n, double sr) { return zero_crossing_rate(x, n, sr); }
