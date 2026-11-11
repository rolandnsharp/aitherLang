/* Native DSP primitives (dsp.h). The arithmetic is written so that every
 * build computes the same bits: no fused multiply-adds (the pragma, and
 * -ffp-contract=off where this is compiled), and min, max, clamp and
 * floor_mod exactly as the Nim they replace defined them, NaN included,
 * so every patch still renders sample for sample as before. */

#pragma GCC optimize("fp-contract=off")

#include <math.h>
#include "dsp.h"

#define PI_ 3.141592653589793
#define TAU_ 6.283185307179586

static inline double max_(double x, double y) { return (y <= x || y != y) ? x : y; }
static inline double min_(double x, double y) { return (x <= y || y != y) ? x : y; }
static inline long maxl(long x, long y) { return y <= x ? x : y; }
static inline long minl(long x, long y) { return x <= y ? x : y; }
static inline double clamp_(double x, double a, double b) {
  if (x < a) return a;
  if (x > b) return b;
  return x;
}
/* Python's %: the sign of the divisor. */
static inline double floor_mod(double x, double y) {
  double r = fmod(x, y);
  if ((r > 0 && y < 0) || (r < 0 && y > 0)) r += y;
  return r;
}

/* Claim n slots at `site`, the call site's region (its offset, passed by the
 * generated code, so no cursor is shared between parts running on
 * different cores), or the shared fallback region at the
 * pool's tail when the patch asks for more than it has (garbage state,
 * not a crash). */
static inline long claim(DspState* s, long site, long n) {
  if (site + n > s->pool_slots - POOL_OVERFLOW_SLOTS) {
    s->overflow = 1;
    return s->pool_slots - POOL_OVERFLOW_SLOTS;
  }
  return site;
}

/* ---------------------------------------------------------------- shapes */

double shape_saw(double x) { return fmod(x / TAU_, 1.0) * 2.0 - 1.0; }
double shape_tri(double x) { return fabs(fmod(x / TAU_, 1.0) * 4.0 - 2.0) - 1.0; }
double shape_sqr(double x) { return fmod(x / TAU_, 1.0) < 0.5 ? 1.0 : -1.0; }

/* ----------------------------------------------------------- oscillators */

double n_wave(DspState* s, long site, double freq, double* values, int n) {
  if (n <= 0) return 0.0;
  long i = claim(s, site, 1);
  s->pool[i] = fmod(s->pool[i] + freq / s->sr, 1.0);
  if (s->pool[i] < 0.0) s->pool[i] += 1.0;
  long idx = (long)(s->pool[i] * (double)n) % (long)n;
  return values[idx];
}

/* -------------------------------------------------------- 1-pole filters
 * The cutoff is in Hz: a = 1 - exp(-2 pi fc / sr) puts the -3 dB point at
 * fc (within 3% up to 4 kHz at 48 kHz).
 *
 * Each stateful native with a coefficient comes in two halves: `coef`
 * (pure, from the parameters and the rate) and the `_c` step that takes
 * it ready-made. When a patch's parameters can't change from sample to
 * sample, codegen computes the coefficient once (hoist_init) and calls
 * the `_c` step; otherwise the plain native does both, by calling the
 * same two, so the two paths give the same bits. */

static inline double one_pole_coef(double cutoff, double sr) {
  return 1.0 - exp(-TAU_ * max_(cutoff, 0.0) / sr);
}

double n_lp1_coef(double cutoff, double sr) { return one_pole_coef(cutoff, sr); }

double n_lp1_c(DspState* s, long site, double sig, double a) {
  long i = claim(s, site, 1);
  s->pool[i] += a * (sig - s->pool[i]);
  return s->pool[i];
}

double n_hp1_c(DspState* s, long site, double sig, double a) {
  long i = claim(s, site, 1);
  s->pool[i] += a * (sig - s->pool[i]);
  return sig - s->pool[i];
}

/* A turning point's step (codegen's turners): the point at p turned by
 * (cs, sn), then brought back to the unit circle by one Newton step.
 * Compiled with the engine, so a patch on TCC turns at -O2 speed; the
 * -O2 build inlines a copy from codegen.c's prelude, kept identical, so
 * the two give the same bits. */
static inline void turn_step(double* p, double cs, double sn) {
  double r = p[0] * cs - p[1] * sn;
  double i = p[0] * sn + p[1] * cs;
  double g = 1.5 - 0.5 * (r * r + i * i);
  p[0] = r * g; p[1] = i * g;
}

/* A steady turner: it starts at (1, 0); p[2] marks it started. */
void n_turn(double* p, double cs, double sn) {
  if (p[2] == 0.0) { p[0] = 1.0; p[1] = 0.0; p[2] = 1.0; }
  turn_step(p, cs, sn);
}

/* A clock turner between its settings: p[2] counts the samples left. */
void n_turn_clock(double* p, double cs, double sn) {
  turn_step(p, cs, sn);
  p[2] = p[2] - 1.0;
}

double n_lp1(DspState* s, long site, double sig, double cut) { return n_lp1_c(s, site, sig, one_pole_coef(cut, s->sr)); }
double n_hp1(DspState* s, long site, double sig, double cut) { return n_hp1_c(s, site, sig, one_pole_coef(cut, s->sr)); }

/* ---------------------------------------------- SVF (Cytomic trapezoidal) */

enum { LOW, HIGH, BAND, NOTCH };

