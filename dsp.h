/* Native DSP primitives: called from a patch's generated C. Each claims
 * slots from its voice's pool (a flat array of doubles) at runtime: no
 * variants, no allocation.
 *
 * `idx` is pointed by the generated C at each call site's region before
 * the call, and a native claims from there, so the same call site gets
 * the same slots every sample (phase continuity), and hot reload carries
 * a region across by its identity (voice.commit). */

#ifndef AITHER_DSP_H
#define AITHER_DSP_H

/* A voice's pool is as big as its patch needs, plus this shared fallback
 * region at the tail for a claim that would run past it. */
#define POOL_OVERFLOW_SLOTS 128

typedef struct DspState {
  double* pool;      /* this voice's, right after its struct */
  long pool_slots;   /* its length, the overflow tail included */
  long idx;
  char overflow;     /* set when a claim had to clamp */
  double sr;
} DspState;

/* What a patch calls by name (the compiler finds natives here). */
double n_lp1(DspState* s, double sig, double cut);
double n_hp1(DspState* s, double sig, double cut);
double n_lpf(DspState* s, double sig, double cut, double res);
double n_hpf(DspState* s, double sig, double cut, double res);
double n_bpf(DspState* s, double sig, double cut, double res);
double n_notch(DspState* s, double sig, double cut, double res);
double n_delay(DspState* s, double sig, double time, double max_time);
double n_fbdelay(DspState* s, double sig, double time, double max_time, double fb);
double n_tape(DspState* s, double sig, double on, double w, double r, double max_time, double keep);
double n_reverb(DspState* s, double sig, double rt60, double wet);
double n_impulse(DspState* s, double freq);
double n_discharge(DspState* s, double sig, double rate);
double n_wave(DspState* s, double freq, double* values, int n);

/* What codegen calls itself: hoisted coefficients and their steps, the
 * shapes, the Hilbert pair and the input. */
double n_lp1_coef(double cutoff, double sr);
void n_turn(double* p, double cs, double sn);
void n_turn_clock(double* p, double cs, double sn);
double n_lp1_c(DspState* s, double sig, double a);
double n_hp1_c(DspState* s, double sig, double a);
void n_svf_coef(double cutoff, double res, double sr, double* o);
double n_lpf_c(DspState* s, double sig, const double* c);
double n_hpf_c(DspState* s, double sig, const double* c);
double n_bpf_c(DspState* s, double sig, const double* c);
double n_notch_c(DspState* s, double sig, const double* c);
void n_reverb_coef(double rt60, double sr, double* o);
double n_reverb_c(DspState* s, double sig, double wet, const double* g);
double n_hilbert_a(DspState* s, double sig);
double n_hilbert_b(DspState* s, double sig);
double shape_saw(double x);
double shape_tri(double x);
double shape_sqr(double x);

/* The current input frame, written by whoever drives tick(): the engine's
 * audio callback or a render. Zero when nothing feeds it. */
extern double dsp_input_l, dsp_input_r;
double n_input_l(void);
double n_input_r(void);

/* The slots one reverb claims. */
long dsp_reverb_slots(double sr);

/* Every native a patch calls by name, in one place: its parameters as
 * SPEC names them (comma-separated; `name=v` has a default), the slots
 * one call site claims, and which parameter (a literal, in seconds)
 * sizes a buffer of `slots + max(1, int(seconds * sr))` slots, or -1.
 * Its C function is `n_<name>`. Ended by a row with no name. */
typedef struct {
  const char* name;
  const char* params;
  long slots;
  int buf_arg;
  void* fn;
} NativeSpec;
extern const NativeSpec dsp_natives[];

#endif
