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
  const char* laws[2] = {"$v' = slope(slope($x)) - $x * 60\n$x' = $v\n",
                         "$x' = $v\n$v' = slope(slope($x)) - $x * 60\n"};
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
    "$v' = slope(slope($x)) * 5 - $x * 200 - $v * 0.68\n"
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
                      "$v' = slope(slope($x))\n$x' = $v";
  const char* mixed = "$x = line(period = 0.5, ends = [fixed, free])\n$v = line(period = 0.5, ends = [fixed, free])\n"
                      "$v' = slope(slope($x))\n$x' = $v";
  const char* mixed2 = "$x = line(period = 0.5, ends = [free, fixed])\n$v = line(period = 0.5, ends = [free, fixed])\n"
                       "$v' = slope(slope($x))\n$x' = $v";
  const char* open = "$v = line(period = 0.5, ends = free)\n$x = line(period = 0.5, ends = fixed)\n"
                     "$v' = -slope($x)\n$x' = -slope($v)";
  const char* shorted = "$v = line(period = 0.5, ends = fixed)\n$x = line(period = 0.5, ends = free)\n"
                        "$v' = -slope($x)\n$x' = -slope($v)";
  double f[5] = {line_pitch(fixed, 2), line_pitch(mixed, 1), line_pitch(mixed2, 1), line_pitch(open, 2), line_pitch(shorted, 2)};
  double want[5] = {2, 1, 1, 2, 2};
  printf("  fixed %.3f Hz, fixed-free %.3f, free-fixed %.3f, open pair %.3f, shorted pair %.3f\n", f[0], f[1], f[2], f[3], f[4]);
  for (int k = 0; k < 5; k++) CHECK(fabs(f[k] / want[k] - 1) < 0.006, "case %d: %g Hz, not %g", k, f[k], want[k]);
}

/* What comes back from the far end, against what went out: a pulse is read
 * going past 0.75, and again where it would return. */
static double echo_db(const char* decl, double width) {
  char src[640], err[4096];
  snprintf(src, sizeof src, "%s\n"
    "$v'[gauss(0.5, %g)] += if t < 0.002 then 100 else 0\n"
    "$v[0.75]\n", decl, width);
  Rendered r = {0};
  if (render_src(src, 1.4, 48000, &r, err, sizeof err)) { CHECK(0, "doesn't render: %s", err); return 0; }
  double out = 0, back = 0;                /* period 2: across in 1 s, 0.75 at 0.25 s, back at 0.75 s */
  for (long i = 0; i < r.n; i++) {
    double s = (double)i / 48000, a = fabs(r.left[i]);
    if (s < 0.45) out = fmax(out, a);
    else if (s < 1.05) back = fmax(back, a);
  }
  rendered_free(&r);
  return 20 * log10((back + 1e-12) / out);
}
TEST(matched_end_takes_it_all) {
  const char* wave = "$x = line(period = 2, ends = %s)\n$v = line(period = 2, ends = %s)\n$v' = slope(slope($x))\n$x' = $v";
  const char* pair = "$v = line(period = 2, ends = %s)\n$i = line(period = 2, ends = %s)\n$v' = -slope($i)\n$i' = -slope($v)";
  char d[300];
  snprintf(d, sizeof d, wave, "[free, free]", "[free, free]");
  double free_end = echo_db(d, 0.03);
  snprintf(d, sizeof d, wave, "[free, matched]", "[free, matched]");
  double wide = echo_db(d, 0.03), narrow = echo_db(d, 0.01);
  snprintf(d, sizeof d, pair, "[free, free]", "[fixed, fixed]");
  double open_end = echo_db(d, 0.03);
  snprintf(d, sizeof d, pair, "[free, matched]", "[fixed, matched]");
  double pair_end = echo_db(d, 0.03);
  printf("  what comes back: free %.1f dB, matched %.1f (narrow pulse %.1f); open pair %.1f, matched pair %.1f\n",
         free_end, wide, narrow, open_end, pair_end);
  CHECK(free_end > -3, "a free end gave back %g dB", free_end);
  CHECK(open_end > -3, "an open end gave back %g dB", open_end);
  CHECK(wide < -30, "a matched end gave back %g dB", wide);
  CHECK(pair_end < -30, "a matched pair gave back %g dB", pair_end);
}

/* A field short enough for a note: a line at 110 Hz, its first partials
 * true, a wave or a pair, and bounded. */
