/* dho(force, freq, damp): universal second-order oscillator, its phasor
 * [x, v/w]. Three invariants pin the math: (1) struck DHO rings at the
 * requested frequency and decays at the predicted timescale, (2) under
 * a constant force at critical damping, x approaches force/k, (3)
 * critical damping with a step input rises monotonically with no
 * overshoot. */

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

TEST(dho) {
  /* 1. Impulse response: pitch + decay envelope. A struck DHO at 440 Hz
   * with damp=0.01 should ring near 440 Hz and its envelope should decay
   * with time constant tau = 1/(damp*omega) ≈ 36 ms. Use a one-shot
   * strike at t=0 (`if t < dt`). Force is acceleration: peak amplitude
   * ≈ force/(sr*omega), so a 440 Hz impulse needs force on the order of
   * 1e8 to ring at unit-ish amplitude. */
  {
    const double freq = 440.0, damp = 0.01;
    double omega = 2.0 * M_PI * freq;
    double tau_expected = 1.0 / (damp * omega);   /* ≈ 0.0362 s */
    long n = (long)(0.4 * SR);
    double* buf = render_mono("dho(if t < dt then 100000000 else 0, 440.0, 0.01)[0]", n);

    /* Sanity: there must actually be signal to autocorrelate. */
    double sig_energy = 0.0;
    for (long i = 0; i < n; i++) sig_energy += buf[i] * buf[i];
    CHECK(sig_energy > 0.0, "dho impulse produced no signal");

    /* Pitch via autocorrelation in a settled window (skip first 5 ms of
     * the strike transient, leave 300 samples of slack at the tail). */
    long start = (long)(0.005 * SR);
    long seg_len = n - start - 300;
    int ideal_lag = (int)(SR / freq);             /* 109 samples */
    int best_lag = 0;
    double best_corr = -1e9;
    for (int lag = ideal_lag - 6; lag <= ideal_lag + 6; lag++) {
      double c = 0.0;
      for (long i = 0; i < seg_len; i++) c += buf[start + i] * buf[start + i + lag];
      if (c > best_corr) {
        best_corr = c;
        best_lag = lag;
      }
    }
    CHECK(abs(best_lag - ideal_lag) <= 3, "dho impulse: autocorr peak at lag %d, expected %d",
          best_lag, ideal_lag);
    CHECK(best_corr > 0.0, "dho impulse: autocorr non-positive, not periodic");

    /* Decay envelope. Compare RMS in two windows (the late one ~2 tau
     * later than the early one). The ratio should match exp(-2) ≈ 0.135
     * within a generous band: semi-implicit Euler at omega*dt ≈ 0.058
     * has small but measurable damping/frequency shift. */
    long win_len = (long)(0.005 * SR);            /* 5 ms windows */
    long early_lo = (long)(0.010 * SR);
    long late_lo = (long)(0.010 * SR + 2.0 * tau_expected * SR);
    double early = rms(buf, early_lo, early_lo + win_len);
    double late = rms(buf, late_lo, late_lo + win_len);
    CHECK(early > 1e-4, "dho impulse: no audible ringdown");
    double ratio = late / early, expected = exp(-2.0);
    CHECK(ratio > expected * 0.5 && ratio < expected * 1.6,
          "dho impulse: decay ratio %g not near exp(-2) ≈ 0.135 (early=%g late=%g)", ratio, early,
          late);
    free(buf);
  }

  /* 2. Steady-state under constant force at critical damping equals
   * force/k where k = omega^2. Critical damping means x rises smoothly
   * to the equilibrium without oscillation; after several time
   * constants the integrator should sit within a few percent of
   * force/k. Settling time at critical damping: well within 1 second
   * for f=10 Hz. */
  const double force = 100.0, freq = 10.0;
  double omega = 2.0 * M_PI * freq;
  double x_expected = force / (omega * omega);    /* ≈ 0.02533 */
  {
    long n = (long)(1.0 * SR);
    double* buf = render_mono("dho(100.0, 10.0, 1.0)[0]", n);
    double final = buf[n - 1];
    CHECK(fabs(final - x_expected) < x_expected * 0.05, "dho steady-state: got %g, expected %g",
          final, x_expected);
    free(buf);
  }

  /* 3. Critical damping under a step input is monotonically rising (no
   * overshoot). Sample-by-sample non-decreasing within a tiny epsilon
   * (numerical noise, semi-implicit integration). */
  {
    long n = (long)(0.5 * SR);
    double* buf = render_mono("dho(100.0, 10.0, 1.0)[0]", n);
    double prev = buf[0], max_val = buf[0], min_delta = 0.0;
    for (long i = 0; i < n; i++) {
      if (buf[i] > max_val) max_val = buf[i];
      double d = buf[i] - prev;
      if (d < min_delta) min_delta = d;
      prev = buf[i];
    }
    CHECK(max_val <= x_expected * 1.001, "dho critical: overshoot, peak %g > steady-state %g",
          max_val, x_expected);
    CHECK(min_delta > -1e-9, "dho critical: non-monotonic step (worst dip = %g)", min_delta);
    free(buf);
  }

  /* 4. dho returns its phasor [x, v / w]: the velocity in the units of
   * the position, so a lightly damped ring traces a circle and both
   * sides peak alike. */
  {
    long n = (long)(0.1 * SR);
    double* x = render_mono("dho(if t < dt then 100000000 else 0, 440, 0.01)[0]", n);
    double* v = render_mono("dho(if t < dt then 100000000 else 0, 440, 0.01)[1]", n);
    double mx = 0.0, mv = 0.0;
    for (long i = 0; i < n; i++) {
      mx = fmax(mx, fabs(x[i]));
      mv = fmax(mv, fabs(v[i]));
    }
    CHECK(mx > 0.01 && fabs(mv - mx) / mx < 0.05, "dho phasor: |x| peaks at %g, |v/w| at %g", mx,
          mv);
    free(x);
    free(v);
  }
}
