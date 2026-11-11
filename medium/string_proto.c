/* tests/physics' string_partials through modes.c: a 110 Hz line, fixed
 * ends, loss 0.22 a second, struck at 0.3 by a force of 1 for 0.5 ms,
 * heard at 0.13.   string_proto RATE SECONDS > out.raw   (float pairs) */
#include "modes.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
  double rate = atof(argv[1]), seconds = atof(argv[2]), dt = 1 / rate;
  Modes m;
  modes_line(&m, 110, 0.22, 100000, rate, dt / 2);
  long n = (long)(seconds * rate);
  for (long i = 0; i < n; i++) {
    double t = i * dt, f = t < 0.0005 ? 1.0 : 0.0;
    modes_step(&m, 0, 0.3);
    modes_kick(&m, f * dt, 0.3);
    modes_step(&m, 0, 0.3);
    float o[2] = {(float)modes_at(&m, 0.13), 0};
    fwrite(o, sizeof o, 1, stdout);
  }
  fprintf(stderr, "%d modes\n", m.n);
  return 0;
}
