/* State is keyed by the statement a stateful call sits in, so adding
 * one to a play or let leaves every other statement's state alone.
 * Before, keys were per-type counters over the whole file: a `prev`
 * added in an early play shifted the `prev` slots of every later one,
 * and physical models fed each other's velocities and blew up. */

#include "check.h"

static const char* A =
  "play a:\n"
  "  let x = prev(sin(TAU * phasor(3)))\n"
  "  x * 0.1\n"
  "\n"
  "play b:\n"
  "  let y = prev(sin(TAU * phasor(5)))\n"
  "  let z = prev(y)\n"
  "  z * 0.1\n";

/* Adds a prev() at the top of play a: under per-type counters, b's two
 * prev regions would move to indices 2 and 3 and pick up a's state. */
static const char* B =
  "play a:\n"
  "  let w = prev(0.25)\n"
  "  let x = prev(sin(TAU * phasor(3)))\n"
  "  x * 0.1 + w * 0\n"
  "\n"
  "play b:\n"
  "  let y = prev(sin(TAU * phasor(5)))\n"
  "  let z = prev(y)\n"
  "  z * 0.1\n";

/* The regions Nim's compile(src, "", 48000) gave (in the arena, which
 * the test's process keeps). */
static Generated layout(const char* src) {
  char err[4096];
  Generated g = {0};
  if (compile_full(src, &g, err, sizeof err) != 0) {
    CHECK(0, "doesn't compile: %s", err);
    g.nregions = 0;
  }
  return g;
}

static Region* find(Generated* g, const char* scope, const char* type, int i) {
  for (int k = 0; k < g->nregions; k++) {
    Region* r = &g->regions[k];
    if (!strcmp(r->scope, scope) && !strcmp(r->type_name, type) && r->scope_idx == i) return r;
  }
  CHECK(0, "no region %s %s#%d", scope, type, i);
  static Region none = {.type_name = "", .scope = "", .owner = ""};
  return &none;
}

static const char* kept[] = {"play b/let y", "play b/let z", "play a/let x"};

TEST(scope_keying) {
  Generated ra = layout(A), rb = layout(B);
  Voice* v = must_load(A, 48000.0);
  if (!v) return;
  double l, r;
  for (int i = 1; i <= 4800; i++) tick_at(v, (double)i / 48000.0, &l, &r);
  double* pool = ((VoiceHeader*)v->cur.state)->pool;
  double before[3][64];
  long sizes[3];
  for (int n = 0; n < 3; n++) {
    Region* g = find(&ra, kept[n], "var", 0);
    sizes[n] = g->size < 64 ? g->size : 64;
    for (long k = 0; k < sizes[n]; k++) before[n][k] = pool[g->offset + k];
  }

  reload(v, B, 48000.0);
  double* pool2 = ((VoiceHeader*)v->cur.state)->pool;
  for (int n = 0; n < 3; n++) {
    Region* g = find(&rb, kept[n], "var", 0);
    CHECK(g->size == sizes[n], "%s changed size: %ld vs %ld", kept[n], g->size, sizes[n]);
    for (long k = 0; k < g->size && k < sizes[n]; k++)
      CHECK(pool2[g->offset + k] == before[n][k], "%s lost its state across the edit", kept[n]);
  }
  /* The new call starts fresh. */
  Region* w = find(&rb, "play a/let w", "var", 0);
  for (long k = 0; k < w->size; k++)
    CHECK(pool2[w->offset + k] == 0.0, "play a/let w doesn't start fresh: slot %ld is %g", k,
          pool2[w->offset + k]);
}
