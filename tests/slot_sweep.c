/* Voice slot leak regression. Pre-fix: stopped voices stayed in the
 * slots array even after their fade-out completed, so a session that
 * stopped 16 voices without re-sending hit "voice limit reached" on
 * the 17th send despite no voice being audible. Fix: engine_load sweeps
 * inactive slots out before checking the MAX_VOICES limit. */

#define _GNU_SOURCE
#include <unistd.h>
#include "check.h"

static const char* patch =
  "play tone:\n"
  "  sin(TAU * phasor(440)) * 0.05\n"
  "tone\n";

static char dir[4096];

/* `dir/name.aither` holding the patch, into path (4200 bytes); 0 on success. */
static int write_patch(const char* name, char* path) {
  snprintf(path, 4200, "%s/%s.aither", dir, name);
  FILE* f = fopen(path, "w");
  if (!f) return -1;
  fputs(patch, f);
  fclose(f);
  return 0;
}

TEST(slot_sweep) {
  const char* tmp = getenv("TMPDIR");
  snprintf(dir, sizeof dir, "%s/aither_test_XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(dir)) { CHECK(0, "mkdtemp %s failed", dir); return; }

  /* Load 12 voices with distinct names. The voice is named after the
   * file's basename (one .aither = one voice slot). */
  char paths[12][4200], name[64], *err;
  for (int i = 0; i < 12; i++) {
    snprintf(name, sizeof name, "aither_test_slot_%d", i);
    if (write_patch(name, paths[i])) { CHECK(0, "can't write %s", paths[i]); return; }
    err = engine_load(paths[i], 0.0);
    CHECK(err == NULL, "load #%d should succeed, got: %s", i, err);
    free(err);
  }

  CHECK(engine_active_count() == 12, "12 voices should be active, got %d", engine_active_count());

  /* Stop all 12 and force their callback-side fade-out to complete via
   * the test helper (no audio thread runs in this test harness). */
  for (int i = 0; i < 12; i++) {
    snprintf(name, sizeof name, "aither_test_slot_%d", i);
    err = engine_stop(name, 0.0);
    CHECK(err == NULL, "stop %s: %s", name, err);
    free(err);
    CHECK(engine_mark_stopped(name), "mark_stopped %s found nothing", name);
  }

  CHECK(engine_active_count() == 0,
        "after force-stopping all 12, active count should be 0, got %d", engine_active_count());

  /* Now load a NEW voice. The sweep should recycle the stopped slots so
   * this succeeds. Pre-fix it returned "voice limit reached" because
   * the slot count stayed at 12 forever. */
  char new_path[4200];
  if (write_patch("aither_test_slot_new", new_path)) { CHECK(0, "can't write %s", new_path); return; }
  err = engine_load(new_path, 0.0);
  CHECK(err == NULL, "new voice after stopping 12 should succeed, got: %s", err);
  free(err);

  VoiceSnap* v;
  int n = engine_voices(&v);
  char* listing = cli_voice_list(midi_last_connect_info(), midi_subscription_active(), v, n);
  engine_free_voices(v, n);
  CHECK(strstr(listing, "aither_test_slot_new"), "new voice should appear in list: %s", listing);
  CHECK(!strstr(listing, "aither_test_slot_0"), "stopped voices should be swept from list: %s", listing);
  free(listing);

  for (int i = 0; i < 12; i++) unlink(paths[i]);
  unlink(new_path);
  rmdir(dir);
}