/* k, a1, a2, a3 from the cutoff, resonance and rate. The cutoff is
 * clamped to (1 Hz, just under Nyquist): a negative or zero one would
 * push tan() into the wrong quadrant and the NaN would stay in the
 * filter's slots for good. */
static inline void svf_coef(double cutoff_arg, double res, double sr, double* o) {
  double cutoff = clamp_(cutoff_arg, 1.0, sr * 0.499);
  double g = tan(PI_ * cutoff / sr);
  double k = 2.0 * (1.0 - res);
  double a1 = 1.0 / (1.0 + g * (g + k));
  double a2 = g * a1;
  double a3 = g * a2;
  o[0] = k; o[1] = a1; o[2] = a2; o[3] = a3;
}

void n_svf_coef(double cutoff, double res, double sr, double* o) { svf_coef(cutoff, res, sr, o); }

static inline double svf_c(DspState* s, long site, double sig, const double* c, int mode) {
  long i = claim(s, site, 2);
  double v3 = sig - s->pool[i + 1];
  double v1 = c[1] * s->pool[i] + c[2] * v3;
  double v2 = s->pool[i + 1] + c[2] * s->pool[i] + c[3] * v3;
  s->pool[i] = 2.0 * v1 - s->pool[i];
  s->pool[i + 1] = 2.0 * v2 - s->pool[i + 1];
  switch (mode) {
    case LOW: return v2;
    case HIGH: return sig - c[0] * v1 - v2;
    case BAND: return v1;
    default: return sig - c[0] * v1;
  }
}

static inline double svf(DspState* s, long site, double sig, double cut, double res, int mode) {
  double c[4];
  svf_coef(cut, res, s->sr, c);
  return svf_c(s, site, sig, c, mode);
}

double n_lpf(DspState* s, long site, double sig, double cut, double res) { return svf(s, site, sig, cut, res, LOW); }
double n_hpf(DspState* s, long site, double sig, double cut, double res) { return svf(s, site, sig, cut, res, HIGH); }
double n_bpf(DspState* s, long site, double sig, double cut, double res) { return svf(s, site, sig, cut, res, BAND); }
double n_notch(DspState* s, long site, double sig, double cut, double res) { return svf(s, site, sig, cut, res, NOTCH); }

double n_lpf_c(DspState* s, long site, double sig, const double* c) { return svf_c(s, site, sig, c, LOW); }
double n_hpf_c(DspState* s, long site, double sig, const double* c) { return svf_c(s, site, sig, c, HIGH); }
double n_bpf_c(DspState* s, long site, double sig, const double* c) { return svf_c(s, site, sig, c, BAND); }
double n_notch_c(DspState* s, long site, double sig, const double* c) { return svf_c(s, site, sig, c, NOTCH); }

/* ------------------------------------------------------------------ delays
 * The buffer lives in the pool: [cursor, sample0, sample1, ...]. `time`
 * need not be a whole number of samples: the read point sits between two
 * taps and the two are mixed linearly, so a tube or string tunes to its
 * length, not to the sample grid. `delay` writes before it reads (a time
 * of 0 is this sample's input; the longest is one sample short of
 * max_time); `fbdelay` reads first (its feedback needs the last output)
 * and so starts at one sample. */

/* Sample `d` (0 .. len - 1) samples behind `cursor`. */
static inline double tap_interp(DspState* s, long base, long len, long cursor, double d) {
  long d0 = (long)d;
  double frac = d - (double)d0;
  long i0 = (cursor - d0 + len) % len;
  if (frac <= 0.0) return s->pool[base + 1 + i0];
  long i1 = (i0 - 1 + len) % len;          /* one sample older */
  return s->pool[base + 1 + i0] * (1.0 - frac) + s->pool[base + 1 + i1] * frac;
}

double n_delay(DspState* s, long site, double sig, double time, double max_time) {
  long len = maxl(1, (long)(max_time * s->sr));
  long base = claim(s, site, 1 + len);
  long cursor = (long)s->pool[base] % len;
  s->pool[base + 1 + cursor] = sig;
  double d = clamp_(time * s->sr, 0.0, (double)(len - 1));
  double out = tap_interp(s, base, len, cursor, d);
  s->pool[base] = (double)((cursor + 1) % len);
  return out;
}

double n_fbdelay(DspState* s, long site, double sig, double time, double max_time, double fb) {
  long len = maxl(1, (long)(max_time * s->sr));
  long base = claim(s, site, 1 + len);
  long cursor = (long)s->pool[base] % len;
  double d = clamp_(time * s->sr, min_(1.0, (double)(len - 1)), (double)(len - 1));
  double out = tap_interp(s, base, len, cursor, d);
  s->pool[base + 1 + cursor] = sig + out * fb;
  s->pool[base] = (double)((cursor + 1) % len);
  return out;
}

/* -------------------------------------------------------------------- tape
 * A piece of tape `max_time` long, with a write head and a read head whose
 * positions are signals, in seconds, wrapping round it. The pool holds the
 * samples and nothing else. It reads before it writes, as a deck's play
 * head sits behind its record head: a read at the write position hears
 * what was there one trip round ago, which a loop recording over itself
 * must play. The read is between samples (linear), so a slow head bends
 * the pitch; the write lands on the nearest sample, as `sig` plus what was
 * there times `keep` (the erase head). `delay` is a tape whose write head
 * stays at now. */

