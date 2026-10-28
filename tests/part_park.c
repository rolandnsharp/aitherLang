/* A parked part is silent and its body is skipped: its state freezes
 * where it was and resumes on unpark. (The 2026-10-10 crackle was the
 * engine underrunning on 21 muted parts that still computed.) */

#include "check.h"

#define PATCH_WITH(scale)                                                      \
  "play count:\n"                                                              \
  "  $n = 0\n"                                                                 \
  "  $n = $n + 1\n"                                                            \
  "  $n\n"                                                                     \
  "play other:\n"                                                              \
  "  $m = 0\n"                                                                 \
  "  $m = $m + 1\n"                                                            \
  "  $m * " scale "\n"                                                         \
  "count + other\n"

TEST(part_park) {
  Voice* v = must_load(PATCH_WITH("1000"), 48000.0);
  if (!v) return;
  CHECK(v->cur.nparts == 2 && !strcmp(v->cur.part_names[0], "count") &&
          !strcmp(v->cur.part_names[1], "other"),
        "parts aren't count, other");
  double l, r;
  for (int i = 0; i < 5; i++) tick_at(v, (double)i / 48000.0, &l, &r);
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 6006.0, "before parking: %g", l);

  /* Park `count`: it contributes 0 and stops counting; `other` goes on. */
  v->part_parked[0] = 1.0;
  for (int i = 0; i < 10; i++) tick_at(v, 0.0, &l, &r);
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 17000.0, "parked: %g", l);
  CHECK(v->part_out[0] == 0.0 && v->part_out[1] == 0.0, "a parked part's out is %g, %g",
        v->part_out[0], v->part_out[1]);

  /* Unpark: count resumes from 6, not from 0 and not from 17. */
  v->part_parked[0] = 0.0;
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 7.0 + 18000.0, "resumed: %g", l);

  /* A hot reload keeps the park by name. */
  v->part_parked[1] = 1.0;
  reload(v, PATCH_WITH("100"), 48000.0);
  CHECK(v->part_parked[0] == 0.0 && v->part_parked[1] == 1.0, "parks after reload: %g, %g",
        v->part_parked[0], v->part_parked[1]);
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 8.0, "after reload: %g", l);
}
