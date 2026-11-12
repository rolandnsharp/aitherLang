/* A horn's modes: Webster's equation, solved once (notes/one-system-2026-11-11.md).
 *
 * A pair with a width, `$p' = -∇$u ...`, `$u' = -∇$p ...`, the first field the
 * pressure: flow changes pressure over the area S = width^2, pressure changes
 * flow times it. Together, p'' = Δ_W p, Δ_W = 4 (1/S) d/dx (S dp/dx) in rounds
 * (a push crosses in half a period, as on a uniform line), Webster's equation.
 * Its modes phi_n, with Δ_W phi_n = -mu_n phi_n, are orthonormal under S:
 * the integral of S phi_m phi_n is 1 or 0. The partner's are psi_n = S ∇phi_n /
 * w_n, w_n = sqrt(mu_n), orthonormal under 1/S. Then each mode is a pair
 * turning at w_n, as a uniform line's is, exactly; and Δ_W, a horn's own
 * Laplacian (what the two laws make together, the cross-section's mean of
 * the 3D one), is diagonal in them, so a loss that grows with pitch stays
 * exact.
 *
 * Numerically: finite volumes on a grid of G cells, the width at the nodes and
 * between them; the operator, symmetrised by S, is tridiagonal, its lowest
 * eigenvalues found by bisection on its Sturm sequence, at G and 2G and
 * extrapolated (Richardson: the second-order error goes, leaving the fourth);
 * each mode's shape by inverse iteration at G. Ends: 1 fixed (p = 0), 2 free
 * (no flow out). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "horn.h"

/* The tridiagonal of the symmetrised operator on G cells: diagonal d, off e. */
static int horn_matrix(const double* S_node, const double* S_mid, int G, int el, int er, double* d, double* e, double* wt) {
  /* nodes 0..G; a fixed end's node is dropped (p = 0 there) */
  double h = 1.0 / G;
  int lo = el == 1 ? 1 : 0, hi = er == 1 ? G - 1 : G, n = hi - lo + 1;
  for (int k = 0; k < n; k++) {
    int i = lo + k;
    double vol = (i == 0 || i == G) ? 0.5 * h : h;              /* a free end's node holds half a cell */
    double left = i > 0 ? S_mid[i - 1] / h : 0.0, right = i < G ? S_mid[i] / h : 0.0;
    wt[k] = S_node[i] * vol;                                     /* the weight, S times the cell */
    d[k] = 4.0 * (left + right);                                 /* -Δ_W times the weight: a flux each side */
    if (k + 1 < n) e[k] = -4.0 * right;
  }
  /* symmetrise: W^{-1/2} K W^{-1/2} */
  for (int k = 0; k < n; k++) d[k] /= wt[k];
  for (int k = 0; k + 1 < n; k++) e[k] /= sqrt(wt[k] * wt[k + 1]);
  return n;
}
/* How many eigenvalues of the tridiagonal are under x (Sturm). */
static int sturm(const double* d, const double* e, int n, double x) {
  int count = 0;
  double q = d[0] - x;
  if (q < 0) count++;
  for (int k = 1; k < n; k++) {
    if (q == 0) q = 1e-300;
    q = d[k] - x - e[k - 1] * e[k - 1] / q;
    if (q < 0) count++;
  }
  return count;
}
static double kth(const double* d, const double* e, int n, int k, double from, double top) {   /* the k-th smallest, from 0 */
  double lo = from, hi = top;                                 /* above the one before it */
  for (int it = 0; it < 200 && hi - lo > 1e-12 * (1.0 + lo); it++) {
    double mid = 0.5 * (lo + hi);
    if (sturm(d, e, n, mid) > k) hi = mid; else lo = mid;
  }
  return 0.5 * (lo + hi);
}
static void widths(const double* width, int W, int G, double* S_node, double* S_mid) {
  /* the width given at W + 1 evenly spaced places, linear between */
  for (int i = 0; i <= G; i++) {
    double x = (double)i / G * W;
    int j = (int)floor(x); if (j >= W) j = W - 1;
    double f = x - j, w = width[j] * (1 - f) + width[j + 1] * f;
    S_node[i] = w * w;
  }
  for (int i = 0; i < G; i++) {
    double x = (i + 0.5) / G * W;
    int j = (int)floor(x); if (j >= W) j = W - 1;
    double f = x - j, w = width[j] * (1 - f) + width[j + 1] * f;
    S_mid[i] = w * w;
  }
}

