/* The test harness: `TEST(name) { ... }` in any C file under tests/
 * registers a test, and tests/run runs every one (or those named), each
 * in its own process, so a test that loads voices into the engine's
 * table can't leave them for the next. CHECK and NEAR report the file,
 * line and message and let the test go on; a test passes with none
 * failed. */

#ifndef AITHER_CHECK_H
#define AITHER_CHECK_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine.h"
#include "../voice.h"
#include "../render.h"
#include "../analysis.h"
#include "../cli_output.h"
#include "../midi.h"
#include "../dsp.h"
#include "../codegen.h"
#include "../imports.h"

extern const char aither_stdlib[];    /* stdlib.c */
extern int check_failures;

void test_register(const char* name, void (*fn)(void));

#define TEST(name)                                                              \
  static void name(void);                                                       \
  __attribute__((constructor)) static void register_##name(void) {              \
    test_register(#name, name);                                                 \
  }                                                                             \
  static void name(void)

#define CHECK(cond, ...)                                                        \
  do {                                                                          \
    if (!(cond)) {                                                              \
      check_failures++;                                                         \
      fprintf(stderr, "  %s:%d: ", __FILE__, __LINE__);                         \
      fprintf(stderr, __VA_ARGS__);                                             \
      fputc('\n', stderr);                                                      \
    }                                                                           \
  } while (0)

/* |a - b| <= tol, saying both. */
#define NEAR(a, b, tol, what)                                                   \
  do {                                                                          \
    double near_a_ = (a), near_b_ = (b);                                        \
    CHECK(fabs(near_a_ - near_b_) <= (tol), "%s: %.17g, wanted %.17g (±%g)",   \
          what, near_a_, near_b_, (double)(tol));                               \
  } while (0)

/* A patch with the stdlib, compiled and committed to a new voice, as the
 * engine loads one; NULL with the message in err when it doesn't compile.
 * `tag` is its path, or "<inline>". */
Voice* load_voice(const char* src, const char* tag, double sr, char* err, int errlen);
/* The same, and a compile error fails the test (and gives NULL). */
Voice* must_load(const char* src, double sr);
/* A hot reload of `v` to `src`: prepare and commit. 0, or -1 on an error. */
int    reload(Voice* v, const char* src, double sr);
/* One sample at time t. */
void   tick_at(Voice* v, double t, double* l, double* r);
/* A patch with the stdlib through the compiler, inline at 48 kHz: its C
 * and layout in *g (in an arena the test's process keeps). 0, -1 for a
 * parse or import error, -2 for a compile error, with the message. */
int    compile_full(const char* src, Generated* g, char* err, int errlen);
/* The C a patch compiles to (malloc'd), or NULL with the error. */
char*  compile_csrc(const char* src, const char* tag, char* err, int errlen);
/* A patch's source rendered from t=0 for `seconds` at `sr`; 0, or the
 * error's kind with its message in err. */
int    render_src(const char* src, double seconds, int sr, Rendered* out, char* err, int errlen);
/* A file's contents (malloc'd), or NULL. */
char*  slurp(const char* path);

#endif
