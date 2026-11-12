/* A horn's modes, Webster's equation solved once (horn.c). */
#ifndef AITHER_HORN_H
#define AITHER_HORN_H
/* The lowest `want` modes of a horn whose width is given at W + 1 evenly
 * spaced places, its ends `el`, `er` (1 fixed, 2 free): mu, each mode's -Δ_W
 * eigenvalue in rounds (its turn is sqrt(mu) a round), and phi, each mode's
 * shape at G + 1 evenly spaced places, orthonormal under S = width^2. Returns
 * how many. */
int horn_modes(const double* width, int W, int el, int er, int want, double* mu, double* phi, int G);
#endif