static double fine_pitch(const char* decl, double want) {
  char src[640], err[4096];
  snprintf(src, sizeof src, "%s\n"
    "$v'[gauss(0.3, 0.05)] += if t < 0.0005 then 2 else 0\n"
    "$x[0.13]\n", decl);
  Rendered r = {0};
  if (render_src(src, 1.0, 48000, &r, err, sizeof err)) { CHECK(0, "doesn't render: %s", err); return -1; }
  double best = 0, top = 0, mean = 0, peak = 0;
  for (long i = 0; i < r.n; i++) { mean += r.left[i] / r.n; peak = fmax(peak, fabs(r.left[i])); }
  for (double f = want * 0.96; f < want * 1.04; f += want * 0.001) {
    double a = 0, b = 0;
    for (long i = 0; i < r.n; i++) {
      double w = 0.5 - 0.5 * cos(TAU * i / r.n), x = (r.left[i] - mean) * w;
      a += x * cos(TAU * f * i / 48000);
      b += x * sin(TAU * f * i / 48000);
    }
    if (a * a + b * b > top) { top = a * a + b * b; best = f; }
  }
  rendered_free(&r);
  CHECK(peak > 0 && peak < 8, "peak %g at %g Hz", peak, want);
  return best;
}
TEST(fine_fields_hold_a_note) {
  const char* wave = "$x = line(period = 1 / 110, ends = fixed)\n$v = line(period = 1 / 110, ends = fixed)\n"
                     "$v' = slope(slope($x)) - $v * 0.05\n$x' = $v";
  const char* pair = "$v = line(period = 1 / 110, ends = free)\n$x = line(period = 1 / 110, ends = fixed)\n"
                     "$v' = -slope($x) - $v * 0.05\n$x' = -slope($v) - $x * 0.05";
  double w1 = fine_pitch(wave, 110), w3 = fine_pitch(wave, 330), p1 = fine_pitch(pair, 110), p3 = fine_pitch(pair, 330);
  printf("  a line at 110 Hz: wave %.1f, %.1f Hz; pair %.1f, %.1f Hz\n", w1, w3, p1, p3);
  CHECK(fabs(w1 / 110 - 1) < 0.01 && fabs(p1 / 110 - 1) < 0.01, "%g, %g", w1, p1);
  CHECK(fabs(w3 / 330 - 1) < 0.01 && fabs(p3 / 330 - 1) < 0.01, "%g, %g", w3, p3);
}

/* A bore closed at one end: a cylinder's modes are odd, and a cone's aren't
 * tan(kL) = -k x0, x0 from its apex to its narrow end. */
static double bore_mode(const char* width, double near) {
  char src[640], err[4096];
  snprintf(src, sizeof src,
    "$p = line(period = 0.5, ends = [free, fixed], width = p => %s)\n"
    "$u = line(period = 0.5, ends = [fixed, free])\n"
    "$p' = -slope($u) - $p * 0.02\n"
    "$u' = -slope($p) - $u * 0.02\n"
    "$p'[0.01] += if t < 0.01 then 0.05 else 0\n"
    "$p[0.02]\n", width);
  Rendered r = {0};
  if (render_src(src, 20.0, 12000, &r, err, sizeof err)) { CHECK(0, "doesn't render: %s", err); return -1; }
  double best = 0, top = 0, mean = 0;
  for (long i = 0; i < r.n; i++) mean += r.left[i] / r.n;
  for (double f = near * 0.92; f < near * 1.08; f += 0.01) {
    double a = 0, b = 0;
    for (long i = 0; i < r.n; i += 40) {
      double w = 0.5 - 0.5 * cos(TAU * i / r.n), x = (r.left[i] - mean) * w;
      a += x * cos(TAU * f * i / 12000);
      b += x * sin(TAU * f * i / 12000);
    }
    if (a * a + b * b > top) { top = a * a + b * b; best = f; }
  }
  rendered_free(&r);
  return best;
}
TEST(bores_resonate_as_they_should) {
  double c1 = bore_mode("1", 1), c2 = bore_mode("1", 3), k1 = bore_mode("0.1 + p", 1.82), k2 = bore_mode("0.1 + p", 3.67);
  printf("  cylinder %.2f, %.2f Hz (1, 3); cone %.2f, %.2f (1.82, 3.67)\n", c1, c2, k1, k2);
  CHECK(fabs(c1 - 1) < 0.02 && fabs(c2 - 3) < 0.04, "cylinder %g, %g", c1, c2);
  CHECK(fabs(k1 - 1.82) < 0.03 && fabs(k2 - 3.67) < 0.05, "cone %g, %g", k1, k2);
}

/* A spring as two laws, against the same spring by `dho`: either order of
 * the lines is the same sound, and the pitch is the spring's. */