int horn_modes(const double* width, int W, int el, int er, int want, double* mu, double* phi, int G) {
  double *S_node = malloc(sizeof(double) * (2 * G + 1)), *S_mid = malloc(sizeof(double) * 2 * G);
  double *d = malloc(sizeof(double) * (2 * G + 1)), *e = malloc(sizeof(double) * (2 * G + 1)), *wt = malloc(sizeof(double) * (2 * G + 1));
  double* coarse = malloc(sizeof(double) * want);
  int got = 0;
  /* eigenvalues at G and 2G, then extrapolated */
  for (int pass = 0; pass < 2; pass++) {
    int g = pass ? 2 * G : G;
    widths(width, W, g, S_node, S_mid);
    int n = horn_matrix(S_node, S_mid, g, el, er, d, e, wt);
    double top = 0;
    for (int k = 0; k < n; k++) top = fmax(top, d[k] + 2 * fabs(k ? e[k - 1] : 0) + 2 * fabs(k + 1 < n ? e[k] : 0));
    int m = want < n ? want : n;
    double before = -1e-9, ceiling = top;
    for (int k = 0; k < m; k++) {
      double v = kth(d, e, n, k, before, ceiling);
      before = v * (1.0 - 1e-9);
      /* the next one's ceiling: a guess above it if the count says so, else all of them */
      double guess = v * 4.0 + 100.0;
      ceiling = guess < top && sturm(d, e, n, guess) > k + 1 ? guess : top;
      if (pass == 0) coarse[k] = v; else mu[k] = (4.0 * v - coarse[k]) / 3.0;
    }
    got = m;
  }
  /* shapes at G, by inverse iteration on the coarse eigenvalue, back through W^{-1/2} */
  widths(width, W, G, S_node, S_mid);
  int n = horn_matrix(S_node, S_mid, G, el, er, d, e, wt);
  int lo = el == 1 ? 1 : 0;
  double *x = malloc(sizeof(double) * n), *c = malloc(sizeof(double) * n), *r = malloc(sizeof(double) * n);
  for (int k = 0; k < got; k++) {
    double shift = coarse[k] * (1.0 + 1e-10) + 1e-12;
    for (int i = 0; i < n; i++) x[i] = 1.0 + 0.001 * sin(1.0 + i * 0.7 + k);
    for (int it = 0; it < 4; it++) {                             /* solve (A - shift) y = x, Thomas */
      double b0 = d[0] - shift;
      c[0] = (n > 1 ? e[0] : 0) / b0; r[0] = x[0] / b0;
      for (int i = 1; i < n; i++) {
        double den = d[i] - shift - e[i - 1] * c[i - 1];
        if (den == 0) den = 1e-300;
        c[i] = i + 1 < n ? e[i] / den : 0;
        r[i] = (x[i] - e[i - 1] * r[i - 1]) / den;
      }
      x[n - 1] = r[n - 1];
      for (int i = n - 2; i >= 0; i--) x[i] = r[i] - c[i] * x[i + 1];
      double norm = 0;
      for (int i = 0; i < n; i++) norm += x[i] * x[i];
      norm = sqrt(norm);
      for (int i = 0; i < n; i++) x[i] /= norm;
    }
    /* phi = W^{-1/2} y, on all G + 1 nodes (0 at a fixed end), sign: positive near 0 */
    double* out = phi + (size_t)k * (G + 1);
    memset(out, 0, sizeof(double) * (G + 1));
    for (int i = 0; i < n; i++) out[lo + i] = x[i] / sqrt(wt[i]);
    int first = el == 1 ? 1 : 0;
    if (out[first + (el == 1)] < 0) for (int i = 0; i <= G; i++) out[i] = -out[i];
  }
  free(S_node); free(S_mid); free(d); free(e); free(wt); free(coarse); free(x); free(c); free(r);
  return got;
}
