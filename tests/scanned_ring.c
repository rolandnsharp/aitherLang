/* patches/scanned_ring.aither's pads: a strike is a function of its age, so
 * a press needs no bookkeeping to reach every mass once. Each run is the
 * patch as it is, against the same run with nothing pressed. */

#include "check.h"

enum { SR = 48000, N = 3 * SR, PRESS = 2 * SR + 1000 };

/* The radius drawn each sample, a pad pressed at `at` (0: never). */
static void run(int note, long at, double* radius) {
  midi_reset_all();
  char* src = slurp("patches/scanned_ring.aither");
  Voice* v = must_load(src, SR);
  free(src);
  double l, r;
  for (long i = 0; i < N; i++) {
    if (at && i == at) midi_note_on(note, 100);
    if (at && i == at + 500) midi_note_off(note);
    tick_at(v, (double)i / SR, &l, &r);
    radius[i] = hypot(l, r);
  }
  voice_dispose(v);
  midi_reset_all();
}

/* How far the press moved the ring, a quarter second on. */
static double moved(const double* a, const double* b) {
  double m = 0;
  for (long i = PRESS + SR / 4; i < PRESS + SR / 2; i++) m = fmax(m, fabs(a[i] - b[i]));
  return m;
}

TEST(scanned_ring_pads) {
  static double none[N], a[N], late[N], b[N];
  run(0, 0, none);
  run(36, PRESS, a);
  run(36, PRESS + 37, late);                /* a step's length is 128: elsewhere in it */
  run(37, PRESS, b);
  for (long i = 0; i < PRESS; i++)
    if (a[i] != none[i] || b[i] != none[i]) {
      CHECK(0, "the ring moved at sample %ld, before the press", i);
      break;
    }
  double ma = moved(a, none), ml = moved(late, none), mb = moved(b, none);
  printf("  pad A moved it %.3f, a press mid-step %.3f, pad B %.3f\n", ma, ml, mb);
  CHECK(ma > 0.02, "pad A moved the ring by %g", ma);
  CHECK(mb > 0.02, "pad B moved the ring by %g", mb);
  CHECK(fabs(ml - ma) < 0.2 * ma, "a press mid-step lands differently: %g against %g", ml, ma);
  double apart = 0;
  for (long i = PRESS; i < N; i++) apart = fmax(apart, fabs(a[i] - b[i]));
  CHECK(apart > 0.02, "narrow and broad are the same: %g", apart);
  for (long i = 0; i < N; i++) CHECK(a[i] < 1.0 && b[i] < 1.0, "radius %g at %ld", fmax(a[i], b[i]), i);
}

/* A ring's period is what it does: the engine picks other places at another
 * rate, and a push still gets a quarter of the way round in a quarter of it. */
TEST(ring_keeps_its_period) {
  const int rates[] = {24000, 48000, 96000};
  const double period = 0.8;
  const char* src =
    "$x = ring(period = 0.8)\n"
    "$v = ring(period = 0.8)\n"
    "$v = $v - concentration($x) * dt\n"
    "$x = $x + $v * dt\n"
    "$v[0.1 * (phasor(1000) * 0.2 - 0.1)] = if t < 0.01 then 2000 else 0\n"
    "$x[0.25]\n";
  double when[3];
  for (int k = 0; k < 3; k++) {
    Rendered r = {0};
    char err[4096];
    int rc = render_src(src, 0.6, rates[k], &r, err, sizeof err);
    CHECK(rc == 0, "doesn't render at %d: %s", rates[k], err);
    if (rc) return;
    double peak = 0;
    for (long i = 0; i < r.n; i++) peak = fmax(peak, fabs(r.left[i]));
    when[k] = 0;
    for (long i = 0; i < r.n; i++)
      if (fabs(r.left[i]) > 0.5 * peak) { when[k] = (double)i / rates[k]; break; }
    printf("  at %d Hz a push reaches a quarter round at %.3f s\n", rates[k], when[k]);
    CHECK(peak > 0, "nothing arrived at %d", rates[k]);
    rendered_free(&r);
  }
  for (int k = 0; k < 3; k++)
    CHECK(fabs(when[k] - when[1]) < 0.03 * period, "at %d Hz it took %g s, at 48000 %g s", rates[k], when[k], when[1]);
  CHECK(fabs(when[1] - period / 4) < 0.05 * period, "a quarter round took %g s of %g", when[1], period);
}
