/* Verify per-helper-type state keying preserves state across structural
 * edits. Patch A has [phasor, lpf, delay]. Patch B inserts a new wave()
 * call at the top. Under the old flat-counter scheme, adding any
 * stateful call shifts every subsequent call's state slot -> lpf and
 * delay would reset. Under per-type keying only wave() gets new state;
 * phasor, lpf, delay continue from their old state. */

#include "check.h"

static const char* PatchA =
  "let sig = phasor(110) * 2 - 1\n"
  "let filtered = sig |> lpf(800, 0.5)\n"
  "let echoed = filtered |> delay(0.2, 0.5)\n"
  "echoed * 0.3\n";

/* B inserts a wave() at the top. Historically this would shift lpf and
 * delay's state slots. With per-type keying, only the wave region is
 * new; phasor/lpf/delay migrate. */
static const char* PatchB =
  "let m = wave(2, [200, 400, 600, 800])\n"
  "let sig = phasor(110 + m * 0.0001) * 2 - 1\n"
  "let filtered = sig |> lpf(800, 0.5)\n"
  "let echoed = filtered |> delay(0.2, 0.5)\n"
  "echoed * 0.3\n";

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

static void dump(const char* what, Generated* g) {
  printf("  %s regions:", what);
  for (int i = 0; i < g->nregions; i++)
    printf(" %s#%d@%ld[+%ld]", g->regions[i].type_name, g->regions[i].per_type_idx,
           g->regions[i].offset, g->regions[i].size);
  printf("\n");
}

static Region* find(Generated* g, const char* type, int idx) {
  for (int i = 0; i < g->nregions; i++)
    if (!strcmp(g->regions[i].type_name, type) && g->regions[i].per_type_idx == idx)
      return &g->regions[i];
  CHECK(0, "no region %s#%d", type, idx);
  static Region none = {.type_name = "", .scope = "", .owner = ""};
  return &none;
}

TEST(pertype_keying) {
  /* Regions produced by codegen should group by helper type. */
  Generated ra = layout(PatchA), rb = layout(PatchB);
  dump("A", &ra);
  dump("B", &rb);

  /* lpf and delay regions (idx 0) must have the same size in A and B,
   * so they are eligible for migration. */
  CHECK(find(&ra, "lpf", 0)->size == find(&rb, "lpf", 0)->size, "lpf sizes differ");
  CHECK(find(&ra, "delay", 0)->size == find(&rb, "delay", 0)->size, "delay sizes differ");
  CHECK(find(&ra, "phasor", 0)->size == find(&rb, "phasor", 0)->size, "phasor sizes differ");

  Voice* v = must_load(PatchA, 48000.0);
  if (!v) return;
  /* Run long enough to fully load the delay buffer (>0.2s). */
  double last_a = 0.0, l, r;
  for (int i = 1; i <= 24000; i++) {
    tick_at(v, (double)i / 48000.0, &l, &r);
    last_a = l;
  }
  printf("  lastA = %g\n", last_a);

  /* Peek at the phasor slot before reload — it must survive. */
  long old_phasor = find(&ra, "phasor", 0)->offset;
  long old_delay = find(&ra, "delay", 0)->offset;
  double* pool = ((VoiceHeader*)v->cur.state)->pool;
  double phasor_before = pool[old_phasor];
  double delay_buf = pool[old_delay + 100];    /* an arbitrary slot in the delay buffer */
  printf("  phasor before = %g  delay-buf@100 = %g\n", phasor_before, delay_buf);
  CHECK(phasor_before != 0.0, "phasor slot should be non-zero after ticks");
  CHECK(delay_buf != 0.0, "delay buffer should contain audio");

  reload(v, PatchB, 48000.0);
  long new_phasor = find(&rb, "phasor", 0)->offset;
  long new_delay = find(&rb, "delay", 0)->offset;
  double* pool2 = ((VoiceHeader*)v->cur.state)->pool;
  NEAR(pool2[new_phasor], phasor_before, 1e-9, "phasor state should migrate");
  NEAR(pool2[new_delay + 100], delay_buf, 1e-9, "delay buffer should migrate");

  /* The first tick after reload shouldn't be catastrophically different
   * — the delay buffer still holds the pre-reload audio, the phasor
   * still has its phase. A small jump from the newly-initialised wave()
   * is expected. */
  tick_at(v, 24001.0 / 48000.0, &l, &r);
  printf("  firstB = %g\n", l);
  CHECK(fabs(l - last_a) < 0.2, "expected smooth continuation; got a jump from %g to %g", last_a, l);
}
