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

/* Claim n slots: the next ones, or the shared fallback region at the
 * pool's tail when the patch asks for more than it has (garbage state,
 * not a crash). */
static inline long claim(DspState* s, long n) {
  if (s->idx + n > s->pool_slots - POOL_OVERFLOW_SLOTS) {
    s->overflow = 1;
    return s->pool_slots - POOL_OVERFLOW_SLOTS;
  }
  long r = s->idx;
  s->idx += n;
  return r;
}

/* ---------------------------------------------------------------- shapes */

double shape_saw(double x) { return fmod(x / TAU_, 1.0) * 2.0 - 1.0; }
double shape_tri(double x) { return fabs(fmod(x / TAU_, 1.0) * 4.0 - 2.0) - 1.0; }
double shape_sqr(double x) { return fmod(x / TAU_, 1.0) < 0.5 ? 1.0 : -1.0; }

/* ----------------------------------------------------------- oscillators */

double n_wave(DspState* s, double freq, double* values, int n) {
  if (n <= 0) return 0.0;
  long i = claim(s, 1);
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

double n_lp1_c(DspState* s, double sig, double a) {
  long i = claim(s, 1);
  s->pool[i] += a * (sig - s->pool[i]);
  return s->pool[i];
}

double n_hp1_c(DspState* s, double sig, double a) {
  long i = claim(s, 1);
  s->pool[i] += a * (sig - s->pool[i]);
  return sig - s->pool[i];
}

double n_lp1(DspState* s, double sig, double cut) { return n_lp1_c(s, sig, one_pole_coef(cut, s->sr)); }
double n_hp1(DspState* s, double sig, double cut) { return n_hp1_c(s, sig, one_pole_coef(cut, s->sr)); }

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

static inline double svf_c(DspState* s, double sig, const double* c, int mode) {
  long i = claim(s, 2);
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

static inline double svf(DspState* s, double sig, double cut, double res, int mode) {
  double c[4];
  svf_coef(cut, res, s->sr, c);
  return svf_c(s, sig, c, mode);
}

double n_lpf(DspState* s, double sig, double cut, double res) { return svf(s, sig, cut, res, LOW); }
double n_hpf(DspState* s, double sig, double cut, double res) { return svf(s, sig, cut, res, HIGH); }
double n_bpf(DspState* s, double sig, double cut, double res) { return svf(s, sig, cut, res, BAND); }
double n_notch(DspState* s, double sig, double cut, double res) { return svf(s, sig, cut, res, NOTCH); }

double n_lpf_c(DspState* s, double sig, const double* c) { return svf_c(s, sig, c, LOW); }
double n_hpf_c(DspState* s, double sig, const double* c) { return svf_c(s, sig, c, HIGH); }
double n_bpf_c(DspState* s, double sig, const double* c) { return svf_c(s, sig, c, BAND); }
double n_notch_c(DspState* s, double sig, const double* c) { return svf_c(s, sig, c, NOTCH); }

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

double n_delay(DspState* s, double sig, double time, double max_time) {
  long len = maxl(1, (long)(max_time * s->sr));
  long base = claim(s, 1 + len);
  long cursor = (long)s->pool[base] % len;
  s->pool[base + 1 + cursor] = sig;
  double d = clamp_(time * s->sr, 0.0, (double)(len - 1));
  double out = tap_interp(s, base, len, cursor, d);
  s->pool[base] = (double)((cursor + 1) % len);
  return out;
}

double n_fbdelay(DspState* s, double sig, double time, double max_time, double fb) {
  long len = maxl(1, (long)(max_time * s->sr));
  long base = claim(s, 1 + len);
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

double n_tape(DspState* s, double sig, double on, double w, double r, double max_time, double keep) {
  long n = maxl(1, (long)(max_time * s->sr));
  long base = claim(s, n);
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
 * Schroeder: 4 parallel comb filters into 2 series allpass. */

static const long comb_lens[4] = {1557, 1617, 1491, 1422};
static const long ap_lens[2] = {225, 556};

/* Each comb's cursor, damp state and buffer; each allpass's cursor and
 * buffer. */
long dsp_reverb_slots(void) {
  long n = 0;
  for (int i = 0; i < 4; i++) n += 2 + comb_lens[i];
  for (int i = 0; i < 2; i++) n += 1 + ap_lens[i];
  return n;
}

/* Each comb's feedback gain from the decay time: one pow per comb. */
void n_reverb_coef(double rt60, double sr, double* o) {
  for (int ci = 0; ci < 4; ci++) o[ci] = pow(10.0, -3.0 * (double)comb_lens[ci] / (rt60 * sr));
}

double n_reverb_c(DspState* s, double sig, double wet, const double* gains) {
  const double ap_fb = 0.5, damp = 0.3;
  long off = claim(s, dsp_reverb_slots());
  double comb_sum = 0.0;
  for (int ci = 0; ci < 4; ci++) {
    long len = comb_lens[ci];
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
    long len = ap_lens[ai];
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

double n_reverb(DspState* s, double sig, double rt60, double wet) {
  double g[4];
  n_reverb_coef(rt60, s->sr, g);
  return n_reverb_c(s, sig, wet, g);
}

/* ----------------------------------------------------------------- physics */

double n_impulse(DspState* s, double freq) {
  long i = claim(s, 1);
  double prev = s->pool[i];
  s->pool[i] = fmod(s->pool[i] + freq / s->sr, 1.0);
  if (s->pool[i] < 0.0) s->pool[i] += 1.0;
  return s->pool[i] < prev ? 1.0 : 0.0;
}

/* A driven damped oscillator ringing at `freq` Hz; `decay` is its damping
 * rate (larger dies faster). */
double n_discharge(DspState* s, double sig, double rate) {
  long i = claim(s, 1);
  s->pool[i] = max_(sig, s->pool[i] * (1.0 - rate / s->sr));
  return s->pool[i];
}

/* ----------------------------------------------------------------- Hilbert
 * Two cascaded 4-stage IIR allpass networks (Niemitalo's coefficients).
 * Their outputs differ by about 90 degrees across the audio band with
 * matched group delay, so one signal into both gives an analytic pair.
 * Each stage: y[n] = c (x[n] + y[n-2]) - x[n-2], with state x[n-2],
 * x[n-1], y[n-2], y[n-1]: 16 slots a branch. */

static const double hilbert_a[4] = {0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
static const double hilbert_b[4] = {0.4021921162426, 0.8561710882487, 0.9722909545651, 0.9952884791278};

static inline double hilbert(DspState* s, long base, double sig, const double* coefs) {
  double x = sig;
  for (int k = 0; k < 4; k++) {
    long off = base + k * 4;
    double x2 = s->pool[off], x1 = s->pool[off + 1], y2 = s->pool[off + 2], y1 = s->pool[off + 3];
    double y = coefs[k] * (x + y2) - x2;
    s->pool[off] = x1;
    s->pool[off + 1] = x;
    s->pool[off + 2] = y1;
    s->pool[off + 3] = y;
    x = y;
  }
  return x;
}

double n_hilbert_a(DspState* s, double sig) { return hilbert(s, claim(s, 16), sig, hilbert_a); }
double n_hilbert_b(DspState* s, double sig) { return hilbert(s, claim(s, 16), sig, hilbert_b); }

/* Single-sideband frequency shift: the analytic signal turned at hz Hz,
 * its real part. hz > 0 shifts up, < 0 down, with no mirror image (unlike
 * a ring modulator); clean above about 30 Hz. The b branch gives -H[m],
 * so the sign of its term is +. */
double n_freq_shift(DspState* s, double sig, double hz) {
  long ab = claim(s, 32);
  double re = hilbert(s, ab, sig, hilbert_a);
  double im = hilbert(s, ab + 16, sig, hilbert_b);
  long ph = claim(s, 1);
  s->pool[ph] = fmod(s->pool[ph] + hz / s->sr, 1.0);
  if (s->pool[ph] < 0.0) s->pool[ph] += 1.0;
  double phase = s->pool[ph] * TAU_;
  return re * cos(phase) + im * sin(phase);
}

/* ------------------------------------------------------------------- input */

double dsp_input_l, dsp_input_r;
double n_input_l(void) { return dsp_input_l; }
double n_input_r(void) { return dsp_input_r; }

/* ------------------------------------------------------------- the natives */

#define REVERB_SLOTS ((2 + 1557) + (2 + 1617) + (2 + 1491) + (2 + 1422) + (1 + 225) + (1 + 556))

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
  {"reverb", "signal,rt60,wet", REVERB_SLOTS, -1, (void*)n_reverb},
  {"impulse", "freq", 1, -1, (void*)n_impulse},
  {"discharge", "signal,rate", 1, -1, (void*)n_discharge},
  {"wave", "freq,arr", 1, -1, (void*)n_wave},     /* its table is emitted (codegen) */
  {0, 0, 0, 0, 0}
};
