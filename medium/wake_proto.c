/* Milestone 1's question: a wake coupled to a modal string, stepped as one
 * system. The physics is tests/physics/modes_wake.aither's: a 110 Hz string,
 * fixed ends, loss 0.22 a second, 60 modes; a van der Pol wake shedding near
 * the 3rd harmonic, reading the string's velocity at 0.37 (a round's worth,
 * / f0) and pushing it there by q * 0.01 * f0^2.
 *
 *   wake_proto RATE SECONDS [split|lag] > out.raw     float pairs: heard at 0.13, q
 *
 * split: Strang. The string goes half a step under the wake's push, the wake
 * a whole step (RK4) on the string's velocity at that midpoint, the string
 * the second half under the wake's new push: second order in the step.
 * kick: Strang as the springs have it. The string turns half a step on its
 * own, the wake takes its step on the string's velocity there and kicks the
 * string by its push, averaged over the step, the string turns the other
 * half. The scheme the core takes for everything.
 * lag: the string a whole step under the push of the sample before, the wake
 * on the velocity of the sample before, as aither couples them today. */
#include "modes.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double f0 = 110, shed = 2 * M_PI * 110 * 3.05;

static void wake_rate(double q, double r, double moving, double* dq, double* dr) {
  *dq = r;
  *dr = -q * shed * shed + shed * 0.1 * (1 - q * q) * r + moving * 0.5 * shed * shed;
}
static int euler;            /* the wake stepped as aither steps a law on plain state: velocity, then place */
static void wake_step(double* q, double* r, double moving, double h) {   /* RK4, the string's velocity held */
  if (euler) {
    double dq, dr;
    wake_rate(*q, *r, moving, &dq, &dr);
    *r += h * dr;
    *q += h * *r;
    return;
  }
  double k1q, k1r, k2q, k2r, k3q, k3r, k4q, k4r;
  wake_rate(*q, *r, moving, &k1q, &k1r);
  wake_rate(*q + 0.5 * h * k1q, *r + 0.5 * h * k1r, moving, &k2q, &k2r);
  wake_rate(*q + 0.5 * h * k2q, *r + 0.5 * h * k2r, moving, &k3q, &k3r);
  wake_rate(*q + h * k3q, *r + h * k3r, moving, &k4q, &k4r);
  *q += h / 6 * (k1q + 2 * k2q + 2 * k3q + k4q);
  *r += h / 6 * (k1r + 2 * k2r + 2 * k3r + k4r);
}

int main(int argc, char** argv) {
  double rate = atof(argv[1]), seconds = atof(argv[2]), dt = 1 / rate;
  int split = argc < 4 || (strcmp(argv[3], "lag") && strcmp(argv[3], "lag-euler"));
  int kick = argc >= 4 && !strcmp(argv[3], "kick");
  euler = argc >= 4 && !strcmp(argv[3], "lag-euler");
  Modes m;
  modes_line(&m, f0, 0.002 * f0, 60, rate, split ? dt / 2 : dt);
  double q = 0.1, r = 0;
  long n = (long)(seconds * rate);
  float* out = malloc(sizeof(float) * 2 * n);
  for (long i = 0; i < n; i++) {
    if (kick) {                  /* Strang: the string turns half alone, the wake steps and kicks it, the string turns half */
      modes_step(&m, 0, 0.37);
      double before = q;
      wake_step(&q, &r, modes_velocity_at(&m, 0.37) / f0, dt);
      modes_kick(&m, 0.5 * (before + q) * 0.01 * f0 * f0 * dt, 0.37);
      modes_step(&m, 0, 0.37);
    } else if (split) {
      modes_step(&m, q * 0.01 * f0 * f0, 0.37);
      wake_step(&q, &r, modes_velocity_at(&m, 0.37) / f0, dt);
      modes_step(&m, q * 0.01 * f0 * f0, 0.37);
    } else {
      double moving = modes_velocity_at(&m, 0.37) / f0, push = q * 0.01 * f0 * f0;
      wake_step(&q, &r, moving, dt);
      modes_step(&m, push, 0.37);
    }
    out[2 * i] = (float)modes_at(&m, 0.13);
    out[2 * i + 1] = (float)q;
  }
  fwrite(out, sizeof(float), 2 * n, stdout);
  return 0;
}
