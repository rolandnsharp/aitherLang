/* pacer SECONDS [latency-ms]: read aither's float32 pairs from stdin at
 * 48 kHz, 512 frames a period, like a card would, and count the periods
 * that arrived after the card would have needed them. Silent. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char** argv) {
  double secs = argc > 1 ? atof(argv[1]) : 30, lat = (argc > 2 ? atof(argv[2]) : 50) / 1000.0;
  fcntl(0, F_SETPIPE_SZ, 4096);
  enum { P = 512 };
  static char buf[P * 8];
  const double per = P / 48000.0;
  long periods = (long)(secs / per), late = 0, wl = 0;
  double worst = 0, start = 0;
  for (long k = 0; k < periods; k++) {
    size_t got = 0;
    while (got < sizeof buf) {
      ssize_t r = read(0, buf + got, sizeof buf - got);
      if (r <= 0) { printf("pipe closed at period %ld\n", k); return 1; }
      got += r;
    }
    double t = now();
    if (k == 0) start = t;  /* the first period starts the card */
    double need = start + lat + k * per;
    if (t > need) {
      late++; wl++; if (t - need > worst) worst = t - need;
      start = t - k * per;   /* the card ran dry: it starts again from here */
    }
    if ((k + 1) % (long)(5.0 / per) == 0) { printf("  %3.0f s: %ld late\n", (k + 1) * per, wl); fflush(stdout); wl = 0; }
    /* the card takes the next period when it has played this one */
    double next = start + (k + 1) * per;
    while (now() < next) {
      struct timespec ts = {0, 200000};
      nanosleep(&ts, NULL);
    }
  }
  printf("%ld periods, %ld late, worst %.1f ms late\n", periods, late, worst * 1000);
  return 0;
}
