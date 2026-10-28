/* What sines cost, and how far the compiler's fast forms stray from them.
 *
 *   make bench                     every shape, 10 s each
 *   tests/bench [seconds] [shape ...]
 *
 * Each shape is a bank of 64 partials, the way a patch writes them:
 * steady sines, a sine of the clock, slow drift, vibrato, a chirp, FM at
 * audio rate, and the bank without its sines (the cost of everything
 * else). Each is compiled twice: as the compiler does by default, and
 * with AITHER_TURN=0, every sine computed by sin(), the reference. For
 * both it reports nanoseconds a sample on TCC's code and on the cc -O2
 * build (what a live voice plays once it lands), and the largest
 * difference between the two outputs over the whole render. The
 * compiler reads AITHER_TURN once a process, so each compile runs in a
 * child of its own. */

#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../voice.h"

extern const char aither_stdlib[];

typedef struct { const char* name; const char* src; } Shape;

static const Shape shapes[] = {
  {"steady",   "sum(64, n => sin(TAU * phasor(55 * n))) / 64\n"},
  {"clock",    "sum(64, n => sin(t * TAU * 55 * n + n)) / 64\n"},
  {"drift",    "sum(64, n => sin(TAU * phasor(55 * n * (1 + 0.006 * sin(t * n / 7))))) / 64\n"},
  {"vibrato",  "sum(64, n => sin(TAU * phasor(55 * n * (1 + 0.01 * sin(TAU * phasor(5.5)))))) / 64\n"},
  {"chirp",    "sum(64, n => sin(TAU * phasor(20 * n * pow(50, t / 10 - floor(t / 10))))) / 64\n"},
  {"fm_audio", "sum(64, n => sin(TAU * phasor(55 * n * (1 + 0.5 * sin(TAU * phasor(110 * n)))))) / 64\n"},
  {"no_sines", "sum(64, n => phasor(55 * n)) / 64\n"},
};

enum { SR = 48000, BLOCK = 512 };

/* A voice of `src` with AITHER_TURN as given ("" leaves it unset). */
static Voice* load(const char* src, const char* turn) {
  if (*turn) setenv("AITHER_TURN", turn, 1);
  else unsetenv("AITHER_TURN");
  char err[4096];
  int kind;
  Prepared* p = voice_prepare(aither_stdlib, src, "<inline>", "", SR, 0, 1, &kind, err, sizeof err);
  unsetenv("AITHER_TURN");
  if (!p) {
    fprintf(stderr, "bench: doesn't compile: %s\n%s", err, src);
    return NULL;
  }
  Voice* v = voice_new();
  voice_commit(v, p);
  VoiceHeader* h = v->cur.state;
  h->fade_gain = 1.0;
  return v;
}

static int optimize(Voice* v) {
  void* lib;
  TickFn fn;
  char err[2048];
  if (voice_compile_optimized(v->cur.csrc, v->cur.state_size, 0, &lib, &fn, err, sizeof err) != 0) {
    fprintf(stderr, "bench: cc -O2 failed: %s\n", err);
    return 0;
  }
  return voice_upgrade(v, v->gen, lib, fn);
}

/* n samples a block at a time, as the engine runs a voice; its left side
 * kept in out (if given). Seconds of wall clock. */
static double run(Voice* v, long n, double* out) {
  double l[BLOCK], r[BLOCK];
  double t0 = voice_mono_now();
  for (long i = 0; i < n; i += BLOCK) {
    long k = n - i < BLOCK ? n - i : BLOCK;
    voice_tick_block(v, k, l, r);
    if (out) memcpy(out + i, l, k * sizeof(double));
  }
  return voice_mono_now() - t0;
}

int main(int argc, char** argv) {
  double seconds = argc > 1 ? atof(argv[1]) : 10.0;
  if (seconds <= 0) seconds = 10.0;
  long n = (long)(seconds * SR);
  /* shared with the children that render into them */
  double* ref = mmap(NULL, n * sizeof(double), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  double* got = mmap(NULL, n * sizeof(double), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  printf("%.0f s at %d Hz, 64 partials; ns a sample (TCC / cc -O2), and the largest difference\n"
         "from computing every sine (AITHER_TURN=0)\n\n", seconds, SR);
  printf("%-10s %22s %22s %12s\n", "shape", "default", "every sine", "max diff");
  for (size_t s = 0; s < sizeof shapes / sizeof *shapes; s++) {
    int wanted = argc <= 2;
    for (int k = 2; k < argc; k++) wanted |= strcmp(argv[k], shapes[s].name) == 0;
    if (!wanted) continue;
    /* [default, every sine][tcc, -O2], filled by the children */
    double* ns = mmap(NULL, 4 * sizeof(double), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    for (int mode = 0; mode < 2; mode++) {
      pid_t pid = fork();
      if (pid == 0) {
        for (int build = 0; build < 2; build++) {
          Voice* v = load(shapes[s].src, mode ? "0" : "");
          if (!v || (build && !optimize(v))) _exit(1);
          ns[mode * 2 + build] = run(v, n, build ? (mode ? ref : got) : NULL) / (double)n * 1e9;
        }
        _exit(0);
      }
      int status;
      waitpid(pid, &status, 0);
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 1;
    }
    double diff = 0.0;
    for (long i = 0; i < n; i++) {
      double d = fabs(got[i] - ref[i]);
      if (d > diff || d != d) diff = d;
    }
    char a[32], b[32];
    snprintf(a, sizeof a, "%.0f / %.0f", ns[0], ns[1]);
    snprintf(b, sizeof b, "%.0f / %.0f", ns[2], ns[3]);
    munmap(ns, 4 * sizeof(double));
    printf("%-10s %22s %22s %12.2e\n", shapes[s].name, a, b, diff);
    fflush(stdout);
  }
  return 0;
}
