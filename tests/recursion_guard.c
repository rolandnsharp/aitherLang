/* A `def rec` past its block's deadline gives up: the block is silent,
 * every sample counted bad, `stuck` set and the def named for the log.
 * (What recursion computes is in tests/lang/state.aither.) */

#include "check.h"

TEST(recursion_guard) {
  char err[4096];
  Voice* v = load_voice("def rec forever(x): forever(x + 1)\n"
                        "forever(0)\n",
                        "patches/stuck.aither", 48000.0, err, sizeof err);
  CHECK(v, "doesn't compile: %s", err);
  if (!v) return;
  VoiceHeader* hdr = v->cur.state;
  hdr->fade_gain = 1.0;
  hdr->in_buf = NULL;
  hdr->deadline = 1e-9;                /* long gone */
  double l[16], r[16];
  voice_tick_block(v, 16, l, r);
  CHECK(hdr->stuck == 1, "stuck %ld", hdr->stuck);
  CHECK(hdr->bad == 16, "bad %ld", hdr->bad);
  for (int i = 0; i < 16; i++)
    CHECK(l[i] == 0.0 && r[i] == 0.0, "sample %d: %g, %g", i, l[i], r[i]);
  CHECK(hdr->stuck_at && !strcmp(hdr->stuck_at, "forever (patches/stuck.aither:1)"), "stuck at %s",
        hdr->stuck_at ? hdr->stuck_at : "(nil)");
}
