/* State regions are owned by the def they sit in (and, for `$state`,
 * by the state's name). Replacing one physical model by another of the
 * same shape starts the new one clean; editing a def's body keeps its
 * state; the renamed-statement fallback also respects the owner.
 * (2026-10-11: `stick` ($x,$dx x3) swapped for `bar` ($bv,$bx x3)
 * inherited the old displacement and `try` showed +24 dBFS.) */

#include "check.h"

#define STICK                                                                  \
  "def stick(force):\n"                                                        \
  "  sum(3, n =>\n"                                                            \
  "    $x = 0.0\n"                                                             \
  "    $dx = 0.0\n"                                                            \
  "    $dx = $dx + (force * 1000 - 50 * $dx - 400000 * n * $x) * dt\n"         \
  "    $x = $x + $dx * dt\n"                                                   \
  "    $x)\n"
#define BAR_WITH(force)                                                        \
  "def bar(force):\n"                                                          \
  "  sum(3, n =>\n"                                                            \
  "    $bv = 0.0\n"                                                            \
  "    $bx = 0.0\n"                                                            \
  "    $bv = $bv + (force * " force " - 20 * $bv - 900000 * n * $bx) * dt\n"   \
  "    $bx = $bx + $bv * dt\n"                                                 \
  "    $bv)\n"
#define BAR BAR_WITH("4000")
#define BAR2 BAR_WITH("3000")       /* an edit inside bar */
#define DRIVE "let trig = if t < dt then 1 else 0\n"

/* The regions Nim's compile(src, "", 48000) gave (in the arena, which
 * the test's process keeps); nregions -1 when it doesn't compile. */
static Generated layout(const char* src) {
  char err[4096];
  Generated g = {.nregions = -1};
  if (compile_full(src, &g, err, sizeof err) != 0) {
    CHECK(0, "doesn't compile: %s", err);
    g.nregions = -1;
  }
  return g;
}

/* The pool's slots of every `var` region whose owner starts with
 * `owner`, in order; their count in *n. */
static double* slots(Voice* v, Generated* g, const char* owner, int* n) {
  double* pool = ((VoiceHeader*)v->cur.state)->pool;
  double* out = calloc(4096, sizeof *out);
  *n = 0;
  for (int i = 0; i < g->nregions; i++) {
    Region* r = &g->regions[i];
    if (strcmp(r->type_name, "var") || strncmp(r->owner, owner, strlen(owner))) continue;
    for (long k = 0; k < r->size && *n < 4096; k++) out[(*n)++] = pool[r->offset + k];
  }
  return out;
}

static int var_regions(Generated* g, const char* owner) {
  int n = 0;
  for (int i = 0; i < g->nregions; i++)
    n += !strcmp(g->regions[i].type_name, "var") &&
         !strncmp(g->regions[i].owner, owner, strlen(owner));
  return n;
}

/* Nim's seq ==: same length, every element ==. */
static int same(const double* a, int na, const double* b, int nb) {
  if (na != nb) return 0;
  for (int k = 0; k < na; k++)
    if (a[k] != b[k]) return 0;
  return 1;
}

static void run(Voice* v, int n) {
  double l, r;
  for (int i = 0; i < n; i++) tick_at(v, (double)i / 48000.0, &l, &r);
}

TEST(migration_def_identity) {
  /* Owners are recorded: bar's state is `bar/$bv`, `bar/$bx`. */
  Generated rb = layout(BAR DRIVE "play hit:\n  bar(trig)\n");
  int bv = 0, bx = 0;
  for (int i = 0; i < rb.nregions; i++) {
    if (strcmp(rb.regions[i].type_name, "var")) continue;
    bv |= !strcmp(rb.regions[i].owner, "bar/$bv");
    bx |= !strcmp(rb.regions[i].owner, "bar/$bx");
  }
  CHECK(bv && bx, "owners bar/$bv and bar/$bx aren't recorded");
  CHECK(var_regions(&rb, "bar") == 6, "%d var regions owned by bar, wanted 6",
        var_regions(&rb, "bar"));

  /* 1. stick -> bar in the same play: bar starts from zero. */
  Voice* v = must_load(STICK DRIVE "play hit:\n  stick(trig)\n", 48000.0);
  if (!v) return;
  run(v, 2000);
  Generated rs = layout(STICK DRIVE "play hit:\n  stick(trig)\n");
  int n;
  double* stick_state = slots(v, &rs, "stick", &n);
  int moving = 0;
  for (int k = 0; k < n; k++) moving += fabs(stick_state[k]) > 1e-6 && stick_state[k] != 1.0;
  CHECK(moving >= 3, "stick should be ringing: %d of %d slots move", moving, n);

  reload(v, BAR DRIVE "play hit:\n  bar(trig)\n", 48000.0);
  double* bar_state = slots(v, &rb, "bar", &n);
  for (int k = 0; k < n; k++)
    CHECK(bar_state[k] == 0.0, "bar inherited stick's state: slot %d is %g", k, bar_state[k]);

  /* 2. An edit inside bar keeps bar's state (same owner, same shape). */
  run(v, 2000);
  int nb;
  double* bar_before = slots(v, &rb, "bar", &nb);
  reload(v, BAR2 DRIVE "play hit:\n  bar(trig)\n", 48000.0);
  Generated rb2 = layout(BAR2 DRIVE "play hit:\n  bar(trig)\n");
  double* s = slots(v, &rb2, "bar", &n);
  CHECK(same(s, n, bar_before, nb), "bar's state should survive an edit");

  /* 3. Renamed statement, same def: the fallback carries bar's state over. */
  reload(v, BAR2 DRIVE "play hit2:\n  bar(trig)\n", 48000.0);
  Generated rb3 = layout(BAR2 DRIVE "play hit2:\n  bar(trig)\n");
  s = slots(v, &rb3, "bar", &n);
  CHECK(same(s, n, bar_before, nb), "renamed play should keep bar's state");

  /* 4. Renamed statement AND a different def of the same shape: clean. */
  reload(v, STICK DRIVE "play hit3:\n  stick(trig)\n", 48000.0);
  Generated rs3 = layout(STICK DRIVE "play hit3:\n  stick(trig)\n");
  s = slots(v, &rs3, "stick", &n);
  for (int k = 0; k < n; k++)
    CHECK(s[k] == 0.0, "stick inherited bar's state via the fallback: slot %d is %g", k, s[k]);
}