static double law_pitch(const char* law, double f) {
  char src[512], err[4096];
  snprintf(src, sizeof src, "let w = TAU * %g\n$x = 1\n$v = 0\n%s\n$x\n", f, law);
  Rendered r = {0};
  if (render_src(src, 0.5, 48000, &r, err, sizeof err)) { CHECK(0, "doesn't render: %s", err); return -1; }
  long first = -1, last = -1, cross = 0;
  for (long i = 1; i < r.n; i++)
    if (r.left[i - 1] < 0 && r.left[i] >= 0) { if (first < 0) first = i; last = i; cross++; }
  double got = (cross - 1) / ((last - first) / 48000.0), peak = 0;
  for (long i = 0; i < r.n; i++) peak = fmax(peak, fabs(r.left[i]));
  CHECK(peak < 1.05, "it grew to %g", peak);
  rendered_free(&r);
  return got;
}
TEST(spring_as_laws) {
  double a = law_pitch("$v' = -$x * w ^ 2\n$x' = $v", 440), b = law_pitch("$x' = $v\n$v' = -$x * w ^ 2", 440);
  double high = law_pitch("$v' = -$x * w ^ 2\n$x' = $v", 3000);
  printf("  a spring at 440 Hz: %.2f, either way written; at 3000 Hz, %.1f (%.1f cents sharp)\n", a, high, 1200 * log2(high / 3000));
  CHECK(a == b, "the order showed: %g against %g", a, b);
  CHECK(fabs(a / 440 - 1) < 0.002, "%g", a);
}

/* Four slopes are a bar. A mode near where it should be, from a strike. */
static double bar_mode(const char* ends, double near) {
  char src[640], err[4096];
  snprintf(src, sizeof src,
    "$x = line(period = 1 / 220, ends = %s)\n$v = line(period = 1 / 220, ends = %s)\n"
    "$v' = -slope(slope(slope(slope($x)))) - $v * 0.05\n$x' = $v\n"
    "$v'[gauss(0.37, 0.03)] += if t < 0.0005 then 1 else 0\n$v[0.23]\n", ends, ends);
  Rendered r = {0};
  if (render_src(src, 0.5, 48000, &r, err, sizeof err)) { CHECK(0, "doesn't render: %s", err); return -1; }
  double best = 0, top = 0, peak = 0;
  for (long i = 0; i < r.n; i++) peak = fmax(peak, fabs(r.left[i]));
  CHECK(peak > 0 && peak < 8, "peak %g", peak);
  for (double f = near * 0.9; f < near * 1.1; f += near * 0.002) {
    double a = 0, b = 0;
    for (long i = 0; i < r.n; i++) {
      double w = 0.5 - 0.5 * cos(TAU * i / r.n);
      a += r.left[i] * w * cos(TAU * f * i / 48000);
      b += r.left[i] * w * sin(TAU * f * i / 48000);
    }
    if (a * a + b * b > top) { top = a * a + b * b; best = f; }
  }
  rendered_free(&r);
  return best;
}
TEST(four_slopes_are_a_bar) {
  double p1 = bar_mode("fixed", 220), p2 = bar_mode("fixed", 860);
  double f1 = bar_mode("free", 220), f2 = bar_mode("free", 595);
  printf("  pinned %.0f, %.0f Hz; free %.0f, %.0f Hz\n", p1, p2, f1, f2);
  CHECK(fabs(p1 / 220 - 1) < 0.01, "pinned %g", p1);
  CHECK(fabs(p2 / p1 - 3.9) < 0.15, "pinned ratio %g", p2 / p1);
  CHECK(fabs(f1 / 220 - 1) < 0.02, "free %g", f1);
  CHECK(fabs(f2 / f1 - 2.7) < 0.1, "free ratio %g", f2 / f1);
}

/* A head stiffer the further it moves: the sweep of a kick is in the law,
 * so it starts higher hit harder, and falls to its tuning. */
static double kick_start(double hit, double* end) {
  char src[512], err[4096];
  snprintf(src, sizeof src,
    "let w = TAU * 50\nlet blow = if t < 0.001 then %g / 0.001 else 0\n$x = 0\n$v = 0\n"
    "$v' = -$x * w ^ 2 * (1 + 30 * $x ^ 2) - $v * 28 + blow\n$x' = $v\n$x\n", hit);
  Rendered r = {0};
  if (render_src(src, 0.5, 48000, &r, err, sizeof err)) { CHECK(0, "doesn't render: %s", err); return -1; }
  long z[64];
  int n = 0;
  for (long i = 1; i < r.n && n < 64; i++) if (r.left[i - 1] < 0 && r.left[i] >= 0) z[n++] = i;
  double first = 48000.0 / (z[1] - z[0]);
  *end = 48000.0 / (z[n - 1] - z[n - 2]);
  rendered_free(&r);
  return first;
}
TEST(a_kick_is_a_law) {
  double hard_end, soft_end, hard = kick_start(900, &hard_end), soft = kick_start(450, &soft_end);
  printf("  hit hard it starts at %.0f Hz, soft %.0f, and they end at %.0f and %.0f\n", hard, soft, hard_end, soft_end);
  CHECK(hard > soft * 1.2, "%g against %g", hard, soft);
  CHECK(hard_end < 60 && soft_end < 60, "%g, %g", hard_end, soft_end);
}

TEST(ring_keeps_its_period) {
  const int rates[] = {24000, 48000, 96000};
  const double period = 0.8;
  const char* src =
    "$x = ring(period = 0.8)\n"
    "$v = ring(period = 0.8)\n"
    "$v' = slope(slope($x))\n"
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
