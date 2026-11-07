/* patches/scanned_ring.aither's pads: a strike is a function of its age, so
 * a press needs no bookkeeping to reach every mass once. Each run is the
 * patch as it is, against the same run with nothing pressed. */

#include "check.h"
#define TAU 6.283185307179586

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
/* A law is one system: written either way round it is the same ring, and
 * one with no loss stays bounded. */
TEST(laws_have_no_order) {
  const char* head =
    "$x = ring(period = 0.5)\n"
    "$v = ring(period = 0.5)\n";
  const char* tail =
    "$v'[0.3] += if t < 0.01 then 10 else 0\n"
    "$x[0.5]\n";
  const char* laws[2] = {"$v' = -concentration($x) - $x * 60\n$x' = $v\n",
                         "$x' = $v\n$v' = -concentration($x) - $x * 60\n"};
  Rendered r[2] = {{0}, {0}};
  for (int k = 0; k < 2; k++) {
    char src[512], err[4096];
    snprintf(src, sizeof src, "%s%s%s", head, laws[k], tail);
    int rc = render_src(src, 8.0, 48000, &r[k], err, sizeof err);
    CHECK(rc == 0, "doesn't render: %s", err);
    if (rc) return;
  }
  CHECK(r[0].n == r[1].n, "%ld samples against %ld", r[0].n, r[1].n);
  double peak = 0, late = 0;
  for (long i = 0; i < r[0].n; i++) {
    CHECK(r[0].left[i] == r[1].left[i], "the order of the laws showed at sample %ld", i);
    if (r[0].left[i] != r[1].left[i]) break;
    peak = fmax(peak, fabs(r[0].left[i]));
    if (i > r[0].n / 2) late = fmax(late, fabs(r[0].left[i]));
  }
  printf("  either way round, peak %.4f, %.4f in the second half\n", peak, late);
  CHECK(peak > 0 && peak < 1, "peak %g", peak);
  CHECK(late < 2 * peak, "it grew: %g after %g", late, peak);
  rendered_free(&r[0]);
  rendered_free(&r[1]);
}

/* A shaped strike is the same strike at any rate, however few places it
 * crosses: the force is caught between visits. */
TEST(shaped_strike_lands_whole) {
  const int rates[] = {22050, 48000, 96000};
  const char* src =
    "$x = ring(period = 1.66)\n"
    "$v = ring(period = 1.66)\n"
    "$v' = -concentration($x) * 5 - $x * 200 - $v * 0.68\n"
    "$x' = $v\n"
    "$v'[gauss(0.4, 0.022)] += if t < 0.002 then 660 else 0\n"
    "$x[0.37]\n";
  double peak[3];
  for (int k = 0; k < 3; k++) {
    Rendered r = {0};
    char err[4096];
    int rc = render_src(src, 1.0, rates[k], &r, err, sizeof err);
    CHECK(rc == 0, "doesn't render at %d: %s", rates[k], err);
    if (rc) return;
    peak[k] = 0;
    for (long i = 0; i < r.n; i++) peak[k] = fmax(peak[k], fabs(r.left[i]));
    printf("  at %d Hz a 2 ms strike peaks at %.4f\n", rates[k], peak[k]);
    rendered_free(&r);
  }
  CHECK(peak[1] > 0.01, "nothing arrived: %g", peak[1]);
  for (int k = 0; k < 3; k++)
    CHECK(fabs(peak[k] / peak[1] - 1) < 0.1, "at %d Hz it peaked at %g, at 48000 %g", rates[k], peak[k], peak[1]);
}

/* The places hold the whole blow, at any width and wherever the centre
 * falls between them: read once round, their mean is its area. */
TEST(shaped_source_has_area_one) {
  const double widths[] = {0, 0.0005, 0.003, 0.022, 0.0899, 0.2, 0.3, 0.5, 2};
  const double centres[] = {0.4, 0.4031, 0.4047};
  double worst = 0;
  for (int w = 0; w < 9; w++)
    for (int k = 0; k < 3; k++) {
      char src[512], err[4096];
      snprintf(src, sizeof src,
        "$x = ring(period = 1)\n"
        "$x' = 0\n"
        "$x'[gauss(%g, %g)] += if t < 0.00051 then 1920 else 0\n"
        "$x[phasor(1)]\n", centres[k], widths[w]);
      Rendered r = {0};
      int rc = render_src(src, 2.0, 48000, &r, err, sizeof err);
      CHECK(rc == 0, "doesn't render: %s", err);
      if (rc) return;
      double area = 0;
      for (long i = 48000; i < 96000; i++) area += r.left[i];
      area /= 48000;
      worst = fmax(worst, fabs(area - 1));
      CHECK(fabs(area - 1) < 1e-4, "width %g at %g holds %.6f", widths[w], centres[k], area);
      rendered_free(&r);
    }
  printf("  area 1 to within %.2e, widths 0 to 2\n", worst);
}

/* Heaviside's line. A pair stays bounded with no loss, either way declared;
 * R = G keeps a pulse's shape as it dies; R alone smears it. */
