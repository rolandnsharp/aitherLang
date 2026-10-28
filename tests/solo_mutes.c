/* solo X must mute the other voices, not stop them. The pre-fix
 * behaviour faded other voices' fade gain to 0 and then marked them
 * inactive ([stopped]), which `unmute` couldn't revive — the operator
 * had to re-`send` and lose running state. Now `solo X` mutes the
 * others (they fade to silence and run on, recoverable) and wakes X.
 *
 * Test path: write two trivial patches to a temp dir, load each via
 * engine_load, call engine_solo, read the voice list. */

#define _GNU_SOURCE
#include <unistd.h>
#include "check.h"

/* What `list` prints. */
static char* listing(void) {
  VoiceSnap* v;
  int n = engine_voices(&v);
  char* s = cli_voice_list(midi_last_connect_info(), midi_subscription_active(), v, n);
  engine_free_voices(v, n);
  return s;
}

static int write_file(const char* path, const char* text) {
  FILE* f = fopen(path, "w");
  if (!f) return -1;
  fputs(text, f);
  fclose(f);
  return 0;
}

/* A command's error fails the test; it says which. */
static void ok(char* err, const char* what) {
  CHECK(err == NULL, "%s: %s", what, err);
  free(err);
}

TEST(solo_mutes) {
  const char* tmp = getenv("TMPDIR");
  char dir[4096], path_a[4200], path_b[4200];
  snprintf(dir, sizeof dir, "%s/aither_test_XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(dir)) { CHECK(0, "mkdtemp %s failed", dir); return; }
  snprintf(path_a, sizeof path_a, "%s/aither_test_solo_a.aither", dir);
  snprintf(path_b, sizeof path_b, "%s/aither_test_solo_b.aither", dir);
  if (write_file(path_a, "play tone:\n  sin(TAU * phasor(440)) * 0.1\ntone\n") ||
      write_file(path_b, "play tone:\n  sin(TAU * phasor(220)) * 0.1\ntone\n")) {
    CHECK(0, "can't write the patches in %s", dir);
    return;
  }

  /* Load both. fade_in=0 → the fade gain starts at 1.0. */
  ok(engine_load(path_a, 0.0), "load a");
  ok(engine_load(path_b, 0.0), "load b");

  /* Sanity: both should be playing. */
  char* s = listing();
  CHECK(strstr(s, "aither_test_solo_a [playing]"), "before solo, voice A should be [playing]:\n%s", s);
  CHECK(strstr(s, "aither_test_solo_b [playing]"), "before solo, voice B should be [playing]:\n%s", s);
  free(s);

  /* Solo A. */
  ok(engine_solo("aither_test_solo_a", 0.0), "solo a");
  s = listing();
  CHECK(strstr(s, "aither_test_solo_a [playing]"), "after solo, voice A should be [playing]:\n%s", s);
  CHECK(strstr(s, "aither_test_solo_b [muted]"), "after solo, voice B should be [muted] not [stopped]:\n%s", s);
  CHECK(!strstr(s, "[stopped]"), "no voice should be [stopped] after solo:\n%s", s);
  free(s);

  /* Unmute B — should return to [playing], proving the muted state is
   * recoverable. The pre-fix behaviour left B [stopped] and unmute had
   * no effect. */
  ok(engine_silence("aither_test_solo_b", 0, 0, 0.0), "unmute b");
  s = listing();
  CHECK(strstr(s, "aither_test_solo_b [playing]"), "after unmute, voice B should be back to [playing]:\n%s", s);
  free(s);

  /* Solo a different voice — A should now go to muted. */
  ok(engine_solo("aither_test_solo_b", 0.0), "solo b");
  s = listing();
  CHECK(strstr(s, "aither_test_solo_a [muted]"), "after solo B, A should be [muted]:\n%s", s);
  CHECK(strstr(s, "aither_test_solo_b [playing]"), "after solo B, B should be [playing]:\n%s", s);
  free(s);

  unlink(path_a);
  unlink(path_b);
  rmdir(dir);
}