double n_tape(DspState* s, long site, double sig, double on, double w, double r, double max_time, double keep) {
  long n = maxl(1, (long)(max_time * s->sr));
  long base = claim(s, site, n);
  double at = floor_mod(r * s->sr, (double)n);
  /* A head on a sample reads that sample: `t * sr` lands a hair either
   * side of a whole number, and a loop (r = w) must not hear a millionth
   * of the sample just written beside it. */
  double near_ = floor(at + 0.5);
  if (fabs(at - near_) < 1e-6) at = floor_mod(near_, (double)n);
  long i = minl((long)at, n - 1);
  double frac = at - (double)i;
  double out = s->pool[base + i] * (1.0 - frac) + s->pool[base + (i + 1) % n] * frac;
  if (on > 0.0) {
    long k = (long)floor_mod(floor(w * s->sr + 0.5), (double)n);
    s->pool[base + k] = sig + s->pool[base + k] * keep;
  }
  return out;
}

/* ------------------------------------------------------------------ reverb
 * Schroeder: 4 parallel comb filters into 2 series allpass. The room is in
 * seconds, so it is the same room at any rate: Schroeder's lengths are
 * samples at 44.1 kHz, kept as those times, and the combs' damping is a
 * one-pole whose pole is 0.3 at 44.1 kHz, kept as its time constant. */

static const double comb_secs[4] = {1557 / 44100.0, 1617 / 44100.0, 1491 / 44100.0, 1422 / 44100.0};
static const double ap_secs[2] = {225 / 44100.0, 556 / 44100.0};

static long reverb_len(double secs, double sr) {
  long n = lround(secs * sr);
  return n > 1 ? n : 1;
}

/* Each comb's cursor, damp state and buffer; each allpass's cursor and
 * buffer. */
long dsp_reverb_slots(double sr) {
  long n = 0;
  for (int i = 0; i < 4; i++) n += 2 + reverb_len(comb_secs[i], sr);
  for (int i = 0; i < 2; i++) n += 1 + reverb_len(ap_secs[i], sr);
  return n;
}

/* Each comb's feedback gain from the decay time (one pow per comb), and
 * the combs' damping pole at this rate: o[0..3] the gains, o[4] the pole. */
void n_reverb_coef(double rt60, double sr, double* o) {
  for (int ci = 0; ci < 4; ci++) o[ci] = pow(10.0, -3.0 * (double)reverb_len(comb_secs[ci], sr) / (rt60 * sr));
  o[4] = pow(0.3, 44100.0 / sr);
}

double n_reverb_c(DspState* s, long site, double sig, double wet, const double* gains) {
  const double ap_fb = 0.5, damp = gains[4];
  long off = claim(s, site, dsp_reverb_slots(s->sr));
  double comb_sum = 0.0;
  for (int ci = 0; ci < 4; ci++) {
    long len = reverb_len(comb_secs[ci], s->sr);
    long cur_slot = off, damp_slot = off + 1, buf = off + 2;
    off += 2 + len;
    long cur = (long)s->pool[cur_slot] % len;
    double output = s->pool[buf + cur];
    double filt = output * (1.0 - damp) + s->pool[damp_slot] * damp;
    s->pool[damp_slot] = filt;
    s->pool[buf + cur] = sig + filt * gains[ci];
    s->pool[cur_slot] = (double)((cur + 1) % len);
    comb_sum += output;
  }
  double ap = comb_sum * 0.25;
  for (int ai = 0; ai < 2; ai++) {
    long len = reverb_len(ap_secs[ai], s->sr);
    long cur_slot = off, buf = off + 1;
    off += 1 + len;
    long cur = (long)s->pool[cur_slot] % len;
    double buf_out = s->pool[buf + cur];
    s->pool[buf + cur] = ap + buf_out * ap_fb;
    ap = buf_out - ap;
    s->pool[cur_slot] = (double)((cur + 1) % len);
  }
  return sig * (1.0 - wet) + ap * wet;
}

double n_reverb(DspState* s, long site, double sig, double rt60, double wet) {
  double g[5];
  n_reverb_coef(rt60, s->sr, g);
  return n_reverb_c(s, site, sig, wet, g);
}

/* ----------------------------------------------------------------- physics */

double n_impulse(DspState* s, long site, double freq) {
  long i = claim(s, site, 1);
  double prev = s->pool[i];
  s->pool[i] = fmod(s->pool[i] + freq / s->sr, 1.0);
  if (s->pool[i] < 0.0) s->pool[i] += 1.0;
  return s->pool[i] < prev ? 1.0 : 0.0;
}

/* A driven damped oscillator ringing at `freq` Hz; `decay` is its damping
 * rate (larger dies faster). */
double n_discharge(DspState* s, long site, double sig, double rate) {
  long i = claim(s, site, 1);
  s->pool[i] = max_(sig, s->pool[i] * (1.0 - rate / s->sr));
  return s->pool[i];
}

/* ----------------------------------------------------------------- Hilbert
 * Two cascaded 4-stage IIR allpass networks (Olli Niemitalo's design and
 * coefficients). Each stage is y[n] = a^2 (x[n] + y[n-2]) - x[n-2]: the
 * table holds a, and the stage uses its square. The first network's
 * output is then delayed one sample; with both, the two outputs are a
 * quarter turn apart, +-0.7 degrees, from 20 Hz to 20 kHz at 44.1 kHz:
 * the signal as a spin, going round a circle. Until 2026-11-05 the stages
 * used a unsquared and nothing was delayed, and the pair was 4.5 degrees
 * off at 441 Hz, 54 at 5 kHz, 85 at 10 kHz, so above a few hundred hertz
 * the "spin" wobbled along a line (tests/hilbert.c measures it).
 * State per stage x[n-2], x[n-1], y[n-2], y[n-1]: 16 slots a network,
 * and the first network's one-sample delay a 17th. */

