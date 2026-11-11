#include "modes.h"
#include <math.h>
#include <stdlib.h>

void modes_line(Modes* m, double f0, double gamma, int most, double rate, double h) {
  int n = (int)(0.5 * rate / f0);           /* under Nyquist */
  if (n > most) n = most;
  m->n = n;
  double* all = calloc((size_t)n * 7, sizeof(double));
  m->a = all; m->v = all + n; m->w2 = all + 2 * n;
  m->p00 = all + 3 * n; m->p01 = all + 4 * n; m->p10 = all + 5 * n; m->p11 = all + 6 * n;
  for (int k = 0; k < n; k++) {
    double w = 2 * M_PI * f0 * (k + 1), wd = sqrt(w * w - 0.25 * gamma * gamma);
    double e = exp(-0.5 * gamma * h), c = cos(wd * h), s = sin(wd * h);
    m->w2[k] = w * w;
    m->p00[k] = e * (c + 0.5 * gamma * s / wd);
    m->p01[k] = e * s / wd;
    m->p10[k] = -e * w * w * s / wd;
    m->p11[k] = e * (c - 0.5 * gamma * s / wd);
  }
}

void modes_free(Modes* m) { free(m->a); }

void modes_step(Modes* m, double force, double at) {
  for (int k = 0; k < m->n; k++) {
    double rest = 2 * sin(M_PI * (k + 1) * at) * force / m->w2[k];
    double a = m->a[k] - rest, v = m->v[k];
    m->a[k] = m->p00[k] * a + m->p01[k] * v + rest;
    m->v[k] = m->p10[k] * a + m->p11[k] * v;
  }
}

void modes_kick(Modes* m, double impulse, double at) {
  for (int k = 0; k < m->n; k++) m->v[k] += 2 * sin(M_PI * (k + 1) * at) * impulse;
}

double modes_at(const Modes* m, double p) {
  double x = 0;
  for (int k = 0; k < m->n; k++) x += m->a[k] * sin(M_PI * (k + 1) * p);
  return x;
}
double modes_velocity_at(const Modes* m, double p) {
  double x = 0;
  for (int k = 0; k < m->n; k++) x += m->v[k] * sin(M_PI * (k + 1) * p);
  return x;
}
