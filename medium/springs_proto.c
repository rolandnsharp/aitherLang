/* Two exact springs coupled, as tests/physics/springs_*.aither: 440 Hz each,
 * modes 440 and 466.16, lossless, spring 1 kicked. Each spring is stepped
 * exactly under a force held over the step (modes.c's rest point), coupled
 * either with the other's place from the sample before (lag) or Strang
 * split (half, exchange, half). Prints the level of the two modes after 2 s
 * against the start: a lossless system must hold it.
 *   springs_proto RATE lag|split */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { double a, v, p00, p01, p10, p11, w2; } S;
static void init(S* s, double w, double h) {
  s->a = s->v = 0; s->w2 = w * w;
  s->p00 = cos(w * h); s->p01 = sin(w * h) / w; s->p10 = -w * sin(w * h); s->p11 = cos(w * h);
}
static void step(S* s, double force) {
  double rest = force / s->w2, a = s->a - rest, v = s->v;
  s->a = s->p00 * a + s->p01 * v + rest; s->v = s->p10 * a + s->p11 * v;
}
int main(int argc, char** argv) {
  double rate = atof(argv[1]), dt = 1 / rate, w = 2 * M_PI * 440, k = (pow(2 * M_PI * 466.16, 2) - w * w) / 2;
  int split = !strcmp(argv[2], "split");
  S x1, x2; init(&x1, w, split ? dt / 2 : dt); init(&x2, w, split ? dt / 2 : dt);
  x1.v = w;
  long n = (long)(2 * rate);
  double e0 = 0, e1 = 0;
  for (long i = 0; i < n; i++) {
    double f1 = -k * (x1.a - x2.a), f2 = -k * (x2.a - x1.a);
    if (split) {                 /* Strang: each turns half alone, the coupling kicks, each turns half */
      step(&x1, 0); step(&x2, 0);
      double c = -k * (x1.a - x2.a);
      x1.v += dt * c; x2.v -= dt * c;
      step(&x1, 0); step(&x2, 0);
    } else { step(&x1, f1); step(&x2, f2); }
    if (argc > 3) { float o[2] = {(float)x1.a, (float)x2.a}; fwrite(o, sizeof o, 1, stdout); }
    double e = 0.5 * (x1.v * x1.v + x2.v * x2.v) + 0.5 * w * w * (x1.a * x1.a + x2.a * x2.a) + 0.5 * k * pow(x1.a - x2.a, 2);
    if (i < rate * 0.1) e0 = fmax(e0, e); else if (i > n - rate * 0.1) e1 = fmax(e1, e);
  }
  if (argc <= 3) printf("%-5s %6.0f Hz: energy %+.4f dB over 2 s\n", argv[2], rate, 10 * log10(e1 / e0));
}
