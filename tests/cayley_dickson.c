/* `*`, `/` and conj against the doubling rule they come from.
 *
 * A pair is a complex number and a pair of pairs Hamilton's quaternion,
 * each the one below doubled (Cayley–Dickson): (a, b)(c, d) =
 * (a c − b conj(d), a d + b conj(c)), conj(a, b) = (conj(a), −b), a
 * number its own conjugate (notes/nested-pair-2026-11-05.md). codegen
 * writes the products out by hand; here the reference is the rule itself,
 * applied recursively to 1, 2 or 4 numbers, and compiled patches are
 * checked against it on random values: `*`, `/` and conj; `*`
 * associating and sizes multiplying; and a complex turn sitting inside the
 * quaternions as their first halves. The reference is checked first on
 * Hamilton's units (i j = k, j i = −k), so it is the note's convention. */

#include "check.h"

/* The doubling rule on n = 1, 2 or 4 numbers. */
static void cd_conj(int n, const double* x, double* out) {
  if (n == 1) { out[0] = x[0]; return; }
  int h = n / 2;
  cd_conj(h, x, out);
  for (int k = 0; k < h; k++) out[h + k] = -x[h + k];
}
static void cd_mul(int n, const double* x, const double* y, double* out) {
  if (n == 1) { out[0] = x[0] * y[0]; return; }
  int h = n / 2;
  const double *a = x, *b = x + h, *c = y, *d = y + h;
  double ac[2], bdc[2], ad[2], bcc[2], cd_[2], cc[2];
  cd_conj(h, d, cd_);
  cd_conj(h, c, cc);
  cd_mul(h, a, c, ac);
  cd_mul(h, b, cd_, bdc);
  cd_mul(h, a, d, ad);
  cd_mul(h, b, cc, bcc);
  for (int k = 0; k < h; k++) { out[k] = ac[k] - bdc[k]; out[h + k] = ad[k] + bcc[k]; }
}
static double cd_size2(int n, const double* x) {
  double s = 0;
  for (int k = 0; k < n; k++) s += x[k] * x[k];
  return s;
}

static unsigned long long seed = 0x9e3779b97f4a7c15ull;
static double rnd(void) {                       /* -2 .. 2, the same every run */
  seed = seed * 6364136223846793005ull + 1442695040888963407ull;
  return ((double)(seed >> 11) / 9007199254740992.0) * 4.0 - 2.0;
}

static int near(double a, double b) { return fabs(a - b) <= 1e-12 * fmax(1.0, fabs(b)); }

TEST(cayley_dickson_reference) {
  /* Hamilton's units, w x y z: i j = k, j i = -k, j k = i, k i = j, i i = -1. */
  const double i[4] = {0, 1, 0, 0}, j[4] = {0, 0, 1, 0}, k[4] = {0, 0, 0, 1};
  double r[4];
  cd_mul(4, i, j, r); CHECK(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 1, "i j is %g %g %g %g, not k", r[0], r[1], r[2], r[3]);
  cd_mul(4, j, i, r); CHECK(r[3] == -1, "j i is %g %g %g %g, not -k", r[0], r[1], r[2], r[3]);
  cd_mul(4, j, k, r); CHECK(r[1] == 1, "j k is %g %g %g %g, not i", r[0], r[1], r[2], r[3]);
  cd_mul(4, k, i, r); CHECK(r[2] == 1, "k i is %g %g %g %g, not j", r[0], r[1], r[2], r[3]);
  cd_mul(4, i, i, r); CHECK(r[0] == -1, "i i is %g %g %g %g, not -1", r[0], r[1], r[2], r[3]);
}

/* One patch per trial: each of its first samples gives one pair of the
 * results, chosen by the sample's index. */
