/* Pure spectral feature tests for analysis.c. Inputs are synthetic
 * buffers (sine, two-tone, sum-of-sines saw, deterministic noise) so
 * the targets are math-exact and survive future FFT tweaks. No engine,
 * no patches, no I/O. */

#include <stdint.h>
#include "check.h"

#define SR 48000.0

static double* sine_buf(double freq, long n, double sr, double amp) {
  double* b = malloc(n * sizeof *b);
  for (long i = 0; i < n; i++) b[i] = amp * sin(2.0 * M_PI * freq * (double)i / sr);
  return b;
}

/* Deterministic [-1, 1] white noise via xorshift64. Same seed each run,
 * so the test is reproducible. */
static double* lcg_noise(long n) {
  double* b = malloc(n * sizeof *b);
  uint64_t s = 0x1234567ABCDEFull;
  for (long i = 0; i < n; i++) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    double x = (double)(s & 0xFFFFFFFFull) / 4294967295.0;
    b[i] = x * 2.0 - 1.0;
  }
  return b;
}

TEST(analysis) {
  SpectrumSummary s;

  /* 1. Pure 440 Hz sine: top peak, centroid, fundamental all match. */
  {
    double* buf = sine_buf(440.0, 48000, SR, 1.0);
    analyze(buf, 48000, SR, 3, &s);
    CHECK(s.npeaks > 0, "should detect at least one peak");
    if (s.npeaks > 0)
      CHECK(fabs(s.peaks[0].freq_hz - 440.0) < 1.0, "top peak should be ~440 Hz, got %g",
            s.peaks[0].freq_hz);
    CHECK(fabs(s.centroid_hz - 440.0) < 5.0, "centroid should be ~440 Hz, got %g", s.centroid_hz);
    CHECK(fabs(s.fundamental_hz - 440.0) < 1.0, "fundamental should be ~440 Hz, got %g",
          s.fundamental_hz);
    /* Sine RMS = amp/sqrt(2) = 0.707, about -3 dB. */
    CHECK(fabs(s.rms_db - (-3.0)) < 1.0, "RMS dB ≈ -3 for unit sine, got %g", s.rms_db);
    free(buf);
  }

  /* 2. White noise: no clear pitch, centroid roughly sr/4. */
  {
    double* buf = lcg_noise(48000);
    analyze(buf, 48000, SR, 8, &s);
    CHECK(s.fundamental_hz == 0.0, "noise has no fundamental, got %g", s.fundamental_hz);
    CHECK(fabs(s.centroid_hz - SR / 4.0) < SR / 8.0, "noise centroid should be roughly sr/4, got %g",
          s.centroid_hz);
    free(buf);
  }

  /* 3. Two-tone (440 + 880, equal amp): top two peaks at those freqs,
   * second peak within ~1 dB of the first. */
  {
    long n = 48000;
    double* buf = sine_buf(440.0, n, SR, 0.5);
    double* buf2 = sine_buf(880.0, n, SR, 0.5);
    for (long i = 0; i < n; i++) buf[i] += buf2[i];
    analyze(buf, n, SR, 4, &s);
    CHECK(s.npeaks >= 2, "should detect both tones");
    if (s.npeaks >= 2) {
      double f1 = s.peaks[0].freq_hz, f2 = s.peaks[1].freq_hz;
      double lo = fmin(f1, f2), hi = fmax(f1, f2);
      CHECK(fabs(lo - 440.0) < 2.0, "low peak should be ~440, got %g", lo);
      CHECK(fabs(hi - 880.0) < 2.0, "high peak should be ~880, got %g", hi);
      CHECK(fabs(s.peaks[0].mag_db - s.peaks[1].mag_db) < 1.0,
            "equal-amp tones should have equal-mag peaks: %g vs %g", s.peaks[0].mag_db,
            s.peaks[1].mag_db);
    }
    free(buf);
    free(buf2);
  }

  /* 4. Hand-built 1/n saw at 110 Hz: 3 strongest harmonics at
   * 110/220/330 with magnitude falloff matching 1/n (within 2 dB). */
  {
    long n = 48000;
    double* buf = calloc(n, sizeof *buf);
    for (int h = 1; h <= 8; h++) {
      double amp = 1.0 / (double)h * 0.5;      /* leave headroom */
      double f = 110.0 * (double)h;
      for (long i = 0; i < n; i++) buf[i] += amp * sin(2.0 * M_PI * f * (double)i / SR);
    }
    analyze(buf, n, SR, 3, &s);
    CHECK(s.npeaks >= 3, "should detect at least 3 harmonics");
    if (s.npeaks > 0)
      CHECK(fabs(s.peaks[0].freq_hz - 110.0) < 2.0, "fundamental peak ~110, got %g",
            s.peaks[0].freq_hz);
    /* Peak 2 should be near 220 (some adjacency, but the fundamental
     * dominates), peak 3 near 330. */
    int saw220 = 0, saw330 = 0;
    for (int k = 0; k < s.npeaks; k++) {
      if (fabs(s.peaks[k].freq_hz - 220.0) < 3.0) saw220 = 1;
      if (fabs(s.peaks[k].freq_hz - 330.0) < 3.0) saw330 = 1;
    }
    CHECK(saw220, "should find a peak near 220 Hz");
    CHECK(saw330, "should find a peak near 330 Hz");
    /* Magnitude falloff: 220 should be ~6 dB below 110 (1/2 = -6 dB).
     * Allow ±3 dB slack for FFT bin spread. */
    for (int k = 0; k < s.npeaks; k++)
      if (fabs(s.peaks[k].freq_hz - 220.0) < 3.0)
        CHECK(fabs(s.peaks[k].mag_db - (-6.0)) < 3.0,
              "220 Hz peak should be ~-6 dB rel to fundamental, got %g", s.peaks[k].mag_db);
    free(buf);
  }

  /* 5. ZCR sanity: 440 Hz sine, ~880 zero crossings/sec. */
  {
    double* buf = sine_buf(440.0, 48000, SR, 1.0);
    double z = analysis_zero_crossing_rate(buf, 48000, SR);
    CHECK(fabs(z - 880.0) < 5.0, "440 Hz sine ZCR ≈ 880/sec, got %g", z);
    free(buf);
  }

  /* 6. peak_db / rms_db for a known scaled sine. */
  {
    double* buf = sine_buf(440.0, 48000, SR, 0.1);
    double p = analysis_peak_db(buf, 48000), r = analysis_rms_db(buf, 48000);
    CHECK(fabs(p - (-20.0)) < 0.5, "0.1-amp sine peak should be ~-20 dB, got %g", p);
    CHECK(fabs(r - (-23.0)) < 0.5, "0.1-amp sine RMS should be ~-23 dB, got %g", r);
    free(buf);
  }
}
