/* How long a patch's -O2 build takes: notes/multicore/buildtime patch.aither
 *   (AITHER_CC_JOBS=1: one file, as before the parallel build) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "voice.h"
#include "render.h"
extern const char aither_stdlib[];
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char** argv) {
  FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  char* src = calloc(n + 1, 1); if (fread(src, 1, n, f) != (size_t)n) return 1; fclose(f);
  char err[4096]; int kind;
  Prepared* p = voice_prepare(aither_stdlib, src, argv[1], argv[1], 48000.0, 0, 1, &kind, err, sizeof err);
  if (!p) { fprintf(stderr, "%s\n", err); return 1; }
  Voice* v = voice_new(); voice_commit(v, p);
  double t0 = now();
  render_optimize(v);
  printf("%.1f s%s\n", now() - t0, v->cur.optimized ? "" : " (no -O2)");
  return 0;
}