static const double hilbert_a[4] = {0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
static const double hilbert_b[4] = {0.4021921162426, 0.8561710882487, 0.9722909545651, 0.9952884791278};

static inline double hilbert(DspState* s, long base, double sig, const double* coefs) {
  double x = sig;
  for (int k = 0; k < 4; k++) {
    long off = base + k * 4;
    double x2 = s->pool[off], x1 = s->pool[off + 1], y2 = s->pool[off + 2], y1 = s->pool[off + 3];
    double y = coefs[k] * coefs[k] * (x + y2) - x2;
    s->pool[off] = x1;
    s->pool[off + 1] = x;
    s->pool[off + 2] = y1;
    s->pool[off + 3] = y;
    x = y;
  }
  return x;
}

double n_hilbert_a(DspState* s, long site, double sig) {
  long base = claim(s, site, 17);
  double y = hilbert(s, base, sig, hilbert_a);
  double out = s->pool[base + 16];                /* one sample late: the design's delay */
  s->pool[base + 16] = y;
  return out;
}
double n_hilbert_b(DspState* s, long site, double sig) { return hilbert(s, claim(s, site, 16), sig, hilbert_b); }

/* ------------------------------------------------------------------- input */

double dsp_input_l, dsp_input_r;
double n_input_l(void) { return dsp_input_l; }
double n_input_r(void) { return dsp_input_r; }

/* ------------------------------------------------------------- the natives */

/* reverb's slots depend on the rate (its room is in seconds):
 * native_slot_size asks dsp_reverb_slots. */

/* ------------------------------------------------- a medium held as modes
 * The field core's (notes/one-system-2026-11-11.md; medium/modes.c is the
 * prototype these were proved on). A line under a linear law is the sum of
 * its modes, each a(t) phi(p) with a'' = -w^2 a - gamma a' + 2 phi(at) F:
 * stepped exactly, whatever the rate, so its partials are exact and a force
 * or a reading at a place is exact there. The voice keeps one state array a
 * medium: a header, then each mode's place and velocity and the four numbers
 * of its exact step for the half sample, worked out again only when the law's
 * coefficients change.
 *
 * Its law, in rounds of `period` T as a field's are: v' = A Δx + B x + C v
 * + D Δv + E ΔΔx + forces. A mode of wavenumber k (phi = sin or cos of k p)
 * has Δ = -4 k^2 (a push crosses in half a period) and ΔΔ = 4 k^4 / pi^2 (a
 * bar's period is its first mode's, SPEC "Four slopes are a bar"), so
 * w^2 = (4 A k^2 - B - 4 E k^4 / pi^2) / T^2 and gamma = (4 D k^2 - C) / T a second. Ends: 1 fixed, 2 free; the shapes
 * are sin (fixed at 0) or cos (free at 0), k = (j + o) pi with o = 1 fixed at
 * both, 0 free at both, 1/2 one of each. Round (ends 0, a ring, a push going
 * round in T): mode 0 the whole ring, then cos and sin of 2 pi n p for each
 * n, k = n pi. Modes above Nyquist are left out. */
enum { MD_N, MD_T, MD_A, MD_B, MD_C, MD_D, MD_EL, MD_ER, MD_H, MD_CAP, MD_E, MD_AT, MD_WIDTH, MD_SPREAD,
       MD_PAIR, MD_DI, MD_AXES, MD_EL2, MD_ER2, MD_AT2, MD_HEAD = 24 };
/* then N places, N velocities (a pair: N of the first field, N of its
 * partner), 4N of the step, N of a spread force's share, N more for the
 * partner's shape (a pair), and, a domain of two axes, each mode's index on
 * each axis (2N). */
#define MD_SLOTS(N) (MD_HEAD + 10 * (N))

/* ---- an axis: an interval with its two ends, or a circle ----
 * A domain is a product of axes, and each axis has modes of its own:
 *   an interval, its ends fixed (1) or free (2): k = (j + o) pi, o = 1 fixed at
 *     both, 0 free at both, 1/2 one of each; shape sin (fixed at 0) or cos
 *     (free at 0);
 *   a circle (round, 0): the whole round, then cos and sin of 2 pi n p, k = n pi.
 * Nothing else about a shape is a special case: a line is one interval, a ring
 * one circle, a sheet two intervals, and their modes are products. */
typedef struct { double el, er; } Axis;
static int ax_round(Axis a) { return a.el == 0; }
static double ax_offset(Axis a) { return a.el == a.er ? (a.el == 1 ? 1.0 : 0.0) : 0.5; }
static double ax_k(Axis a, long j) {                         /* mode j's wavenumber */
  return ax_round(a) ? (double)((j + 1) / 2) * PI_ : ((double)j + ax_offset(a)) * PI_;
}
/* The sign of ∇ on mode j's first shape: sin goes to +cos, cos to -sin. */
static double ax_sign(Axis a, long j) {
  if (ax_round(a)) return j == 0 ? 0.0 : (j % 2 ? -1.0 : 1.0);
  return a.el == 1 ? 1.0 : -1.0;
}
/* Whether mode j is the whole axis moving (its share 1, not 2). */
static int ax_rigid(Axis a, long j, int partner) { return j == 0 && (ax_round(a) || (!partner && a.el == 2 && a.er == 2)); }
/* phi_j(p) for every j, by turning: e^{i (j + o) pi p} = e^{i o pi p} e^{i pi p}^j.
 * `partner`: the shapes of a pair's other field, psi_j, each the other trig
 * function of phi_j's wavenumber (where ∇ takes phi_j), so ∇ phi_j = 2 k s_j
 * psi_j and ∇ psi_j = -2 k s_j phi_j, s_j its sign (ax_sign). */
static void ax_shapes(Axis a, double p, double* out, long n, int partner) {
  if (ax_round(a)) {                                        /* 1, then cos and sin of 2 pi n p */
    double rr = cos(TAU_ * p), ri = sin(TAU_ * p), zr = 1.0, zi = 0.0;
    if (n > 0) out[0] = 1.0;
    for (long j = 1; j < n; j += 2) {
      double t = zr * rr - zi * ri;
      zi = zr * ri + zi * rr;
      zr = t;
      out[j] = partner ? zi : zr;
      if (j + 1 < n) out[j + 1] = partner ? zr : zi;
    }
    return;
  }
  double o = ax_offset(a), zr = cos(PI_ * o * p), zi = sin(PI_ * o * p), rr = cos(PI_ * p), ri = sin(PI_ * p);
  int sine = (a.el == 1) != (partner != 0);
  for (long j = 0; j < n; j++) {
    out[j] = sine ? zi : zr;
    double t = zr * rr - zi * ri;
    zi = zr * ri + zi * rr;
    zr = t;
  }
}
/* A force spread at `at`, a Gaussian of area 1 and standard deviation `width`,
 * what of it lies past an end gone: each mode's share, 2 times the integral
 * along the axis of the force's shape times the mode's (1 for the whole axis
 * moving). Round, the Gaussian wraps and its transform is exact at any width;
 * well inside an interval, so is it; near an end it is integrated, finely
 * enough for the highest mode. A point is width 0. */
static void ax_shares(Axis a, double at, double width, double* out, long n, int partner) {
  double o = ax_offset(a);
  int sine = (a.el == 1) != (partner != 0);
  if (ax_round(a) || width <= 0.0 || (at - 8.0 * width > 0.0 && at + 8.0 * width < 1.0)) {
    ax_shapes(a, at, out, n, partner);
    for (long j = 0; j < n; j++) {
      double k = ax_k(a, j) * (ax_round(a) ? 2.0 : 1.0);     /* the shape's own wavenumber in p */
      out[j] *= (ax_rigid(a, j, partner) ? 1.0 : 2.0) * exp(-0.5 * k * k * width * width);
    }
    return;
  }
  double lo = max_(0.0, at - 8.0 * width), hi = min_(1.0, at + 8.0 * width);
  double kmax = (n + o) * PI_;
  long q = (long)ceil((hi - lo) * kmax / PI_ * 16.0) + 64;    /* sixteen points a half wave of the highest mode */
  if (q > 1 << 16) q = 1 << 16;
  if (q % 2) q++;
  for (long j = 0; j < n; j++) out[j] = 0.0;
  double dx = (hi - lo) / (double)q, norm = 1.0 / (width * sqrt(TAU_));
  for (long i = 0; i <= q; i++) {                              /* Simpson's rule */
    double x = lo + dx * (double)i, d = (x - at) / width;
    double wgt = (i == 0 || i == q ? 1.0 : (i % 2 ? 4.0 : 2.0)) * dx / 3.0 * norm * exp(-0.5 * d * d);
    double zr = cos(PI_ * o * x), zi = sin(PI_ * o * x), rr = cos(PI_ * x), ri = sin(PI_ * x);
    for (long j = 0; j < n; j++) {
      out[j] += wgt * (sine ? zi : zr);
      double t = zr * rr - zi * ri;
      zi = zr * ri + zi * rr;
      zr = t;
    }
  }
  for (long j = 0; j < n; j++) out[j] *= ax_rigid(a, j, partner) ? 1.0 : 2.0;
}

/* ---- a medium: one axis, or a product of two ---- */
static Axis md_axis(const double* m, int which) {
  return which ? (Axis){m[MD_EL2], m[MD_ER2]} : (Axis){m[MD_EL], m[MD_ER]};
}
static int md_two(const double* m) { return m[MD_AXES] == 2.0; }
/* Mode j's shape at a place, for every mode: one axis's own, or the product
 * of two axes' at (p, q). */
static void md_shapes_at(const double* m, double p, double q, double* out, long n, int partner) {
  if (!md_two(m)) { ax_shapes(md_axis(m, 0), p, out, n, partner); return; }
  long N = (long)m[MD_CAP], top = 0;
  const double *i1 = m + MD_HEAD + 8 * N, *i2 = i1 + N;
  for (long j = 0; j < n; j++) top = (long)max_((double)top, max_(i1[j], i2[j]) + 1.0);
  double a1[top], a2[top];
  ax_shapes(md_axis(m, 0), p, a1, top, 0);
  ax_shapes(md_axis(m, 1), q, a2, top, 0);
  for (long j = 0; j < n; j++) out[j] = a1[(long)i1[j]] * a2[(long)i2[j]];
}
static void md_shares_at(const double* m, double p, double q, double width, double* out, long n, int partner) {
  if (!md_two(m)) { ax_shares(md_axis(m, 0), p, width, out, n, partner); return; }
  long N = (long)m[MD_CAP], top = 0;                         /* a Gaussian is a product of one an axis */
  const double *i1 = m + MD_HEAD + 8 * N, *i2 = i1 + N;
  for (long j = 0; j < n; j++) top = (long)max_((double)top, max_(i1[j], i2[j]) + 1.0);
  double a1[top], a2[top];
  ax_shares(md_axis(m, 0), p, width, a1, top, 0);
  ax_shares(md_axis(m, 1), q, width, a2, top, 0);
  for (long j = 0; j < n; j++) out[j] = a1[(long)i1[j]] * a2[(long)i2[j]];
}
/* A coefficient that moves is followed to 1 part in 10^4: the steps are
 * worked out again when one has moved further, so a law whose loss drifts
 * (heaviside_line's, every sample) costs a recompute every few dozen
 * samples, not every one. In pitch that is at most 0.17 cents behind. */
#define MD_FOLLOW 1e-4
static int md_near(double now, double was) { return now == was || fabs(now - was) <= MD_FOLLOW * fabs(was); }

/* The exact step of one mode of a wave, its Δ eigenvalue -lam (rounds), for h. */
static void md_wave_step(double* p00, double* p01, double* p10, double* p11, double w2, double g, double h) {
  double s2 = w2 - 0.25 * g * g, c, S;
  if (s2 > 1e-12) { double sq = sqrt(s2); c = cos(sq * h); S = sin(sq * h) / sq; }
  else if (s2 < -1e-12) { double sq = sqrt(-s2); c = cosh(sq * h); S = sinh(sq * h) / sq; }
  else { c = 1.0; S = h; }
  double e = exp(-0.5 * g * h);
  *p00 = e * (c + 0.5 * g * S); *p01 = e * S;
  *p10 = -e * w2 * S;           *p11 = e * (c - 0.5 * g * S);
}
/* A wave, `$v' = A Δx + B x + C v + D Δv + E ΔΔx`: one axis, or two (el2 >= 0,
 * the second axis's ends; each axis's period T, a square). A mode's Δ is
 * -4 (k1^2 + k2^2); a bar's ΔΔ (one axis) 4 k^4 / pi^2. Two axes keep the
 * modes under Nyquist with the smallest k1^2 + k2^2, up to `cap`. */
double n_medium_turn(double* m, double cap, double T, double A, double B, double C, double D, double E,
                     double el, double er, double el2, double er2, double h) {
  long N = (long)cap;
  double* a = m + MD_HEAD;
  double *v = a + N, *p00 = v + N, *p01 = p00 + N, *p10 = p01 + N, *p11 = p10 + N;
  double axes = el2 >= 0 ? 2.0 : 1.0;
  if (m[MD_CAP] != cap || m[MD_AXES] != axes || m[MD_EL] != el || m[MD_ER] != er || m[MD_EL2] != el2 ||
      m[MD_ER2] != er2 || m[MD_H] != h || !md_near(T, m[MD_T]) || !md_near(A, m[MD_A]) || !md_near(B, m[MD_B]) ||
      !md_near(C, m[MD_C]) || !md_near(D, m[MD_D]) || !md_near(E, m[MD_E])) {
    int same_modes = m[MD_CAP] == cap && m[MD_AXES] == axes && m[MD_EL] == el && m[MD_ER] == er &&
                     m[MD_EL2] == el2 && m[MD_ER2] == er2;
    m[MD_CAP] = cap; m[MD_T] = T; m[MD_A] = A; m[MD_B] = B; m[MD_C] = C; m[MD_D] = D; m[MD_E] = E;
    m[MD_SPREAD] = 0.0;                                     /* a spread force's shares are worked out again */
    m[MD_EL] = el; m[MD_ER] = er; m[MD_EL2] = el2; m[MD_ER2] = er2; m[MD_H] = h; m[MD_AXES] = axes;
    double nyquist = PI_ / (2.0 * h);                        /* h is half a sample */
    long used = 0;
    if (axes == 1.0) {
      Axis x = md_axis(m, 0);
      for (long j = 0; j < N; j++) {
        double k = ax_k(x, j), w2 = (4.0 * A * k * k - B - 4.0 * E * k * k * k * k / (PI_ * PI_)) / (T * T), g = (4.0 * D * k * k - C) / T;
        if (w2 > nyquist * nyquist) break;
        md_wave_step(&p00[j], &p01[j], &p10[j], &p11[j], w2, g, h);
        used = j + 1;
      }
    } else {
      Axis x = md_axis(m, 0), y = md_axis(m, 1);
      double *i1 = m + MD_HEAD + 8 * N, *i2 = i1 + N;
      /* the largest k1^2 + k2^2 under Nyquist: 4 A lam / T^2 < nyquist^2 */
      double most = A > 0 ? nyquist * nyquist * T * T / (4.0 * A) : 0.0;
      /* the cut that keeps at most `cap`: the k^2 a count first passes it at */
      double lo = 0.0, hi = most;
      for (int it = 0; it < 60; it++) {
        double mid = 0.5 * (lo + hi);
        long count = 0;
        for (long j1 = 0; j1 < N && count <= N; j1++) {
          double k1 = ax_k(x, j1);
          if (k1 * k1 > mid) break;
          for (long j2 = 0; j2 < N; j2++) { double k2 = ax_k(y, j2); if (k1 * k1 + k2 * k2 > mid) break; count++; }
        }
        if (count <= N) lo = mid; else hi = mid;
      }
      if (!same_modes) for (long j = 0; j < N; j++) a[j] = v[j] = 0.0;
      for (long j1 = 0; j1 < N && used < N; j1++) {
        double k1 = ax_k(x, j1);
        if (k1 * k1 > lo) break;
        for (long j2 = 0; j2 < N && used < N; j2++) {
          double k2 = ax_k(y, j2), l = k1 * k1 + k2 * k2;
          if (l > lo) break;
          double w2 = (4.0 * A * l - B - 4.0 * E * l * l / (PI_ * PI_)) / (T * T), g = (4.0 * D * l - C) / T;
          md_wave_step(&p00[used], &p01[used], &p10[used], &p11[used], w2, g, h);
          i1[used] = (double)j1; i2[used] = (double)j2;
          used++;
        }
      }
    }
    for (long j = used; j < N; j++) a[j] = v[j] = 0.0;
    m[MD_N] = (double)used;
  }
  long n = (long)m[MD_N];
  for (long j = 0; j < n; j++) {
    double x = a[j], y = v[j];
    a[j] = p00[j] * x + p01[j] * y;
    v[j] = p10[j] * x + p11[j] * y;
  }
  return 0.0;
}
/* A force at a place, `force * δ(at)`, or spread, `δ(at, width)`: each mode
 * takes its share (ax_shares; a product of axes takes the product), kept
 * while the place and the width stay as they are, as they do through a
 * strike; a kick of nothing costs nothing. `impulse` is the force, in the
 * field's units (rounds), times the seconds it acts; `into` 0 a wave's
 * velocity, 1 a wave's place or a pair's first field, 2 a pair's partner (a
 * field's own rate: impulse / T, as a place's is). */
static double md_kick(double* m, double impulse, double at, double at2, double width, double into) {
  long n = (long)m[MD_N], N = (long)m[MD_CAP];
  if (n <= 0 || impulse == 0.0) return 0.0;
  int partner = into == 2;
  double* share = m + MD_HEAD + (partner ? 7 : 6) * N;
  int flag = partner ? 2 : 1;                                /* MD_SPREAD's bits: each shape's shares are worked out */
  if (!((int)m[MD_SPREAD] & flag) || m[MD_AT] != at || m[MD_AT2] != at2 || m[MD_WIDTH] != width) {
    if (m[MD_AT] != at || m[MD_AT2] != at2 || m[MD_WIDTH] != width) m[MD_SPREAD] = 0.0;
    md_shares_at(m, at, at2, width, share, n, partner);
    m[MD_AT] = at; m[MD_AT2] = at2; m[MD_WIDTH] = width;
    m[MD_SPREAD] = (double)((int)m[MD_SPREAD] | flag);
  }
  double* x = m + MD_HEAD + (into == 0 || into == 2 ? N : 0);
  double scale = impulse / (into ? m[MD_T] : m[MD_T] * m[MD_T]);
  for (long j = 0; j < n; j++) x[j] += share[j] * scale;
  return 0.0;
}
double n_medium_kick(double* m, double impulse, double at, double width, double into) {
  return md_kick(m, impulse, at, 0.0, width, into);
}
double n_medium_kick2(double* m, double impulse, double at, double at2, double width, double into) {
  return md_kick(m, impulse, at, at2, width, into);
}
static double md_read(const double* m, double at, double at2, int which) {   /* 0 place, 1 velocity, 2 partner */
  long n = (long)m[MD_N], N = (long)m[MD_CAP];
  if (n <= 0) return 0.0;
  double phi[n], x = 0.0;
  md_shapes_at(m, at, at2, phi, n, which == 2);
  const double* f = m + MD_HEAD + (which ? N : 0);
  for (long j = 0; j < n; j++) x += f[j] * phi[j];
  return which == 1 ? x * m[MD_T] : x;
}
double n_medium_at(double* m, double at) { return md_read(m, at, 0.0, 0); }
double n_medium_at2(double* m, double at, double at2) { return md_read(m, at, at2, 0); }
/* The velocity a field's law means, in rounds: T times the velocity a second. */
double n_medium_velocity_at(double* m, double at) { return md_read(m, at, 0.0, 1); }
double n_medium_velocity_at2(double* m, double at, double at2) { return md_read(m, at, at2, 1); }
/* A pair's partner at a place: its own shapes, psi_j. */
double n_medium_partner_at(double* m, double at) { return md_read(m, at, 0.0, 2); }
/* A pair, ∇ each the other's, as Heaviside's line is:
 *   v' = Cv ∇i + Gv v + Dv Δv,   i' = Ci ∇v + Gi i + Di Δi   (+ forces), in rounds.
 * Mode j holds v = a_j phi_j, i = b_j psi_j, and ∇ turns each into the other:
 *   a' = (Gv - 4 k^2 Dv) a - 2 k s Cv b,   b' = 2 k s Ci a + (Gi - 4 k^2 Di) b,
 * a 2x2 law per mode, stepped exactly by its exponential, worked out again
 * when a coefficient changes. With equal losses and no Δ (a lossless or
 * distortionless line), it is a turn and a decay: the turns by multiplying,
 * the decay one exp, so the period can move every sample, as a slid string's
 * does. Ends are the first field's; the partner's are the others. */
double n_medium_pair_turn(double* m, double cap, double T, double Cv, double Gv, double Dv,
                          double Ci, double Gi, double Di, double el, double er, double h) {
  long N = (long)cap;
  double* a = m + MD_HEAD;
  double *b = a + N, *p00 = b + N, *p01 = p00 + N, *p10 = p01 + N, *p11 = p10 + N;
  if (m[MD_PAIR] != 1.0 || m[MD_CAP] != cap || m[MD_EL] != el || m[MD_ER] != er || m[MD_H] != h ||
      !md_near(T, m[MD_T]) || !md_near(Cv, m[MD_A]) || !md_near(Gv, m[MD_B]) || !md_near(Dv, m[MD_D]) ||
      !md_near(Ci, m[MD_C]) || !md_near(Gi, m[MD_E]) || !md_near(Di, m[MD_DI])) {
    int was = m[MD_PAIR] == 1.0 && m[MD_CAP] == cap && m[MD_EL] == el && m[MD_ER] == er && m[MD_H] == h;
    m[MD_PAIR] = 1.0; m[MD_CAP] = cap; m[MD_T] = T; m[MD_A] = Cv; m[MD_B] = Gv; m[MD_D] = Dv;
    m[MD_C] = Ci; m[MD_E] = Gi; m[MD_DI] = Di; m[MD_EL] = el; m[MD_ER] = er; m[MD_H] = h;
    if (!was) m[MD_SPREAD] = 0.0;
    double nyquist = PI_ / (2.0 * h);
    long used = 0;
    int fast = Gv == Gi && Dv == 0.0 && Di == 0.0 && fabs(Cv) == fabs(Ci) && Cv != 0.0;
    double decay = exp(Gv / T * h), turn = 2.0 * fabs(Cv) * PI_ / T * h;   /* a wavenumber of pi turns this much */
    double o = ax_round(md_axis(m, 0)) ? 0.0 : ax_offset(md_axis(m, 0));
    /* the turn of wavenumber (j + o) pi, by multiplying: start at o, step one */
    double cr = cos(turn * o), ci = sin(turn * o), sr = cos(turn), si = sin(turn);
    double rr = 1.0, ri = 0.0;                                /* a ring's: n pi, n = (j + 1) / 2 */
    for (long j = 0; j < N; j++) {
      double k = ax_k(md_axis(m, 0), j), sgn = ax_sign(md_axis(m, 0), j);
      double w = 2.0 * fabs(Cv) * k / T;
      if (w > nyquist) break;
      double q = -2.0 * k * sgn * Cv / T, r = 2.0 * k * sgn * Ci / T;
      double pp = (Gv - 4.0 * k * k * Dv) / T, ss = (Gi - 4.0 * k * k * Di) / T;
      if (fast) {
        double c, sn;
        if (ax_round(md_axis(m, 0))) {
          if (j > 0 && j % 2) { double t = rr * sr - ri * si; ri = rr * si + ri * sr; rr = t; }
          c = rr; sn = ri;
        } else {
          c = cr; sn = ci;
          double t = cr * sr - ci * si; ci = cr * si + ci * sr; cr = t;
        }
        double sw = w > 0.0 ? sn / w : h;
        p00[j] = decay * c;      p01[j] = decay * q * sw;
        p10[j] = decay * r * sw; p11[j] = decay * c;
      } else {
        double tau = 0.5 * (pp + ss), del = 0.5 * (pp - ss), disc = del * del + q * r, c, S;
        if (disc < -1e-12) { double sq = sqrt(-disc); c = cos(sq * h); S = sin(sq * h) / sq; }
        else if (disc > 1e-12) { double sq = sqrt(disc); c = cosh(sq * h); S = sinh(sq * h) / sq; }
        else { c = 1.0; S = h; }
        double e = exp(tau * h);
        p00[j] = e * (c + del * S); p01[j] = e * q * S;
        p10[j] = e * r * S;         p11[j] = e * (c - del * S);
      }
      used = j + 1;
    }
    for (long j = used; j < N; j++) a[j] = b[j] = 0.0;
    m[MD_N] = (double)used;
  }
  long n = (long)m[MD_N];
  for (long j = 0; j < n; j++) {
    double x = a[j], y = b[j];
    a[j] = p00[j] * x + p01[j] * y;
    b[j] = p10[j] * x + p11[j] * y;
  }
  return 0.0;
}
const NativeSpec dsp_natives[] = {
  {"lp1", "signal,cut", 1, -1, (void*)n_lp1},
  {"hp1", "signal,cut", 1, -1, (void*)n_hp1},
  {"lpf", "signal,cut,res", 2, -1, (void*)n_lpf},
  {"hpf", "signal,cut,res", 2, -1, (void*)n_hpf},
  {"bpf", "signal,cut,res", 2, -1, (void*)n_bpf},
  {"notch", "signal,cut,res", 2, -1, (void*)n_notch},
  {"delay", "signal,time,max_time", 1, 2, (void*)n_delay},
  {"fbdelay", "signal,time,max_time,fb", 1, 2, (void*)n_fbdelay},
  {"tape", "signal,on,w,r,max_time,keep=0", 0, 4, (void*)n_tape},
  {"reverb", "signal,rt60,wet", 0, -1, (void*)n_reverb},
  {"impulse", "freq", 1, -1, (void*)n_impulse},
  {"discharge", "signal,rate", 1, -1, (void*)n_discharge},
  {"wave", "freq,arr", 1, -1, (void*)n_wave},     /* its table is emitted (codegen) */
  {0, 0, 0, 0, 0}
};
