#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "voice.h"
#include "render.h"
extern const char aither_stdlib[];
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char** argv) {
  FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  char* src = calloc(n + 1, 1); fread(src, 1, n, f); fclose(f);
  int opt = atoi(argv[2]);
  char err[4096]; int kind;
  Prepared* p = voice_prepare(aither_stdlib, src, argv[1], argv[1], 48000.0, 0, 1, &kind, err, sizeof err);
  if (!p) { fprintf(stderr, "%s\n", err); return 1; }
  Voice* v = voice_new(); voice_commit(v, p);
  VoiceHeader* h = v->cur.state; h->fade_gain = 1.0;
  if (opt) { render_optimize(v); if (!v->cur.optimized) { fprintf(stderr, "no -O2\n"); return 1; } }
  static double l[512], r[512];
  long N = 48000 * 10;
  double t0 = now();
  for (long i = 0; i < N; i += 512) voice_tick_block(v, 512, l, r);
  double tb = now() - t0;
  t0 = now();
  double x, y;
  for (long i = 0; i < N; i++) voice_tick(v, (double)i / 48000.0, &x, &y);
  double ts = now() - t0;
  printf("%.3f %.3f\n", tb, ts);
  return 0;
}
