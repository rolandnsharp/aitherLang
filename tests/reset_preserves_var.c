/* resetPool used to zero the entire DSP pool. That clobbered state
 * storage too — meaning user-authored auto-fade-in patterns (e.g.
 * `$startTime = -1; $startTime = if $startTime < 0 then t else $startTime`)
 * would re-trigger from scratch every time NaN tripped a reset. Now
 * the reset walks the regions and skips the "var" type, so user-named
 * state survives. (Region type_name is still "var" for layout-stability
 * across the pre-/post-sigil migration boundary; the sigil is a parser
 * change, not a codegen one.) */

#include "check.h"

static const char* Patch =
  "def step():\n"
  "  $counter = 0.0\n"
  "  $counter = $counter + 1.0\n"
  "  $counter\n"
  "let c = step()\n"
  "c\n";

TEST(reset_preserves_var) {
  Voice* v = must_load(Patch, 48000.0);
  if (!v) return;
  double last = 0.0, l, r;
  for (int i = 1; i <= 100; i++) {
    tick_at(v, (double)i / 48000.0, &l, &r);
    last = l;
  }
  CHECK(last == 100.0, "expected counter=100 after 100 ticks, got %g", last);

  /* Locate the (single) var region and confirm its value slot really holds 100. */
  long off = -1;
  for (int i = 0; i < v->cur.nregions; i++)
    if (!strcmp(v->cur.regions[i].type_name, "var")) off = v->cur.regions[i].offset;
  CHECK(off >= 0, "test setup failed: no var region");
  if (off < 0) return;
  double* pool = ((VoiceHeader*)v->cur.state)->pool;
  CHECK(pool[off] == 1.0, "var inited-flag should be set");
  CHECK(pool[off + 1] == 100.0, "var value slot should be 100");

  /* Reset the pool. The var region must NOT be zeroed. */
  voice_reset_pool(v);
  CHECK(pool[off] == 1.0, "var inited-flag didn't survive the reset");
  CHECK(pool[off + 1] == 100.0, "var value didn't survive the reset");

  tick_at(v, 101.0 / 48000.0, &l, &r);
  CHECK(l == 101.0, "counter should continue past reset, got %g", l);
}
