/* Item 3 test: play blocks compile, partNames populate, per-part gain
 * works, stereo bodies produce independent L/R channels, and hot-reload
 * preserves part gains by name. */

#include "check.h"

/* Three plays: scalar, scalar-but-different, and explicit stereo. The
 * final expression references them both as a sum (stereo broadcast)
 * and via indexing (check channel picks work). */
static const char* PatchA =
  "play a:\n"
  "  sin(TAU * phasor(440)) * 0.3\n"
  "play b:\n"
  "  [sin(TAU * phasor(220)) * 0.3, sin(TAU * phasor(221)) * 0.3]\n"
  "a + b\n";

/* Changed slightly, keeping both part names. */
static const char* PatchB =
  "play a:\n"
  "  sin(TAU * phasor(441)) * 0.3\n"
  "play b:\n"
  "  [sin(TAU * phasor(219)) * 0.3, sin(TAU * phasor(222)) * 0.3]\n"
  "a + b\n";

TEST(playblocks) {
  Voice* v = must_load(PatchA, 48000.0);
  if (!v) return;
  CHECK(v->cur.nparts == 2 && !strcmp(v->cur.part_names[0], "a") &&
          !strcmp(v->cur.part_names[1], "b"),
        "parts aren't a, b (%d of them)", v->cur.nparts);
  for (int i = 0; i < v->cur.nparts; i++)
    CHECK(v->part_gains[i] == 1.0, "part %d's gain is %g", i, v->part_gains[i]);

  /* Play block b is stereo — expect L != R over a short run (220 Hz vs 221 Hz). */
  double delta_sq = 0.0, l, r;
  for (int i = 0; i < 4800; i++) {
    tick_at(v, (double)i / 48000.0, &l, &r);
    delta_sq += (l - r) * (l - r);
  }
  CHECK(delta_sq > 0.0, "stereo body should produce L != R over time");

  /* Muting play a should drop the overall level significantly. */
  for (int i = 0; i < 2; i++) v->part_gains[i] = 1.0;
  double full = 0.0, muted = 0.0;
  for (int i = 0; i < 4800; i++) {
    tick_at(v, (double)i / 48000.0, &l, &r);
    full = fmax(full, fmax(fabs(l), fabs(r)));
  }
  v->part_gains[0] = 0.0;              /* silence play a */
  for (int i = 0; i < 4800; i++) {
    tick_at(v, (double)(i + 10000) / 48000.0, &l, &r);
    muted = fmax(muted, fmax(fabs(l), fabs(r)));
  }
  CHECK(muted < full, "muting play a should reduce peak (full=%g muted=%g)", full, muted);

  /* Hot reload: part a's gain (set to 0.0 above) should persist. */
  reload(v, PatchB, 48000.0);
  CHECK(v->part_gains[0] == 0.0, "hot reload should preserve part 'a' gain of 0.0, got %g",
        v->part_gains[0]);
  CHECK(v->part_gains[1] == 1.0, "part 'b' gain should stay 1.0, got %g", v->part_gains[1]);
}
