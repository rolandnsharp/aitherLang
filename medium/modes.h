/* A linear medium held as its modes (notes/one-system-2026-11-11.md).
 *
 * A mode is a(t) with a'' = -w^2 a - gamma a' + g F: one degree of freedom,
 * stepped exactly. For a step h, Phi = exp(A h) with A = [[0, 1], [-w^2,
 * -gamma]] is worked out once; a force held constant over the step moves the
 * rest point to g F / w^2, and the mode turns exactly about it. So partials,
 * a strike at a point and a reading at a point are exact at any rate, and the
 * only approximation is how the force varies within a step. */
#ifndef MEDIUM_MODES_H
#define MEDIUM_MODES_H

typedef struct {
  int n;                  /* modes kept: those under Nyquist, at most the ask */
  double *a, *v;          /* each mode's place and velocity */
  double *w2;             /* w^2 */
  double *p00, *p01, *p10, *p11;   /* exp(A h), for the half step h */
} Modes;

/* A line of fixed ends: mode n at n f0, shape sin(n pi p), loss gamma a
 * second for every mode, stepped h seconds at a time. */
void modes_line(Modes* m, double f0, double gamma, int most, double rate, double h);
void modes_free(Modes* m);

/* One step of h under a force F at place p: each mode is pushed by its own
 * shape there, 2 sin(n pi p) F, the exact projection of F delta(s - p). */
void modes_step(Modes* m, double force, double at);

/* A kick: every mode's velocity changes by its shape at p times `impulse`
 * (a force times the step it acts over), its place not at all. */
void modes_kick(Modes* m, double impulse, double at);

/* The field and its velocity at place p: sum of each mode times its shape. */
double modes_at(const Modes* m, double p);
double modes_velocity_at(const Modes* m, double p);

#endif