static double pulse_width(const char* first, const char* second, double r, double g, double rounds, double* peak) {
  char src[640], err[4096];
  snprintf(src, sizeof src,
    "%s\n%s\n"
    "$v' = -slope($i) - $v * %g\n"
    "$i' = -slope($v) - $i * %g\n"
    "$v'[gauss(0.3, 0.03)] += if t < 0.002 then 8 else 0\n"
    "$v[0.55]\n", first, second, g, r);
  Rendered o = {0};
  if (render_src(src, rounds + 0.25, 48000, &o, err, sizeof err)) { CHECK(0, "doesn't render: %s", err); return -1; }
  long from = (long)((rounds - 0.25) * 48000), to = o.n;
  double top = 0;
  for (long i = from; i < to; i++) top = fmax(top, fabs(o.left[i]));
  long over = 0;
  for (long i = from; i < to; i++) if (fabs(o.left[i]) > top / 2) over++;
  *peak = top;
  rendered_free(&o);
  return (double)over / 48000;
}
TEST(heavisides_line) {
  const char* v = "$v = ring(period = 1)";
  const char* i = "$i = ring(period = 1)";
  double p0, p1, pd, pr;
  double first = pulse_width(v, i, 0, 0, 0.25, &p0);
  double lossless = pulse_width(v, i, 0, 0, 4.25, &p1);
  double swapped_peak;
  double swapped = pulse_width(i, v, 0, 0, 4.25, &swapped_peak);
  double balanced = pulse_width(v, i, 2, 2, 4.25, &pd);
  double resistive = pulse_width(v, i, 4, 0, 4.25, &pr);
  printf("  a pulse half-height wide: %.3f s at first; after 4 rounds %.3f, declared the other way %.3f, R = G %.3f, R alone %.3f\n",
         first, lossless, swapped, balanced, resistive);
  CHECK(p1 > 0.3 * p0 && p1 < 1.5 * p0, "no loss: %g after %g", p1, p0);
  CHECK(fabs(swapped_peak / p1 - 1) < 0.2, "declared the other way %g against %g", swapped_peak, p1);
  CHECK(fabs(balanced / lossless - 1) < 0.15, "R = G widened it: %g against %g", balanced, lossless);
  CHECK(resistive > 1.5 * balanced, "R alone didn't smear it: %g against %g", resistive, balanced);
}

/* A line's period is there and back, and its pitch is exact: the strongest
 * frequency near the one it should have. */
static double line_pitch(const char* decl, double expect) {
  char src[640], err[4096];
  snprintf(src, sizeof src, "%s\n"
    "$v'[gauss(0.3, 0.05)] += if t < 0.002 then 2 else 0\n"
    "$x[0.17]\n", decl);
  Rendered r = {0};
  if (render_src(src, 12.0, 48000, &r, err, sizeof err)) { CHECK(0, "doesn't render: %s", err); return -1; }
  double best = 0, top = 0, mean = 0;
  for (long i = 0; i < r.n; i++) mean += r.left[i] / r.n;
  for (double f = expect * 0.9; f < expect * 1.1; f += expect * 0.001) {
    double a = 0, b = 0;
    for (long i = 0; i < r.n; i += 16) {
      double w = 0.5 - 0.5 * cos(TAU * i / r.n), x = (r.left[i] - mean) * w;
      a += x * cos(TAU * f * i / 48000);
      b += x * sin(TAU * f * i / 48000);
    }
    if (a * a + b * b > top) { top = a * a + b * b; best = f; }
  }
  rendered_free(&r);
  return best;
}
TEST(line_tunes) {
  const char* fixed = "$x = line(period = 0.5, ends = fixed)\n$v = line(period = 0.5, ends = fixed)\n"
                      "$v' = -concentration($x)\n$x' = $v";
  const char* mixed = "$x = line(period = 0.5, ends = [fixed, free])\n$v = line(period = 0.5, ends = [fixed, free])\n"
                      "$v' = -concentration($x)\n$x' = $v";
  const char* mixed2 = "$x = line(period = 0.5, ends = [free, fixed])\n$v = line(period = 0.5, ends = [free, fixed])\n"
                       "$v' = -concentration($x)\n$x' = $v";
  const char* open = "$v = line(period = 0.5, ends = free)\n$x = line(period = 0.5, ends = fixed)\n"
                     "$v' = -slope($x)\n$x' = -slope($v)";
  const char* shorted = "$v = line(period = 0.5, ends = fixed)\n$x = line(period = 0.5, ends = free)\n"
                        "$v' = -slope($x)\n$x' = -slope($v)";
  double f[5] = {line_pitch(fixed, 2), line_pitch(mixed, 1), line_pitch(mixed2, 1), line_pitch(open, 2), line_pitch(shorted, 2)};
  double want[5] = {2, 1, 1, 2, 2};
  printf("  fixed %.3f Hz, fixed-free %.3f, free-fixed %.3f, open pair %.3f, shorted pair %.3f\n", f[0], f[1], f[2], f[3], f[4]);
  for (int k = 0; k < 5; k++) CHECK(fabs(f[k] / want[k] - 1) < 0.006, "case %d: %g Hz, not %g", k, f[k], want[k]);
}

TEST(ring_keeps_its_period) {
  const int rates[] = {24000, 48000, 96000};
  const double period = 0.8;
  const char* src =
    "$x = ring(period = 0.8)\n"
    "$v = ring(period = 0.8)\n"
    "$v' = -concentration($x)\n"
    "$x' = $v\n"
    "$v'[0.1 * (phasor(1000) * 0.2 - 0.1)] += if t < 0.01 then 1280 else 0\n"
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