TEST(cayley_dickson_turn) {
  enum { TRIALS = 40, OUTS = 12 };
  for (int trial = 0; trial < TRIALS; trial++) {
    double p[4], q[4], r[4], P[2], Q[2];
    for (int k = 0; k < 4; k++) { p[k] = rnd(); q[k] = rnd(); r[k] = rnd(); }
    for (int k = 0; k < 2; k++) { P[k] = rnd(); Q[k] = rnd(); }
    char src[4096];
    snprintf(src, sizeof src,
      "let p = ((%.17g, %.17g), (%.17g, %.17g))\n"
      "let q = ((%.17g, %.17g), (%.17g, %.17g))\n"
      "let r = ((%.17g, %.17g), (%.17g, %.17g))\n"
      "let zp = (%.17g, %.17g)\n"
      "let zq = (%.17g, %.17g)\n"
      "let ((a0, a1), (a2, a3)) = p * q\n"
      "let ((u0, u1), (u2, u3)) = p / q\n"
      "let ((c0, c1), (c2, c3)) = conj(p)\n"
      "let ((l0, l1), (l2, l3)) = (p * q) * r\n"
      "let ((m0, m1), (m2, m3)) = p * (q * r)\n"
      "let (z0, z1) = zp * zq\n"
      "let ((e0, e1), (e2, e3)) = (zp, (0, 0)) * (zq, (0, 0))\n"
      "let size = amplitude(p * q) - amplitude(p) * amplitude(q)\n"
      "let n = floor(t * sr + 0.5)\n"
      "if n == 0 then [a0, a1] else if n == 1 then [a2, a3]\n"
      "else if n == 2 then [u0, u1] else if n == 3 then [u2, u3]\n"
      "else if n == 4 then [c0, c1] else if n == 5 then [c2, c3]\n"
      "else if n == 6 then [l0 - m0, l1 - m1] else if n == 7 then [l2 - m2, l3 - m3]\n"
      "else if n == 8 then [z0, z1] else if n == 9 then [e0 - z0, e1 - z1]\n"
      "else if n == 10 then [e2, e3] else [size, 0]\n",
      p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3], r[0], r[1], r[2], r[3], P[0], P[1], Q[0], Q[1]);
    Voice* v = must_load(src, 48000.0);
    if (!v) return;
    double got[OUTS][2];
    for (int s = 0; s < OUTS; s++) tick_at(v, s / 48000.0, &got[s][0], &got[s][1]);

    double pq[4], cq[4], pcq[4], cp[4], zz[2];
    cd_mul(4, p, q, pq);
    cd_conj(4, q, cq);
    cd_mul(4, p, cq, pcq);
    double d = cd_size2(4, q);
    for (int k = 0; k < 4; k++) pcq[k] /= d;               /* unturn: p conj(q) over |q|^2 */
    cd_conj(4, p, cp);
    cd_mul(2, P, Q, zz);
    const double want[OUTS][2] = {
      {pq[0], pq[1]}, {pq[2], pq[3]}, {pcq[0], pcq[1]}, {pcq[2], pcq[3]}, {cp[0], cp[1]}, {cp[2], cp[3]},
      {0, 0}, {0, 0}, {zz[0], zz[1]}, {0, 0}, {0, 0}, {0, 0}};
    const char* what[OUTS] = {"turn w x", "turn y z", "unturn w x", "unturn y z", "conj w x", "conj y z",
                              "(pq)r - p(qr), w x", "(pq)r - p(qr), y z", "complex turn",
                              "complex turn inside the quaternions, less itself", "... its second half",
                              "|pq| - |p||q|"};
    for (int s = 0; s < OUTS; s++)
      for (int side = 0; side < 2; side++) {
        /* differences that should vanish are checked against the size of what they come from */
        double tol_of = s == 6 || s == 7 ? 64.0 : s == 11 ? 16.0 : 1.0;
        double w = want[s][side], g = got[s][side];
        CHECK(fabs(g - w) <= 1e-12 * fmax(tol_of, fabs(w)) || near(g, w),
              "trial %d, %s, side %d: %.17g, the rule gives %.17g", trial, what[s], side, g, w);
      }
    voice_dispose(v);
  }
}

/* The same rule through `*` and `/` on values written (a, b): the ladder's
 * product is turn, and the laws hold with a number or a narrower value
 * sitting inside. */
TEST(cayley_dickson_ladder) {
  enum { TRIALS = 40, OUTS = 10 };
  for (int trial = 0; trial < TRIALS; trial++) {
    double p[4], q[4], r[4], Z[2];
    for (int k = 0; k < 4; k++) { p[k] = rnd(); q[k] = rnd(); r[k] = rnd(); }
    for (int k = 0; k < 2; k++) Z[k] = rnd();
    char src[4096];
    snprintf(src, sizeof src,
      "let p = ((%.17g, %.17g), (%.17g, %.17g))\n"
      "let q = ((%.17g, %.17g), (%.17g, %.17g))\n"
      "let r = ((%.17g, %.17g), (%.17g, %.17g))\n"
      "let z = (%.17g, %.17g)\n"
      "let ((a0, a1), (a2, a3)) = p * q\n"
      "let ((u0, u1), (u2, u3)) = p / q\n"
      "let ((d0, d1), (d2, d3)) = p * (q + r) - (p * q + p * r)\n"
      "let ((n0, n1), (n2, n3)) = p * (q + 1) - (p * q + p)\n"
      "let ((e0, e1), (e2, e3)) = z * q - ((z, 0) * q)\n"
      "let n = floor(t * sr + 0.5)\n"
      "if n == 0 then [a0, a1] else if n == 1 then [a2, a3]\n"
      "else if n == 2 then [u0, u1] else if n == 3 then [u2, u3]\n"
      "else if n == 4 then [d0, d1] else if n == 5 then [d2, d3]\n"
      "else if n == 6 then [n0, n1] else if n == 7 then [n2, n3]\n"
      "else if n == 8 then [e0, e1] else [e2, e3]\n",
      p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3], r[0], r[1], r[2], r[3], Z[0], Z[1]);
    Voice* v = must_load(src, 48000.0);
    if (!v) return;
    double got[OUTS][2];
    for (int s = 0; s < OUTS; s++) tick_at(v, s / 48000.0, &got[s][0], &got[s][1]);
    double pq[4], cq[4], u[4];
    cd_mul(4, p, q, pq);
    cd_conj(4, q, cq);
    cd_mul(4, p, cq, u);
    double d = cd_size2(4, q);
    for (int k = 0; k < 4; k++) u[k] /= d;
    const double want[OUTS][2] = {{pq[0], pq[1]}, {pq[2], pq[3]}, {u[0], u[1]}, {u[2], u[3]},
                                  {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
    const char* what[OUTS] = {"p * q, w x", "p * q, y z", "p / q, w x", "p / q, y z",
                              "p(q + r) - (pq + pr), w x", "... y z", "p(q + 1) - (pq + p), w x", "... y z",
                              "z q - (z, 0) q, w x", "... y z"};
    for (int s = 0; s < OUTS; s++)
      for (int side = 0; side < 2; side++) {
        double w = want[s][side], g = got[s][side];
        CHECK(fabs(g - w) <= 1e-12 * fmax(s >= 4 ? 64.0 : 1.0, fabs(w)),
              "trial %d, %s, side %d: %.17g, the rule gives %.17g", trial, what[s], side, g, w);
      }
    voice_dispose(v);
  }
}
