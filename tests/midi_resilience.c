/* MIDI subscription bookkeeping smoke test. Real ALSA-loop tests
 * need hardware, but the no-connect paths and the voice list's header
 * format can be verified without a sequencer.
 *
 * Pre-fix: the engine had no notion of "subscription dropped" — when
 * the input thread died mid-session (issue 4c) the operator just
 * reached for the keys, heard nothing, and had to guess whether the
 * patch was wrong or MIDI was gone. This test pins the no-MIDI path
 * (silent in `list`, no false "DROPPED" line) and the recovery API
 * contract (NULL when nothing to do). */

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

TEST(midi_resilience) {
  /* Before any connect, the resubscribe call should be a no-op. */
  char* re = midi_resubscribe_if_dropped();
  CHECK(re == NULL, "no-connect resubscribe should return nothing, got %s", re);
  free(re);

  /* And the subscription should be inactive (we never started a thread). */
  CHECK(!midi_subscription_active(), "subscription should be inactive before any start_thread");

  /* `list` output with no voices and no MIDI should be the literal
   * "(no voices)" — adding the MIDI header in the no-connect case
   * would clutter the output. */
  char* before = listing();
  CHECK(!strstr(before, "MIDI:"),
        "list should not show MIDI header when nothing was connected: %s", before);
  free(before);

  /* Load a voice → list output now mentions the voice but still no MIDI. */
  const char* tmp = getenv("TMPDIR");
  char dir[4096];
  snprintf(dir, sizeof dir, "%s/aither_test_XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(dir)) { CHECK(0, "mkdtemp %s failed", dir); return; }
  char path[4200];
  snprintf(path, sizeof path, "%s/aither_test_midi_resilience.aither", dir);
  FILE* f = fopen(path, "w");
  if (!f) { CHECK(0, "can't write %s", path); return; }
  fputs("play tone:\n"
        "  sin(TAU * phasor(440)) * 0.05\n"
        "tone\n", f);
  fclose(f);

  char* err = engine_load(path, 0.0);
  CHECK(err == NULL, "load failed: %s", err);
  free(err);
  char* after = listing();
  CHECK(!strstr(after, "MIDI:"), "MIDI header still hidden when never connected: %s", after);
  CHECK(strstr(after, "aither_test_midi_resilience"), "loaded voice should appear: %s", after);
  free(after);

  unlink(path);
  rmdir(dir);
}
