/* Physical-instrument library: pluck_string, bowed_string, struck_bar,
 * tuning_fork. Each pins a property that justifies the physics
 * paradigm: additive can't fake these without elaborate envelope
 * tricks that miss the character.
 *
 * Invariants:
 *   1. All four compile and tick NaN-free for 1 second @ 220 Hz.
 *   2. pluck_string is actually pitched at the requested freq:
 *      autocorrelation peak lands near lag = sr / freq.
 *   3. struck_bar's mode-dependent damping is real: amplitude shortly
 *      after the strike is much higher than at 1.5 s.
 *   4. bowed_string's continuous excitation produces sustained output:
 *      RMS after a 0.5 s settle is non-trivial. */

#include "check.h"

#define SR 48000.0

/* The left side of `n` samples, NaN failing the test. */
static double* render_mono(const char* src, long n) {
  Voice* v = must_load(src, SR);
  double* b = calloc(n, sizeof *b);
  if (!v) return b;
  for (long i = 0; i < n; i++) {
    double l, r;
    tick_at(v, (double)i / SR, &l, &r);
    if (l != l) {
      CHECK(0, "NaN at sample %ld in:\n%s", i, src);
      break;
    }
    b[i] = l;
  }
  return b;
}

static double rms(const double* b, long lo, long hi) {
  double e = 0.0;
  for (long i = lo; i < hi; i++) e += b[i] * b[i];
  return sqrt(e / (double)(hi - lo));
}

/* The lag within ±width of ideal where the window from `start`
 * correlates best with itself, and that correlation. */
static int best_lag(const double* buf, long n, long start, int ideal, int width, double* corr) {
  long seg_len = n - start - 300;
  int best = 0;
  *corr = -1e9;
  for (int lag = ideal - width; lag <= ideal + width; lag++) {
    double c = 0.0;
    for (long i = 0; i < seg_len; i++) c += buf[start + i] * buf[start + i + lag];
    if (c > *corr) {
      *corr = c;
      best = lag;
    }
  }
  return best;
}

TEST(physical_instruments) {
  /* 1. NaN-free for 1 second on a strike-at-fundamental excitation.
   * Smoke test that every def survives an audio-rate run with the kind
   * of input it's documented to accept. */
  {
    const char* srcs[] = {
      "tuning_fork(impulse(2) * 50, 220) * 0.05",
      "pluck_string(noise() * impulse(2) * 0.5, 220, 0.7)",
      "bowed_string(noise() * 0.3, 220) * 0.1",
      "struck_bar(impulse(2) * 30, 220) * 0.02",
    };
    for (int k = 0; k < 4; k++) {
      double* buf = render_mono(srcs[k], 48000);
      double energy = 0.0;
      for (long i = 0; i < 48000; i++) energy += buf[i] * buf[i];
      CHECK(energy > 0.0, "no signal from: %s", srcs[k]);
      free(buf);
    }
  }

  /* 2. pluck_string is pitched at the requested freq. Autocorrelation
   * over a settled window: the first prominent peak after lag 0 should
   * sit near `sr / freq`. Karplus-Strong's exact pitch drifts slightly
   * from the ideal because of LP-filter group delay and the one-sample
   * loop offset, so the test allows a ±20-sample window around the
   * ideal lag (about ±10% of the period). */
  {
    const double freq = 220.0;
    long n = (long)(1.0 * SR);
    double* buf = render_mono("pluck_string(noise() * impulse(2) * 0.5, 220.0, 0.7)", n);
    /* Skip the first 0.4 s: the initial noise burst still dominates
     * before the LP-feedback loop has carved out the periodic shape. */
    int ideal = (int)(SR / freq);                 /* 218 samples */
    double corr;
    int lag = best_lag(buf, n, (long)(0.4 * SR), ideal, 20, &corr);
    CHECK(abs(lag - ideal) <= 18,
          "pluck_string at %g Hz: autocorrelation peak at lag %d, expected near %d", freq, lag,
          ideal);
    /* And the autocorrelation at the best lag should be clearly
     * positive (a periodic signal correlates with itself at the period). */
    CHECK(corr > 0.0,
          "pluck_string: autocorr at expected lag is non-positive (%g), string is not periodic",
          corr);
    free(buf);
  }

  /* 3. struck_bar: bright transient mellows to fundamental ring. Strike
   * at sample 0 via a `t < dt` pulse (one-time impulse, no period
   * alignment to worry about). Compare RMS in an early window (0..150
   * ms, all modes ringing) against a late window centred on 1.5 s. The
   * late window should be markedly quieter: the high modes have died
   * and only the lightly-damped fundamental remains. */
  {
    double* buf = render_mono("struck_bar(if t < dt then 50 else 0, 220) * 0.05",
                              (long)(1.7 * SR));
    double early = rms(buf, 0, (long)(0.15 * SR));
    double late = rms(buf, (long)(1.45 * SR), (long)(1.55 * SR));
    CHECK(early > 0.0, "struck_bar produced no early signal");
    CHECK(early > late * 3.0,
          "struck_bar: mode-dependent decay should make early window much louder than late; "
          "earlyRms=%g lateRms=%g", early, late);
    free(buf);
  }

  /* 4. bowed_string sustains under continuous excitation. RMS in a
   * window 0.5..1.0 s should be comparable to a window 0.0..0.5 s
   * (steady-state, not a decaying transient). Pin both: non-zero and
   * within a 3x band of each other. */
  {
    double* buf = render_mono("bowed_string(noise() * 0.3, 220) * 0.1", (long)(1.0 * SR));
    double early = rms(buf, 0, (long)(0.5 * SR));
    double late = rms(buf, (long)(0.5 * SR), (long)(1.0 * SR));
    CHECK(late > 0.001, "bowed_string: late window silent (%g), no sustain", late);
    CHECK(late > 0.3 * early,
          "bowed_string: late RMS (%g) collapsed vs early (%g), should be steady-state under "
          "continuous bow", late, early);
    CHECK(late < 3.0 * early,
          "bowed_string: late RMS (%g) way bigger than early (%g), system is blowing up, not "
          "steady", late, early);
    free(buf);
  }

  /* 5. tuning_fork's pitch is exact. Single-mode HO has no detune from
   * filter delays etc., so it is easier to pin tightly. */
  {
    const double freq = 220.0;
    long n = (long)(0.4 * SR);
    double* buf = render_mono("tuning_fork(if t < dt then 50 else 0, 220.0) * 0.1", n);
    int ideal = (int)(SR / freq);
    double corr;
    int lag = best_lag(buf, n, (long)(0.05 * SR), ideal, 10, &corr);   /* past the initial jolt */
    CHECK(abs(lag - ideal) <= 3, "tuning_fork at %g Hz: autocorr peak at lag %d, expected %d", freq,
          lag, ideal);
    free(buf);
  }
}
