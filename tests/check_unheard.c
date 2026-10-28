/* `check`, `parts` and `audit` say when a part plays and isn't heard.
 *
 * Static: a part the final expression never reaches (directly, or
 * through another part, or inside a call) is NOT IN THE FINAL
 * EXPRESSION, known at compile time. No final expression: every part
 * is heard. Dynamic: the loudest heard part is healthy but the voice's
 * output is 30 dB or more below it: QUIET VOICE. */

#define _GNU_SOURCE
#include <unistd.h>
#include "check.h"

/* The plays a patch's output never reaches, joined with spaces (as the
 * compiler sees it: with the stdlib, inline). "!" if it doesn't compile. */
static char* unheard_of(const char* src) {
  char err[4096];
  Generated g;
  char* out;
  if (compile_full(src, &g, err, sizeof err) != 0) {
    CHECK(0, "doesn't compile: %s", err);
    out = strdup("!");
  } else {
    size_t len = 1;
    for (int i = 0; i < g.nunheard; i++) len += strlen(g.unheard[i]) + 1;
    out = calloc(len, 1);
    for (int i = 0; i < g.nunheard; i++) {
      if (i) strcat(out, " ");
      strcat(out, g.unheard[i]);
    }
  }
  return out;
}

static void unheard_is(const char* src, const char* want) {
  char* got = unheard_of(src);
  CHECK(strcmp(got, want) == 0, "unheard of\n%s\nis '%s', wanted '%s'", src, got, want);
  free(got);
}

TEST(check_unheard) {
  /* Forgotten on the last line. */
  unheard_is("play drone:\n  0.1\nplay sticks:\n  0.2\n\ndrone\n", "sticks");
  /* Reached through another part, and inside a call. */
  unheard_is("play drone:\n  0.1\nplay sticks:\n  0.2\nplay mix:\n  drone + sticks\n\nabs(mix)\n", "");
  /* No final expression: all parts play. */
  unheard_is("play drone:\n  0.1\nplay sticks:\n  0.2\n", "");
  /* A name that is a let, not a part, doesn't count as reaching a part. */
  unheard_is("play drone:\n  0.1\nlet sticks = 0.2\n\ndrone + sticks\n", "");

  /* audit's part table carries the flag. */
  {
    const char* tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/aither_unheard_%d.aither", tmp && *tmp ? tmp : "/tmp",
             (int)getpid());
    FILE* f = fopen(path, "w");
    fputs("play drone:\n  0.1\nplay sticks:\n  0.2\n\ndrone\n", f);
    fclose(f);
    char* src = slurp(path);
    Rendered r;
    char err[4096];
    int rc = render_source(aither_stdlib, src, path, 0.01, 48000, NULL, NULL, 0, 0.0, &r, err,
                           sizeof err);
    unlink(path);
    free(src);
    CHECK(rc == 0, "doesn't render: %s", err);
    if (rc == 0) {
      int sticks_heard = 1;
      for (int i = 0; i < r.nparts; i++)
        if (strcmp(r.parts[i].name, "sticks") == 0) sticks_heard = !r.parts[i].unheard;
      CHECK(!sticks_heard, "audit should flag sticks as unheard");
      rendered_free(&r);
    }
  }

  /* The dynamic warning in check's report. */
  {
    PartSnap parts[2] = {
      {.name = "drone", .state = PS_PLAYING, .gain = 1.0, .rms = 0.3, .peak = 0.4, .unheard = 0},
      {.name = "sticks", .state = PS_PLAYING, .gain = 1.0, .rms = 0.2, .peak = 0.3, .unheard = 1},
    };
    CheckSnap q = {
      .found = 1,
      .voice = {.name = "v", .state = VS_PLAYING, .gain = 1.0,
                .stats = {.rms_l = 0.001, .rms_r = 0.001, .peak_l = 0.001, .peak_r = 0.001}},
      .parts = parts, .nparts = 1,
    };
    char* s = cli_check(&q);
    CHECK(strstr(s, "QUIET VOICE"), "%s", s);
    free(s);
    /* A healthy voice says nothing. */
    CheckSnap ok = q;
    ok.voice.stats = (StatsSnap){.rms_l = 0.3, .rms_r = 0.3, .peak_l = 0.4, .peak_r = 0.4};
    s = cli_check(&ok);
    CHECK(!strstr(s, "QUIET VOICE"), "%s", s);
    free(s);
    /* The static note. */
    CheckSnap missing = q;
    missing.nparts = 2;
    s = cli_check(&missing);
    CHECK(strstr(s, "NOT IN THE FINAL EXPRESSION (computed, never heard): sticks"), "%s", s);
    free(s);
    PartLevel lv = {.name = "sticks", .rms = 0.2, .peak = 0.3, .gain = 1.0, .unheard = 1};
    s = cli_part_levels(&lv, 1);
    CHECK(strstr(s, "NOT IN THE FINAL EXPRESSION"), "%s", s);
    free(s);
  }
}
