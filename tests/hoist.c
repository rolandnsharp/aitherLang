/* Hoisting: a native whose coefficient arguments can't change from
 * sample to sample gets them computed once (hoist_init) and steps with
 * the `_c` form; a modulated one keeps the plain call. Either way the
 * bits are the same: the same arithmetic, once instead of every sample. */

#include "check.h"

static const char* Patch =
  "let cut = 1200\n"
  "def warm(x, hz = 800): lp1(x, hz)\n"
  "play a:\n"
  "  let s = saw(TAU * phasor(55))\n"
  "  warm(s) * 0.1 + lpf(s, cut, 0.4) * 0.1 + hp1(s, 2000) * 0.05\n"
  "play b:\n"
  "  let m = 800 + 400 * sin(TAU * t)\n"
  "  lp1(saw(TAU * phasor(110 + 5 * sin(TAU * 0.5 * t))), m) * 0.1\n"
  "play c:\n"
  "  (a + b) |> reverb(2, 0.3)\n"
  "\n"
  "c\n";

/* Knobs: midi_cc is read once a block, so a name bound to one, a def
 * parameter given one, and costly pure math over them are invariant.
 * The per-partial exp of a knob-driven spectral morph moves to
 * hoist_init, and the bits stay the same. */
static const char* Knobs =
  "let morph = midi_cc(20)\n"
  "let u = clamp(morph * 2, 0, 1)\n"
  "def partial(n, u): exp(log(n) * (1 - u) + log(n * n) * u)\n"
  "let cut = 400 + 3000 * midi_cc(21)\n"
  "sum(4, n => sin(TAU * phasor(110 * partial(n, u))) / n) * 0.1 |> lp1(cut)\n";

static int count(const char* s, const char* sub) {
  int n = 0;
  for (const char* p = strstr(s, sub); p; p = strstr(p + strlen(sub), sub)) n++;
  return n;
}

static char* csrc_of(const char* src) {
  char err[4096];
  char* c = compile_csrc(src, "<inline>", err, sizeof err);
  CHECK(c, "doesn't compile: %s", err);
  return c ? c : strdup("");
}

/* The left channel of n samples from t=0. */
static double* render(const char* src, int n) {
  Voice* v = must_load(src, 48000.0);
  double* out = calloc(n, sizeof *out);
  if (!v) return out;
  for (int i = 0; i < n; i++) {
    double r;
    tick_at(v, (double)i / 48000.0, &out[i], &r);
  }
  return out;
}

enum { N = 2400 };

TEST(hoist) {
  /* Turning (a sine of a steady phasor or of the clock as a turning
   * point) needs hoisting too, but changes the last bits: it has its own
   * bound (tests/lang/clock.aither, make bench). Off here, so this pins
   * hoisting alone. The compiler reads it once a process: before any
   * compile. */
  setenv("AITHER_TURN", "0", 1);
  setenv("AITHER_HOIST", "1", 1);
  char* on_all = csrc_of(Patch);
  /* Past the extern declarations: hoist_init, then the per-sample code. */
  const char* on = strstr(on_all, "static void hoist_init");
  CHECK(on, "no hoist_init");
  if (!on) return;
  /* The constant ones are hoisted: lp1 via the def's default, the SVF
   * with a let-bound cutoff, hp1 with a literal, reverb's decay, both
   * constant phasors. The modulated lp1 and phasor in `b` are not. */
  CHECK(count(on, "n_lp1_coef(") == 2, "n_lp1_coef( %d times, wanted 2 (800 and 2000):\n%s",
        count(on, "n_lp1_coef("), on);
  CHECK(count(on, "n_lp1_c(") == 1 && count(on, "n_hp1_c(") == 1, "n_lp1_c( %d, n_hp1_c( %d",
        count(on, "n_lp1_c("), count(on, "n_hp1_c("));
  CHECK(count(on, "n_svf_coef(") == 1 && count(on, "n_lpf_c(") == 1, "n_svf_coef( %d, n_lpf_c( %d",
        count(on, "n_svf_coef("), count(on, "n_lpf_c("));
  /* `a + b` is a pair (parts are), so reverb is split per channel: two
   * steps, one hoisted set of coefficients. */
  CHECK(count(on, "n_reverb_coef(") == 1 && count(on, "n_reverb_c(") == 2,
        "n_reverb_coef( %d, n_reverb_c( %d:\n%s", count(on, "n_reverb_coef("),
        count(on, "n_reverb_c("), on);
  CHECK(count(on, "n_lp1((DspState") == 1, "the modulated lp1 stays plain");
  CHECK(count(on, "hoist_init(s);") == 2, "both entry points init");
  double* a_on = render(Patch, N);

  setenv("AITHER_HOIST", "0", 1);
  char* off = csrc_of(Patch);
  /* the extern only */
  CHECK(count(off, "n_lp1_coef(") == 1 && count(off, "_c((DspState") == 0,
        "unhoisted: n_lp1_coef( %d, _c((DspState %d", count(off, "n_lp1_coef("),
        count(off, "_c((DspState"));
  double* a_off = render(Patch, N);
  setenv("AITHER_HOIST", "1", 1);

  double peak = 0.0;
  for (int i = 0; i < N; i++) {
    peak = fmax(peak, fabs(a_on[i]));
    if (a_on[i] != a_off[i]) {
      CHECK(0, "sample %d: %.17g vs %.17g", i, a_on[i], a_off[i]);
      break;
    }
  }
  CHECK(peak > 0.001, "the patch made no sound");

  char* k_on = csrc_of(Knobs);
  char* at = strstr(k_on, "static void hoist_init(");
  CHECK(at, "no hoist_init in the knobs patch");
  if (!at) return;
  char* end = strstr(at, "\n}");
  char* hoist_init = strndup(at, end ? (size_t)(end - at + 1) : strlen(at));
  CHECK(count(hoist_init, "exp(") >= 4 && strstr(hoist_init, "n_midi_cc"),
        "the knob-driven exps are computed in hoist_init:\n%s", hoist_init);
  double* k1 = render(Knobs, N);
  setenv("AITHER_HOIST", "0", 1);
  double* k0 = render(Knobs, N);
  setenv("AITHER_HOIST", "1", 1);
  for (int i = 0; i < N; i++)
    if (k1[i] != k0[i]) {
      CHECK(0, "knob sample %d: %.17g vs %.17g", i, k1[i], k0[i]);
      break;
    }
}
